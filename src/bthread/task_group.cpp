// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// bthread - An M:N threading library to make applications more concurrent.

// Date: Tue Jul 10 17:40:58 CST 2012

#include <sys/types.h>
#include <stddef.h>                         // size_t
#include <gflags/gflags.h>
#include "butil/compat.h"                   // OS_MACOSX
#include "butil/macros.h"                   // ARRAY_SIZE
#include "butil/scoped_lock.h"              // BAIDU_SCOPED_LOCK
#include "butil/fast_rand.h"
#include "butil/unique_ptr.h"
#include "butil/third_party/murmurhash3/murmurhash3.h" // fmix64
#include "bthread/errno.h"                  // ESTOP
#include "bthread/butex.h"                  // butex_*
#include "bthread/sys_futex.h"              // futex_wake_private
#include "bthread/processor.h"              // cpu_relax
#include "bthread/task_control.h"
#include "bthread/task_group.h"
#include "bthread/timer_thread.h"
#include "bthread/errno.h"
#include "task_meta.h"
#include "bthread/eloq_module.h"
#ifdef IO_URING_ENABLED
#include <liburing.h>
#include "ring_write_buf_pool.h"
#include "bthread/ring_listener.h"
#endif

std::array<eloq::EloqModule *, 10> registered_modules;
std::atomic<int> registered_module_cnt;

DEFINE_int32(steal_task_rnd, 100, "Steal task frequency in wait_task");
DEFINE_bool(brpc_worker_as_ext_processor, false, "Work as external processor");
DECLARE_bool(use_io_uring);

namespace bthread {

static const bthread_attr_t BTHREAD_ATTR_TASKGROUP = {
    BTHREAD_STACKTYPE_UNKNOWN, 0, NULL };

static bool pass_bool(const char*, bool) { return true; }

DEFINE_bool(show_bthread_creation_in_vars, false, "When this flags is on, The time "
            "from bthread creation to first run will be recorded and shown "
            "in /vars");
const bool ALLOW_UNUSED dummy_show_bthread_creation_in_vars =
    ::GFLAGS_NS::RegisterFlagValidator(&FLAGS_show_bthread_creation_in_vars,
                                    pass_bool);

DEFINE_bool(show_per_worker_usage_in_vars, false,
            "Show per-worker usage in /vars/bthread_per_worker_usage_<tid>");
const bool ALLOW_UNUSED dummy_show_per_worker_usage_in_vars =
    ::GFLAGS_NS::RegisterFlagValidator(&FLAGS_show_per_worker_usage_in_vars,
                                    pass_bool);

DEFINE_int32(worker_polling_time_us, 0, "Worker keep busy polling some time before "
                                       "sleeping on parking lot");

BAIDU_VOLATILE_THREAD_LOCAL(TaskGroup*, tls_task_group, NULL);
// Sync with TaskMeta::local_storage when a bthread is created or destroyed.
// During running, the two fields may be inconsistent, use tls_bls as the
// groundtruth.
__thread LocalStorage tls_bls = BTHREAD_LOCAL_STORAGE_INITIALIZER;

// defined in bthread/key.cpp
extern void return_keytable(bthread_keytable_pool_t*, KeyTable*);

// [Hacky] This is a special TLS set by bthread-rpc privately... to save
// overhead of creation keytable, may be removed later.
BAIDU_VOLATILE_THREAD_LOCAL(void*, tls_unique_user_ptr, NULL);

const TaskStatistics EMPTY_STAT = { 0, 0 };

const size_t OFFSET_TABLE[] = {
#include "bthread/offset_inl.list"
};

int TaskGroup::get_attr(bthread_t tid, bthread_attr_t* out) {
    TaskMeta* const m = address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            *out = m->attr;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

void TaskGroup::set_stopped(bthread_t tid) {
    TaskMeta* const m = address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            m->stop = true;
        }
    }
}

bool TaskGroup::is_stopped(bthread_t tid) {
    TaskMeta* const m = address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            return m->stop;
        }
    }
    // If the tid does not exist or version does not match, it's intuitive
    // to treat the thread as "stopped".
    return true;
}

bool TaskGroup::wait_task(bthread_t* tid) {
    int64_t poll_start_us = FLAGS_worker_polling_time_us > 0 ? butil::cpuwide_time_us() : 0;
    int empty_rnd = 0;
    do {
#ifndef BTHREAD_DONT_SAVE_PARKING_STATE
        if (_last_pl_state.stopped()) {

            NotifyRegisteredModules(WorkerStatus::Sleep);

          return false;
        }

#ifdef IO_URING_ENABLED
        // if (FLAGS_use_io_uring && ring_listener_ != nullptr) {
        //     ring_listener_->SubmitAll();
        // }
#endif

        ProcessModulesTask();

#ifdef IO_URING_ENABLED
        // if (FLAGS_use_io_uring && ring_listener_ != nullptr) {
        //     size_t ring_poll_cnt = ring_listener_->ExtPoll();
        // }
#endif

        if (_rq.pop(tid) || _bound_rq.pop(tid) || _remote_rq.pop(tid)) {
            _processed_tasks++;
            return true;
        }
#ifdef IO_URING_ENABLED
        // if (cnt > 0 || ring_poll_cnt > 0) {
        //     empty_rnd = 0;
        //     continue;
        // }
#endif

        if (empty_rnd % FLAGS_steal_task_rnd == 0 && steal_from_others(tid)) {
            return true;
        }
        if (empty_rnd == 0) {
            // Empty rounds start counting.
            poll_start_us =
                FLAGS_worker_polling_time_us > 0 ? butil::cpuwide_time_us() : 0;
        }

        empty_rnd++;

        // if ((empty_rnd & 1023) != 0) {
        //     // For every 1024 rounds, checks if the worker thread should sleep.
        //     continue;
        // }

        // keep polling for some time before waiting on parking lot
        if (FLAGS_worker_polling_time_us <= 0 ||
            butil::cpuwide_time_us() - poll_start_us > FLAGS_worker_polling_time_us) {
            if (!HasTasks()) {
#ifdef IO_URING_ENABLED
                if (FLAGS_use_io_uring && ring_listener_ != nullptr) {
                    ring_listener_->ExtWakeup();
                }
#endif
                NotifyRegisteredModules(WorkerStatus::Sleep);

                Wait();

                NotifyRegisteredModules(WorkerStatus::Working);
            }
            poll_start_us = FLAGS_worker_polling_time_us > 0 ? butil::cpuwide_time_us() : 0;
            empty_rnd = 0;
        }
#else
        const ParkingLot::State st = _pl->get_state();
        if (st.stopped()) {
            return false;
        }
        if (steal_task(tid)) {
            return true;
        }
        _pl->wait(st);
#endif
    } while (true);
}

static double get_cumulated_cputime_from_this(void* arg) {
    return static_cast<TaskGroup*>(arg)->cumulated_cputime_ns() / 1000000000.0;
}

void TaskGroup::run_main_task() {
    bvar::PassiveStatus<double> cumulated_cputime(
        get_cumulated_cputime_from_this, this);
    std::unique_ptr<bvar::PerSecond<bvar::PassiveStatus<double> > > usage_bvar;

    TaskGroup* dummy = this;
    bthread_t tid;
    while (wait_task(&tid)) {
        TaskGroup::sched_to(&dummy, tid);
        DCHECK_EQ(this, dummy);
        DCHECK_EQ(_cur_meta->stack, _main_stack);
        if (_cur_meta->tid != _main_tid) {
            TaskGroup::task_runner(1/*skip remained*/);
        }
        if (FLAGS_show_per_worker_usage_in_vars && !usage_bvar) {
            char name[32];
#if defined(OS_MACOSX)
            snprintf(name, sizeof(name), "bthread_worker_usage_%" PRIu64,
                     pthread_numeric_id());
#else
            snprintf(name, sizeof(name), "bthread_worker_usage_%ld",
                     (long)syscall(SYS_gettid));
#endif
            usage_bvar.reset(new bvar::PerSecond<bvar::PassiveStatus<double> >
                             (name, &cumulated_cputime, 1));
        }
    }
    // Don't forget to add elapse of last wait_task.
    current_task()->stat.cputime_ns += butil::cpuwide_time_ns() - _last_run_ns;
}

TaskGroup::TaskGroup(TaskControl* c)
    :
    _cur_meta(NULL)
    , _control(c)
    , _num_nosignal(0)
    , _nsignaled(0)
    , _last_run_ns(butil::cpuwide_time_ns())
    , _cumulated_cputime_ns(0)
    , _nswitch(0)
    , _last_context_remained(NULL)
    , _last_context_remained_arg(NULL)
    , _pl(NULL)
    , _main_stack(NULL)
    , _main_tid(0)
    , _remote_num_nosignal(0)
    , _remote_nsignaled(0)
#ifndef NDEBUG
    , _sched_recursive_guard(0)
#endif
{
    _steal_seed = butil::fast_rand();
    _steal_offset = OFFSET_TABLE[_steal_seed % ARRAY_SIZE(OFFSET_TABLE)];
    CHECK(c);
    _bound_rq.is_bound_queue = true;
}

TaskGroup::~TaskGroup() {
    if (_main_tid) {
        TaskMeta* m = address_meta(_main_tid);
        CHECK(_main_stack == m->stack);
        return_stack(m->release_stack());
        return_resource(get_slot(_main_tid));
        _main_tid = 0;
    }
}

int TaskGroup::init(size_t runqueue_capacity) {
    if (_rq.init(runqueue_capacity) != 0) {
        LOG(FATAL) << "Fail to init _rq";
        return -1;
    }
    if (_remote_rq.init(runqueue_capacity / 2) != 0) {
        LOG(FATAL) << "Fail to init _remote_rq";
        return -1;
    }
    ContextualStack* stk = get_stack(STACK_TYPE_MAIN, NULL);
    if (NULL == stk) {
        LOG(FATAL) << "Fail to get main stack container";
        return -1;
    }
    butil::ResourceId<TaskMeta> slot;
    TaskMeta* m = butil::get_resource<TaskMeta>(&slot);
    if (NULL == m) {
        LOG(FATAL) << "Fail to get TaskMeta";
        return -1;
    }
    m->stop = false;
    m->interrupted = false;
    m->about_to_quit = false;
    m->fn = NULL;
    m->arg = NULL;
    m->local_storage = LOCAL_STORAGE_INIT;
    m->cpuwide_start_ns = butil::cpuwide_time_ns();
    m->stat = EMPTY_STAT;
    m->attr = BTHREAD_ATTR_TASKGROUP;
    m->tid = make_tid(*m->version_butex, slot);
    m->set_stack(stk);

    _cur_meta = m;
    _main_tid = m->tid;
    _main_stack = stk;
    _last_run_ns = butil::cpuwide_time_ns();
#ifdef IO_URING_ENABLED
    if (FLAGS_use_io_uring) {
        ring_listener_ = std::make_unique<RingListener>(this);
        int ret = ring_listener_->Init();
        if (ret) {
            LOG(ERROR) << "Failed to initialize the IO uring listener.";
            ring_listener_ = nullptr;
        }
    }
#endif
    return 0;
}

void TaskGroup::task_runner(intptr_t skip_remained) {
    // NOTE: tls_task_group is volatile since tasks are moved around
    //       different groups.
    TaskGroup* g = tls_task_group;

    if (!skip_remained) {
        while (g->_last_context_remained) {
            RemainedFn fn = g->_last_context_remained;
            g->_last_context_remained = NULL;
            fn(g->_last_context_remained_arg);
            g = BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
        }

#ifndef NDEBUG
        --g->_sched_recursive_guard;
#endif
    }

    do {
        // A task can be stopped before it gets running, in which case
        // we may skip user function, but that may confuse user:
        // Most tasks have variables to remember running result of the task,
        // which is often initialized to values indicating success. If an
        // user function is never called, the variables will be unchanged
        // however they'd better reflect failures because the task is stopped
        // abnormally.

        // Meta and identifier of the task is persistent in this run.
        TaskMeta* const m = g->_cur_meta;

        if (FLAGS_show_bthread_creation_in_vars) {
            // NOTE: the thread triggering exposure of pending time may spend
            // considerable time because a single bvar::LatencyRecorder
            // contains many bvar.
            g->_control->exposed_pending_time() <<
                (butil::cpuwide_time_ns() - m->cpuwide_start_ns) / 1000L;
        }

        // Not catch exceptions except ExitException which is for implementing
        // bthread_exit(). User code is intended to crash when an exception is
        // not caught explicitly. This is consistent with other threading
        // libraries.
        void* thread_return;
        try {
            thread_return = m->fn(m->arg);
        } catch (ExitException& e) {
            thread_return = e.value();
        }

        // Group is probably changed
        g =  BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);

        // TODO: Save thread_return
        (void)thread_return;

        // Logging must be done before returning the keytable, since the logging lib
        // use bthread local storage internally, or will cause memory leak.
        // FIXME: the time from quiting fn to here is not counted into cputime
        if (m->attr.flags & BTHREAD_LOG_START_AND_FINISH) {
            LOG(INFO) << "Finished bthread " << m->tid << ", cputime="
                      << m->stat.cputime_ns / 1000000.0 << "ms";
        }

        // Clean tls variables, must be done before changing version_butex
        // otherwise another thread just joined this thread may not see side
        // effects of destructing tls variables.
        KeyTable* kt = tls_bls.keytable;
        if (kt != NULL) {
            return_keytable(m->attr.keytable_pool, kt);
            // After deletion: tls may be set during deletion.
            tls_bls.keytable = NULL;
            m->local_storage.keytable = NULL; // optional
        }

        // Increase the version and wake up all joiners, if resulting version
        // is 0, change it to 1 to make bthread_t never be 0. Any access
        // or join to the bthread after changing version will be rejected.
        // The spinlock is for visibility of TaskGroup::get_attr.
        {
            BAIDU_SCOPED_LOCK(m->version_lock);
            if (0 == ++*m->version_butex) {
                ++*m->version_butex;
            }
        }
        butex_wake_except(m->version_butex, 0);

        g->_control->_nbthreads << -1;
        g->set_remained(TaskGroup::_release_last_context, m);
        ending_sched(&g);

    } while (g->_cur_meta->tid != g->_main_tid);

    // Was called from a pthread and we don't have BTHREAD_STACKTYPE_PTHREAD
    // tasks to run, quit for more tasks.
}

void TaskGroup::_release_last_context(void* arg) {
    TaskMeta* m = static_cast<TaskMeta*>(arg);
    if (m->stack_type() != STACK_TYPE_PTHREAD) {
        return_stack(m->release_stack()/*may be NULL*/);
        m->SetBoundGroup(nullptr);
    } else {
        // it's _main_stack, don't return.
        m->set_stack(NULL);
    }
    return_resource(get_slot(m->tid));
}

int TaskGroup::start_foreground(TaskGroup** pg,
                                bthread_t* __restrict th,
                                const bthread_attr_t* __restrict attr,
                                void * (*fn)(void*),
                                void* __restrict arg) {
    if (__builtin_expect(!fn, 0)) {
        return EINVAL;
    }
    const int64_t start_ns = butil::cpuwide_time_ns();
    const bthread_attr_t using_attr = (attr ? *attr : BTHREAD_ATTR_NORMAL);
    butil::ResourceId<TaskMeta> slot;
    TaskMeta* m = butil::get_resource(&slot);
    if (__builtin_expect(!m, 0)) {
        return ENOMEM;
    }
    CHECK(m->current_waiter.load(butil::memory_order_relaxed) == NULL);
    m->stop = false;
    m->interrupted = false;
    m->about_to_quit = false;
    m->fn = fn;
    m->arg = arg;
    CHECK(m->stack == NULL);
    m->attr = using_attr;
    m->local_storage = LOCAL_STORAGE_INIT;
    if (using_attr.flags & BTHREAD_INHERIT_SPAN) {
        m->local_storage.rpcz_parent_span = tls_bls.rpcz_parent_span;
    }
    m->cpuwide_start_ns = start_ns;
    m->stat = EMPTY_STAT;
    m->tid = make_tid(*m->version_butex, slot);
    m->SetBoundGroup(nullptr);
    *th = m->tid;
    if (using_attr.flags & BTHREAD_LOG_START_AND_FINISH) {
        LOG(INFO) << "Started bthread " << m->tid;
    }

    TaskGroup* g = *pg;
    g->_control->_nbthreads << 1;
    if (g->is_current_pthread_task()) {
        // never create foreground task in pthread.
        g->ready_to_run(m->tid, (using_attr.flags & BTHREAD_NOSIGNAL));
    } else {
        // NOSIGNAL affects current task, not the new task.
        RemainedFn fn = NULL;
        TaskMeta* current_task = g->current_task();
        if (current_task->bound_task_group) {
            CHECK(current_task->bound_task_group == g);
            fn = ready_to_run_in_target_worker_bound;
            ReadyToRunTargetArgs args = {g->current_tid(), false, g};
            g->set_remained(fn, &args);
            TaskGroup::sched_to(pg, m->tid);
        } else {
            if (g->current_task()->about_to_quit) {
                fn = ready_to_run_in_worker_ignoresignal;
            } else {
                fn = ready_to_run_in_worker;
            }
            ReadyToRunArgs args = {
                g->current_tid(),
                (bool)(using_attr.flags & BTHREAD_NOSIGNAL)
            };
            g->set_remained(fn, &args);
            TaskGroup::sched_to(pg, m->tid);
        }
    }
    return 0;
}

template <bool REMOTE>
int TaskGroup::start_background(bthread_t* __restrict th,
                                const bthread_attr_t* __restrict attr,
                                void * (*fn)(void*),
                                void* __restrict arg,
                                bool is_bound) {
    if (__builtin_expect(!fn, 0)) {
        return EINVAL;
    }
    const int64_t start_ns = butil::cpuwide_time_ns();
    const bthread_attr_t using_attr = (attr ? *attr : BTHREAD_ATTR_NORMAL);
    butil::ResourceId<TaskMeta> slot;
    TaskMeta* m = butil::get_resource(&slot);
    if (__builtin_expect(!m, 0)) {
        return ENOMEM;
    }
    CHECK(m->current_waiter.load(butil::memory_order_relaxed) == NULL);
    m->stop = false;
    m->interrupted = false;
    m->about_to_quit = false;
    m->fn = fn;
    m->arg = arg;
    CHECK(m->stack == NULL);
    m->attr = using_attr;
    m->local_storage = LOCAL_STORAGE_INIT;
    if (using_attr.flags & BTHREAD_INHERIT_SPAN) {
        m->local_storage.rpcz_parent_span = tls_bls.rpcz_parent_span;
    }
    m->cpuwide_start_ns = start_ns;
    m->stat = EMPTY_STAT;
    m->tid = make_tid(*m->version_butex, slot);
    m->SetBoundGroup(nullptr);
    *th = m->tid;
    if (using_attr.flags & BTHREAD_LOG_START_AND_FINISH) {
        LOG(INFO) << "Started bthread " << m->tid;
    }
    _control->_nbthreads << 1;
    if (REMOTE) {
        if (is_bound) {
            ready_to_run_bound(m->tid);
        } else {
            ready_to_run_remote(m->tid, (using_attr.flags & BTHREAD_NOSIGNAL));
        }
    } else {
        ready_to_run(m->tid, (using_attr.flags & BTHREAD_NOSIGNAL));
    }
    return 0;
}

// Explicit instantiations.
template int
TaskGroup::start_background<true>(bthread_t* __restrict th,
                                  const bthread_attr_t* __restrict attr,
                                  void * (*fn)(void*),
                                  void* __restrict arg,
                                  bool is_bound = false);
template int
TaskGroup::start_background<false>(bthread_t* __restrict th,
                                   const bthread_attr_t* __restrict attr,
                                   void * (*fn)(void*),
                                   void* __restrict arg,
                                   bool is_bound = false);

int TaskGroup::start_from_dispatcher(bthread_t* __restrict th,
                                     const bthread_attr_t* __restrict attr,
                                     void * (*fn)(void*),
                                     void* __restrict arg) {
    if (__builtin_expect(!fn, 0)) {
        return EINVAL;
    }
    const int64_t start_ns = butil::cpuwide_time_ns();
    const bthread_attr_t using_attr = (attr ? *attr : BTHREAD_ATTR_NORMAL);
    butil::ResourceId<TaskMeta> slot;
    TaskMeta* m = butil::get_resource(&slot);
    if (__builtin_expect(!m, 0)) {
        return ENOMEM;
    }
    CHECK(m->current_waiter.load(butil::memory_order_relaxed) == NULL);
    m->stop = false;
    m->interrupted = false;
    m->about_to_quit = false;
    m->fn = fn;
    m->arg = arg;
    CHECK(m->stack == NULL);
    m->attr = using_attr;
    m->local_storage = LOCAL_STORAGE_INIT;
    if (using_attr.flags & BTHREAD_INHERIT_SPAN) {
        m->local_storage.rpcz_parent_span = tls_bls.rpcz_parent_span;
    }
    m->cpuwide_start_ns = start_ns;
    m->stat = EMPTY_STAT;
    m->tid = make_tid(*m->version_butex, slot);
    m->SetBoundGroup(nullptr);
    *th = m->tid;
    if (using_attr.flags & BTHREAD_LOG_START_AND_FINISH) {
        LOG(INFO) << "Started bthread " << m->tid;
    }
    _control->_nbthreads << 1;

    //    CHECK(address_meta(tid)->bound_task_group == nullptr);

    while (!_remote_rq.push(m->tid)) {
        flush_nosignal_tasks_remote();
        LOG_EVERY_SECOND(ERROR)
                << "_remote_rq is full, capacity=" << _remote_rq.capacity();
        ::usleep(1000);
    }
    // From dispatcher, should never be nosignal, at least for now.
    CHECK(!(using_attr.flags & BTHREAD_NOSIGNAL));
    _control->signal_group(group_id_);

    return 0;
}

int TaskGroup::join(bthread_t tid, void** return_value) {
    if (__builtin_expect(!tid, 0)) {  // tid of bthread is never 0.
        return EINVAL;
    }
    TaskMeta* m = address_meta(tid);
    if (__builtin_expect(!m, 0)) {
        // The bthread is not created yet, this join is definitely wrong.
        return EINVAL;
    }
    TaskGroup* g = tls_task_group;
    if (g != NULL && g->current_tid() == tid) {
        // joining self causes indefinite waiting.
        return EINVAL;
    }
    const uint32_t expected_version = get_version(tid);
    while (*m->version_butex == expected_version) {
        if (butex_wait(m->version_butex, expected_version, NULL) < 0 &&
            errno != EWOULDBLOCK && errno != EINTR) {
            return errno;
        }
    }
    if (return_value) {
        *return_value = NULL;
    }
    return 0;
}

bool TaskGroup::exists(bthread_t tid) {
    if (tid != 0) {  // tid of bthread is never 0.
        TaskMeta* m = address_meta(tid);
        if (m != NULL) {
            return (*m->version_butex == get_version(tid));
        }
    }
    return false;
}

TaskStatistics TaskGroup::main_stat() const {
    TaskMeta* m = address_meta(_main_tid);
    return m ? m->stat : EMPTY_STAT;
}

void TaskGroup::ending_sched(TaskGroup** pg) {
    TaskGroup* g = *pg;
    bthread_t next_tid = 0;
    // Find next task to run, if none, switch to idle thread of the group.
#ifndef BTHREAD_FAIR_WSQ
    // When BTHREAD_FAIR_WSQ is defined, profiling shows that cpu cost of
    // WSQ::steal() in example/multi_threaded_echo_c++ changes from 1.9%
    // to 2.9%
    const bool popped = g->_rq.pop(&next_tid);
#else
    const bool popped = g->_rq.steal(&next_tid);
#endif
    if (!popped) {
        // Yield proactively to run modules workload.
        if (g->modules_cnt_ > 0 && g->_processed_tasks > 30) {
            g->_processed_tasks = 0;
            next_tid = g->_main_tid;
        } else {
            if (!g->steal_task(&next_tid)) {
                // Jump to main task if there's no task to run.
                g->_processed_tasks = 0;
                next_tid = g->_main_tid;
            }
        }
    }

    if (next_tid != g->_main_tid) {
        g->_processed_tasks++;
    }

    TaskMeta* const cur_meta = g->_cur_meta;
    TaskMeta* next_meta = address_meta(next_tid);
    if (next_meta->stack == NULL) {
        if (next_meta->stack_type() == cur_meta->stack_type()) {
            // also works with pthread_task scheduling to pthread_task, the
            // transfered stack is just _main_stack.
            next_meta->set_stack(cur_meta->release_stack());
        } else {
            ContextualStack* stk = get_stack(next_meta->stack_type(), task_runner);
            if (stk) {
                next_meta->set_stack(stk);
            } else {
                // stack_type is BTHREAD_STACKTYPE_PTHREAD or out of memory,
                // In latter case, attr is forced to be BTHREAD_STACKTYPE_PTHREAD.
                // This basically means that if we can't allocate stack, run
                // the task in pthread directly.
                next_meta->attr.stack_type = BTHREAD_STACKTYPE_PTHREAD;
                next_meta->set_stack(g->_main_stack);
            }
        }
    }
    sched_to(pg, next_meta);
}

void TaskGroup::sched(TaskGroup** pg) {
    TaskGroup* g = *pg;
    bthread_t next_tid = 0;
    // Find next task to run, if none, switch to idle thread of the group.
#ifndef BTHREAD_FAIR_WSQ
    const bool popped = g->_rq.pop(&next_tid);
#else
    const bool popped = g->_rq.steal(&next_tid);
#endif
    if (!popped) {
        // Yield proactively to run modules workload.
        if (g->modules_cnt_ > 0 && g->_processed_tasks > 30) {
            g->_processed_tasks = 0;
            next_tid = g->_main_tid;
        } else {
            if (!g->steal_task(&next_tid)) {
              // Jump to main task if there's no task to run.
              g->_processed_tasks = 0;
              next_tid = g->_main_tid;
            } else if (next_tid == g->current_tid()) {
                if (!g->steal_task(&next_tid)) {
                    next_tid = g->_main_tid;
                    g->_processed_tasks = 0;
                }
                g->ready_to_run_bound(g->current_tid());
            }
        }
    }

    if (next_tid != g->_main_tid) {
        g->_processed_tasks++;
    }
    sched_to(pg, next_tid);
}

void TaskGroup::sched_to(TaskGroup** pg, TaskMeta* next_meta) {
    CHECK(next_meta->bound_task_group == nullptr || next_meta->bound_task_group == *pg);
    TaskGroup* g = *pg;

#ifndef NDEBUG
    if ((++g->_sched_recursive_guard) > 1) {
        LOG(FATAL) << "Recursively(" << g->_sched_recursive_guard - 1
                   << ") call sched_to(" << g << ")";
    }
#endif
    // Save errno so that errno is bthread-specific.
    const int saved_errno = errno;
    void* saved_unique_user_ptr = tls_unique_user_ptr;

    TaskMeta* const cur_meta = g->_cur_meta;
    const int64_t now = butil::cpuwide_time_ns();
    const int64_t elp_ns = now - g->_last_run_ns;
    g->_last_run_ns = now;
    cur_meta->stat.cputime_ns += elp_ns;
    if (cur_meta->tid != g->main_tid()) {
        g->_cumulated_cputime_ns += elp_ns;
    }
    ++cur_meta->stat.nswitch;
    ++ g->_nswitch;
    // Switch to the task
    if (__builtin_expect(next_meta != cur_meta, 1)) {
        g->_cur_meta = next_meta;
        // Switch tls_bls
        cur_meta->local_storage = tls_bls;
        tls_bls = next_meta->local_storage;

        // Logging must be done after switching the local storage, since the logging lib
        // use bthread local storage internally, or will cause memory leak.
        if ((cur_meta->attr.flags & BTHREAD_LOG_CONTEXT_SWITCH) ||
            (next_meta->attr.flags & BTHREAD_LOG_CONTEXT_SWITCH)) {
            LOG(INFO) << "Switch bthread: " << cur_meta->tid << " -> "
                      << next_meta->tid;
        }

        if (cur_meta->stack != NULL) {
            if (next_meta->stack != cur_meta->stack) {
                jump_stack(cur_meta->stack, next_meta->stack);
                // probably went to another group, need to assign g again.
                g = BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
            }
#ifndef NDEBUG
            else {
                // else pthread_task is switching to another pthread_task, sc
                // can only equal when they're both _main_stack
                CHECK(cur_meta->stack == g->_main_stack);
            }
#endif
        }
        // else because of ending_sched(including pthread_task->pthread_task)
    } else {
        LOG(FATAL) << "bthread=" << g->current_tid() << " sched_to itself!";
    }

    while (g->_last_context_remained) {
        RemainedFn fn = g->_last_context_remained;
        g->_last_context_remained = NULL;
        fn(g->_last_context_remained_arg);
        g = BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
    }

    // Restore errno
    errno = saved_errno;
    // tls_unique_user_ptr probably changed.
    BAIDU_SET_VOLATILE_THREAD_LOCAL(tls_unique_user_ptr, saved_unique_user_ptr);

#ifndef NDEBUG
    --g->_sched_recursive_guard;
#endif
    *pg = g;
}

void TaskGroup::destroy_self() {
    if (_control) {
        _control->_destroy_group(this);
        _control = NULL;
    } else {
        CHECK(false);
    }
}

void TaskGroup::ready_to_run(bthread_t tid, bool nosignal) {
    push_rq(tid);
    if (nosignal) {
        ++_num_nosignal;
    } else {
        const int additional_signal = _num_nosignal;
        _num_nosignal = 0;
        _nsignaled += 1 + additional_signal;
        _control->signal_task(1 + additional_signal);
        // _control->signal_group(group_id_);
    }
}

void TaskGroup::flush_nosignal_tasks() {
    const int val = _num_nosignal;
    if (val) {
        _num_nosignal = 0;
        _nsignaled += val;
        _control->signal_task(val);
        // _control->signal_group(group_id_);
    }
}

void TaskGroup::ready_to_run_remote(bthread_t tid, bool nosignal) {
    // CHECK(address_meta(tid)->bound_task_group == nullptr);
    while (!_remote_rq.push(tid)) {
        flush_nosignal_tasks_remote();
        LOG_EVERY_SECOND(ERROR)
            << "_remote_rq is full, capacity=" << _remote_rq.capacity();
        ::usleep(1000);
    }
    if (nosignal) {
        _remote_num_nosignal.fetch_add(1, std::memory_order_release);
    } else {
        const int additional_signal =_remote_num_nosignal.load(std::memory_order_acquire);
        _remote_num_nosignal.store(0, std::memory_order_release);
        _remote_nsignaled.fetch_add(additional_signal, std::memory_order_release);
        _control->signal_task(1 + additional_signal);
        // _control->signal_group(group_id_);
    }
}

void TaskGroup::flush_nosignal_tasks_remote() {
    const int val = _remote_num_nosignal.load(std::memory_order_acquire);
    if (!val) {
        return;
    }
    _remote_num_nosignal.store(0);
    _remote_nsignaled.fetch_add(val);
    _control->signal_task(val);
    // _control->signal_group(group_id_);
}

void TaskGroup::ready_to_run_bound(bthread_t tid, bool nosignal) {
    if (!_bound_rq.push(tid)) {
        LOG(FATAL) << "fail to push bounded task into group: " << group_id_;
        return;
    }
    // Update bound group.
    TaskMeta *m = TaskGroup::address_meta(tid);
    m->SetBoundGroup(this);
    CHECK(!nosignal);
    _control->signal_group(group_id_);
}

void TaskGroup::resume_bound_task(bthread_t tid, bool nosignal) {
    if (!_bound_rq.push(tid)) {
        LOG(FATAL) << "fail to push bounded task into group: " << group_id_;
    }
    CHECK(!nosignal);
    _control->signal_group(group_id_);
}

void TaskGroup::ready_to_run_general(bthread_t tid, bool nosignal) {
    if (tls_task_group == this) {
        return ready_to_run(tid, nosignal);
    }
    return ready_to_run_remote(tid, nosignal);
}

void TaskGroup::flush_nosignal_tasks_general() {
    if (tls_task_group == this) {
        return flush_nosignal_tasks();
    }
    return flush_nosignal_tasks_remote();
}

void TaskGroup::ready_to_run_in_worker(void* args_in) {
    ReadyToRunArgs* args = static_cast<ReadyToRunArgs*>(args_in);
    return tls_task_group->ready_to_run(args->tid, args->nosignal);
}

void TaskGroup::ready_to_run_in_target_worker_bound(void* args_in) {
    ReadyToRunTargetArgs* args = static_cast<ReadyToRunTargetArgs*>(args_in);
    CHECK(args->target_group != nullptr);
    args->target_group->ready_to_run_bound(args->tid, args->nosignal);
}

void TaskGroup::ready_to_run_in_worker_ignoresignal(void* args_in) {
    ReadyToRunArgs* args = static_cast<ReadyToRunArgs*>(args_in);
    return tls_task_group->push_rq(args->tid);
}

void TaskGroup::empty_callback(void *) {
    return;
}

struct SleepArgs {
    uint64_t timeout_us;
    bthread_t tid;
    TaskMeta* meta;
    TaskGroup* group;
};

static void ready_to_run_from_timer_thread(void* arg) {
    CHECK(tls_task_group == NULL);
    const SleepArgs* e = static_cast<const SleepArgs*>(arg);
    // If the task is bound to some group. Put it back to the bound queue.
    if (e->meta->bound_task_group != nullptr) {
        e->meta->bound_task_group->resume_bound_task(e->tid);
    } else {
        e->group->control()->choose_one_group()->ready_to_run_remote(e->tid);
    }
}

void TaskGroup::_add_sleep_event(void* void_args) {
    // Must copy SleepArgs. After calling TimerThread::schedule(), previous
    // thread may be stolen by a worker immediately and the on-stack SleepArgs
    // will be gone.
    SleepArgs e = *static_cast<SleepArgs*>(void_args);
    TaskGroup* g = e.group;

    TimerThread::TaskId sleep_id;
    sleep_id = get_global_timer_thread()->schedule(
        ready_to_run_from_timer_thread, void_args,
        butil::microseconds_from_now(e.timeout_us));

    if (!sleep_id) {
        // fail to schedule timer, go back to previous thread.
        if (e.meta->bound_task_group) {
            e.meta->bound_task_group->resume_bound_task(e.tid);
        } else {
            g->ready_to_run(e.tid);
        }
        return;
    }

    // Set TaskMeta::current_sleep which is for interruption.
    const uint32_t given_ver = get_version(e.tid);
    {
        BAIDU_SCOPED_LOCK(e.meta->version_lock);
        if (given_ver == *e.meta->version_butex && !e.meta->interrupted) {
            e.meta->current_sleep = sleep_id;
            return;
        }
    }
    // The thread is stopped or interrupted.
    // interrupt() always sees that current_sleep == 0. It will not schedule
    // the calling thread. The race is between current thread and timer thread.
    if (get_global_timer_thread()->unschedule(sleep_id) == 0) {
        // added to timer, previous thread may be already woken up by timer and
        // even stopped. It's safe to schedule previous thread when unschedule()
        // returns 0 which means "the not-run-yet sleep_id is removed". If the
        // sleep_id is running(returns 1), ready_to_run_in_worker() will
        // schedule previous thread as well. If sleep_id does not exist,
        // previous thread is scheduled by timer thread before and we don't
        // have to do it again.
        if (e.meta->bound_task_group) {
            e.meta->bound_task_group->resume_bound_task(e.tid);
        } else {
            g->ready_to_run(e.tid);
        }
    }
}

// To be consistent with sys_usleep, set errno and return -1 on error.
int TaskGroup::usleep(TaskGroup** pg, uint64_t timeout_us) {
    if (0 == timeout_us) {
        yield(pg);
        return 0;
    }
    TaskGroup* g = *pg;
    // We have to schedule timer after we switched to next bthread otherwise
    // the timer may wake up(jump to) current still-running context.
    SleepArgs e = { timeout_us, g->current_tid(), g->current_task(), g };
    g->set_remained(_add_sleep_event, &e);
    sched(pg);
    g = *pg;
    e.meta->current_sleep = 0;
    if (e.meta->interrupted) {
        // Race with set and may consume multiple interruptions, which are OK.
        e.meta->interrupted = false;
        // NOTE: setting errno to ESTOP is not necessary from bthread's
        // pespective, however many RPC code expects bthread_usleep to set
        // errno to ESTOP when the thread is stopping, and print FATAL
        // otherwise. To make smooth transitions, ESTOP is still set instead
        // of EINTR when the thread is stopping.
        errno = (e.meta->stop ? ESTOP : EINTR);
        return -1;
    }
    return 0;
}

// Defined in butex.cpp
bool erase_from_butex_because_of_interruption(ButexWaiter* bw);

static int interrupt_and_consume_waiters(
    bthread_t tid, ButexWaiter** pw, uint64_t* sleep_id) {
    TaskMeta* const m = TaskGroup::address_meta(tid);
    if (m == NULL) {
        return EINVAL;
    }
    const uint32_t given_ver = get_version(tid);
    BAIDU_SCOPED_LOCK(m->version_lock);
    if (given_ver == *m->version_butex) {
        *pw = m->current_waiter.exchange(NULL, butil::memory_order_acquire);
        *sleep_id = m->current_sleep;
        m->current_sleep = 0;  // only one stopper gets the sleep_id
        m->interrupted = true;
        return 0;
    }
    return EINVAL;
}

static int set_butex_waiter(bthread_t tid, ButexWaiter* w) {
    TaskMeta* const m = TaskGroup::address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            // Release fence makes m->interrupted visible to butex_wait
            m->current_waiter.store(w, butil::memory_order_release);
            return 0;
        }
    }
    return EINVAL;
}

// The interruption is "persistent" compared to the ones caused by signals,
// namely if a bthread is interrupted when it's not blocked, the interruption
// is still remembered and will be checked at next blocking. This designing
// choice simplifies the implementation and reduces notification loss caused
// by race conditions.
// TODO: bthreads created by BTHREAD_ATTR_PTHREAD blocking on bthread_usleep()
// can't be interrupted.
int TaskGroup::interrupt(bthread_t tid, TaskControl* c) {
    // Consume current_waiter in the TaskMeta, wake it up then set it back.
    ButexWaiter* w = NULL;
    uint64_t sleep_id = 0;
    int rc = interrupt_and_consume_waiters(tid, &w, &sleep_id);
    if (rc) {
        return rc;
    }
    // a bthread cannot wait on a butex and be sleepy at the same time.
    CHECK(!sleep_id || !w);
    if (w != NULL) {
        erase_from_butex_because_of_interruption(w);
        // If butex_wait() already wakes up before we set current_waiter back,
        // the function will spin until current_waiter becomes non-NULL.
        rc = set_butex_waiter(tid, w);
        if (rc) {
            LOG(FATAL) << "butex_wait should spin until setting back waiter";
            return rc;
        }
    } else if (sleep_id != 0) {
        if (get_global_timer_thread()->unschedule(sleep_id) == 0) {
            TaskMeta *m = TaskGroup::address_meta(tid);
            if (m->bound_task_group) {
                m->bound_task_group->resume_bound_task(tid);
            } else {
                bthread::TaskGroup* g = bthread::tls_task_group;
                if (g) {
                    g->ready_to_run(tid);
                } else {
                    if (!c) {
                        return EINVAL;
                    }
                    c->choose_one_group()->ready_to_run_remote(tid);
                }
            }
        }
    }
    return 0;
}

void TaskGroup::yield(TaskGroup** pg) {
    TaskGroup* g = *pg;
    TaskMeta* m = g->current_task();
    if (m->bound_task_group) {
        CHECK(m->bound_task_group == g);
        ReadyToRunTargetArgs args = { g->current_tid(), false, m->bound_task_group };
        g->set_remained(ready_to_run_in_target_worker_bound, &args);
        sched(pg);
    }
    else {
        ReadyToRunArgs args = { g->current_tid(), false };
        g->set_remained(ready_to_run_in_worker, &args);
        sched(pg);
    }
}

void TaskGroup::jump_group(TaskGroup **pg, int target_gid) {
    TaskGroup* g = *pg;
    TaskControl *c = g->control();
    TaskGroup *target_group = c->select_group(target_gid);
    if (target_group == nullptr) {
        return;
    }
    ReadyToRunTargetArgs args = { g->current_tid(), false, target_group };
    g->set_remained(ready_to_run_in_target_worker_bound, &args);
    sched(pg);
}

void TaskGroup::block(TaskGroup **pg) {
    TaskGroup* g = *pg;
    g->set_remained(empty_callback, nullptr);
    sched(pg);
}

void print_task(std::ostream& os, bthread_t tid) {
    TaskMeta* const m = TaskGroup::address_meta(tid);
    if (m == NULL) {
        os << "bthread=" << tid << " : never existed";
        return;
    }
    const uint32_t given_ver = get_version(tid);
    bool matched = false;
    bool stop = false;
    bool interrupted = false;
    bool about_to_quit = false;
    void* (*fn)(void*) = NULL;
    void* arg = NULL;
    bthread_attr_t attr = BTHREAD_ATTR_NORMAL;
    bool has_tls = false;
    int64_t cpuwide_start_ns = 0;
    TaskStatistics stat = {0, 0};
    {
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            matched = true;
            stop = m->stop;
            interrupted = m->interrupted;
            about_to_quit = m->about_to_quit;
            fn = m->fn;
            arg = m->arg;
            attr = m->attr;
            has_tls = m->local_storage.keytable;
            cpuwide_start_ns = m->cpuwide_start_ns;
            stat = m->stat;
        }
    }
    if (!matched) {
        os << "bthread=" << tid << " : not exist now";
    } else {
        os << "bthread=" << tid << " :\nstop=" << stop
           << "\ninterrupted=" << interrupted
           << "\nabout_to_quit=" << about_to_quit
           << "\nfn=" << (void*)fn
           << "\narg=" << (void*)arg
           << "\nattr={stack_type=" << attr.stack_type
           << " flags=" << attr.flags
           << " keytable_pool=" << attr.keytable_pool
           << "}\nhas_tls=" << has_tls
           << "\nuptime_ns=" << butil::cpuwide_time_ns() - cpuwide_start_ns
           << "\ncputime_ns=" << stat.cputime_ns
           << "\nnswitch=" << stat.nswitch;
    }
}

void TaskGroup::Notify() {
    if (_waiting.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lk(_mux);
        _cv.notify_one();
    }
}

// TODO(zkl): keep notify signal and store it so that there won't be lots of lock acquiresation
bool TaskGroup::NotifyIfWaiting() {
    if (!_waiting.load(std::memory_order_relaxed)) {
        return false;
    }
    std::unique_lock<std::mutex> lk(_mux);
    _cv.notify_one();
    return true;
}

bool TaskGroup::Wait(){
    _waiting.store(true, std::memory_order_release);
    _waiting_workers.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock<std::mutex> lk(_mux);
    _cv.wait(lk, [this]()->bool {
        // No need to check _rq since _rq can only be pushed by itself.
        if (!_remote_rq.empty() || !_bound_rq.empty()) {
            return true;
        }
        bthread_t tid;
        if (steal_from_others(&tid)) {
            if (!_rq.push(tid)) {
                LOG(FATAL) << "Group: " << group_id_ << " fail to push into rq";
            }
            return true;
        }

        // Check any new module registered before checking modules' tasks.
        CheckAndUpdateModules();
        return HasTasks();
    });
    _waiting.store(false, std::memory_order_release);
    _waiting_workers.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

void TaskGroup::ProcessModulesTask() {
    int old_modules_cnt = modules_cnt_;

    CheckAndUpdateModules();

    int new_modules_cnt = modules_cnt_;
    for (int i = old_modules_cnt; i < new_modules_cnt; ++i) {
        eloq::EloqModule *module = registered_modules_[i];
        module->ExtThdStart(group_id_);
    }

    for (auto *module : registered_modules_) {
        if (module != nullptr) {
            module->Process(group_id_);
        }
    }
}

bool TaskGroup::HasTasks() {
    if (!_remote_rq.empty() || !_bound_rq.empty()) {
        return true;
    }
    bool has_task = std::any_of(registered_modules_.begin(), registered_modules_.end(),
        [this](eloq::EloqModule* module) {
            return module != nullptr && module->HasTask(group_id_);
    });

    return has_task;
}

void TaskGroup::CheckAndUpdateModules() {
    if (modules_cnt_ != registered_module_cnt.load(std::memory_order_acquire)) {
        registered_modules_ = registered_modules;
        modules_cnt_ = std::count_if(registered_modules_.begin(), registered_modules_.end(), [](eloq::EloqModule* module) {
            return module != nullptr;
        });
    }
}

void TaskGroup::NotifyRegisteredModules(WorkerStatus status) {
    for (eloq::EloqModule *module : registered_modules_) {
        if (module != nullptr) {
            if (status == WorkerStatus::Sleep) {
                module->ExtThdEnd(group_id_);
            } else {
                module->ExtThdStart(group_id_);
            }
        }
    }
}

#ifdef IO_URING_ENABLED
int TaskGroup::RegisterSocket(SocketRegisterData *data) {
    return ring_listener_->Register(data);
}

int TaskGroup::UnregisterSocket(SocketUnRegisterData *data) {
    return ring_listener_->Unregister(data);
}

void TaskGroup::SocketRecv(brpc::Socket *sock) {
  ring_listener_->SubmitRecv(sock);
}

int TaskGroup::SocketFixedWrite(brpc::Socket *sock, uint16_t ring_buf_idx, uint32_t ring_buf_size) {
  // ReAddress() increments references of the socket (same as
  // KeepWrite in background) so that the socket is still
  // available when the async IO finishes and the write request
  // is post-processed. The socket needs to be deferenced when
  // post-processing the write request.
  brpc::SocketUniquePtr ptr_for_async_write;
  sock->ReAddress(&ptr_for_async_write);

  int ret = ring_listener_->SubmitFixedWrite(sock, ring_buf_idx, ring_buf_size);
  if (ret == 0) {
    (void)ptr_for_async_write.release();
  }

  return ret;
}

int TaskGroup::SocketNonFixedWrite(brpc::Socket *sock) {
  // ReAddress() increments references of the socket (same as
  // KeepWrite in background) so that the socket is still
  // available when the async IO finishes and the write request
  // is post-processed. The socket needs to be deferenced when
  // post-processing the write request.
  brpc::SocketUniquePtr ptr_for_async_write;
  sock->ReAddress(&ptr_for_async_write);

  int ret = ring_listener_->SubmitNonFixedWrite(sock);
  if (ret == 0) {
    (void)ptr_for_async_write.release();
  }

  return ret;
}

int TaskGroup::SocketWaitingNonFixedWrite(brpc::Socket *sock) {
    int ret = ring_listener_->SubmitWaitingNonFixedWrite(sock);
    if (ret != 0) {
        LOG(FATAL) << "Submit Waiting Fixed write fails. Socket: " << *sock;
    }
    return sock->WaitForNonFixedWrite();
}

int TaskGroup::RingFsync(int fd) {
    RingFsyncData args;
    args.fd_ = fd;

    int res = ring_listener_->SubmitFsync(&args);
    if (res != 0) {
        return -1;
    }

    return args.Wait();
}

const char *TaskGroup::GetRingReadBuf(uint16_t buf_id) {
  return ring_listener_->GetReadBuf(buf_id);
}

void TaskGroup::RecycleRingReadBuf(uint16_t bid, int32_t bytes) {
  ring_listener_->RecycleReadBuf(bid, bytes);
}

std::pair<char *, uint16_t> TaskGroup::GetRingWriteBuf() {
  return ring_listener_->GetWriteBuf();
}

void TaskGroup::RecycleRingWriteBuf(uint16_t buf_idx) {
  ring_listener_->RecycleWriteBuf(buf_idx);
}

TaskGroup* TaskGroup::VolatileTLSTaskGroup() {
    return BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
}
#endif
}  // namespace bthread
