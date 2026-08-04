/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifdef IO_URING_ENABLED
#include <cstdint>
#include <errno.h>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/resource.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <gflags/gflags.h>

#include "brpc/socket.h"
#include "bthread/task_group.h"
#include "bthread/eloq_module.h"
#include "bthread/inbound_ring_buf.h"
#include "bthread/ring_write_buf_pool.h"

#include "ring_listener.h"

DEFINE_int32(io_uring_queue_entries, 1024,
             "Number of entries requested when initializing the io_uring-based "
             "inbound listener");
DEFINE_int32(io_uring_registered_files, 1024,
             "Number of sparse fixed-file slots reserved for the io_uring-based "
             "inbound listener");
DEFINE_int32(io_uring_write_buffer_pool_size, 1024,
             "Number of buffers kept in the io_uring-based write buffer pool.");

void RingListener::Close() {
    if (ring_init_) {
        io_uring_queue_exit(&ring_);
        ring_init_ = false;
    }

    if (wakeup_event_fd_ >= 0) {
        close(wakeup_event_fd_);
        wakeup_event_fd_ = -1;
    }

    if (in_buf_) {
        free(in_buf_);
        in_buf_ = nullptr;
    }
}

RingListener::~RingListener() {
    Close();
}

int RingListener::Init() {
    int32_t flag_entries = FLAGS_io_uring_queue_entries;
    if (flag_entries <= 0) {
        LOG(WARNING)
            << "FLAGS_io_uring_queue_entries must be positive, "
               "falling back to 64";
        flag_entries = 64;
    }
    const int32_t max_entries = 16384;
    if (flag_entries > max_entries) {
        LOG(WARNING) << "FLAGS_io_uring_queue_entries exceeds "
                     << max_entries << ", clamping to " << max_entries;
        flag_entries = max_entries;
    }
    const unsigned queue_entries = static_cast<unsigned>(flag_entries);

    int32_t flag_file_slots = FLAGS_io_uring_registered_files;
    if (flag_file_slots <= 0) {
        LOG(WARNING)
            << "FLAGS_io_uring_registered_files must be positive, "
               "falling back to 128";
        flag_file_slots = 128;
    }
    const int32_t max_file_slots = 128 * 1024;
    if (flag_file_slots > max_file_slots) {
        LOG(WARNING) << "FLAGS_io_uring_registered_files exceeds "
                     << max_file_slots << ", clamping to " << max_file_slots;
        flag_file_slots = max_file_slots;
    }
    const unsigned file_slots = static_cast<unsigned>(flag_file_slots);

    int32_t flag_write_buffers = FLAGS_io_uring_write_buffer_pool_size;
    if (flag_write_buffers <= 0) {
        LOG(WARNING)
            << "FLAGS_io_uring_write_buffer_pool_size must be positive, "
               "falling back to "
            << 64;
        flag_write_buffers = 64;
    }
    const int32_t max_write_buffers = 4096;
    if (flag_write_buffers > max_write_buffers) {
        LOG(WARNING) << "FLAGS_io_uring_write_buffer_pool_size exceeds "
                     << max_write_buffers << ", clamping to " << max_write_buffers;
        flag_write_buffers = max_write_buffers;
    }

    const unsigned write_buf_slots = static_cast<unsigned>(flag_write_buffers);

    unsigned ring_flags = IORING_SETUP_SINGLE_ISSUER |
                          IORING_SETUP_DEFER_TASKRUN |
                          IORING_SETUP_TASKRUN_FLAG;
    int ret = io_uring_queue_init(queue_entries, &ring_, ring_flags);

    if (ret < 0) {
        LOG(WARNING) << "Failed to initialize the IO uring of the inbound "
                  "listener, errno: "
               << ret;
        if (ret == -ENOMEM) {
            rlimit memlock_limit;
            if (getrlimit(RLIMIT_MEMLOCK, &memlock_limit) == 0) {
                std::string soft_limit = memlock_limit.rlim_cur == RLIM_INFINITY
                                             ? "unlimited"
                                             : std::to_string(static_cast<unsigned long long>(memlock_limit.rlim_cur));
                std::string hard_limit = memlock_limit.rlim_max == RLIM_INFINITY
                                             ? "unlimited"
                                             : std::to_string(static_cast<unsigned long long>(memlock_limit.rlim_max));
                LOG(WARNING) << "io_uring_queue_init returned -ENOMEM. "
                             << "Queue entries request: " << queue_entries << ". "
                             << "Current RLIMIT_MEMLOCK soft=" << soft_limit
                             << " hard=" << hard_limit
                             << ". Raise the limit (e.g. `ulimit -l` or systemd "
                             << "LimitMEMLOCK) or lower the FLAGS_io_uring_queue_entries.";
            } else {
                const int saved_errno = errno;
                LOG(WARNING) << "Failed to query RLIMIT_MEMLOCK, errno: "
                             << saved_errno;
            }
        }
        Close();
        return ret;
    }
    ring_init_ = true;

    ret = io_uring_register_files_sparse(&ring_, file_slots);
    if (ret < 0) {
        const int err = -ret;
        LOG(WARNING) << "Failed to register sparse files for the inbound listener, "
                     << "errno: " << err << " (" << strerror(err) << ")";
        if (ret == -ENOMEM) {
            rlimit memlock_limit;
            if (getrlimit(RLIMIT_MEMLOCK, &memlock_limit) == 0) {
                std::string soft_limit = memlock_limit.rlim_cur == RLIM_INFINITY
                                             ? "unlimited"
                                             : std::to_string(static_cast<unsigned long long>(memlock_limit.rlim_cur));
                std::string hard_limit = memlock_limit.rlim_max == RLIM_INFINITY
                                             ? "unlimited"
                                             : std::to_string(static_cast<unsigned long long>(memlock_limit.rlim_max));
                LOG(WARNING) << "io_uring_register_files_sparse encountered ENOMEM. "
                             << "Requested file slots: " << file_slots << ". "
                             << "Current RLIMIT_MEMLOCK soft=" << soft_limit
                             << " hard=" << hard_limit
                             << ". Raise the limit or reduce "
                                "FLAGS_io_uring_registered_files.";
            } else {
                const int saved_errno = errno;
                LOG(WARNING) << "Failed to query RLIMIT_MEMLOCK while handling "
                                "io_uring_register_files_sparse failure, errno: "
                             << saved_errno;
            }
        } else if (ret == -EMFILE) {
            LOG(WARNING) << "io_uring_register_files_sparse returned EMFILE. "
                         << "Try reducing FLAGS_io_uring_registered_files "
                            "or increasing process-open-file limits.";
        }
        Close();
        return ret;
    }

    free_reg_fd_idx_.reserve(file_slots);
    for (uint16_t f_idx = 0; f_idx < file_slots; ++f_idx) {
        free_reg_fd_idx_.emplace_back(f_idx);
    }

    in_buf_ =
            (char *) std::aligned_alloc(buf_length, buf_length * buf_ring_size);
    in_buf_ring_ = io_uring_setup_buf_ring(&ring_, buf_ring_size, 0, 0, &ret);
    if (in_buf_ring_ == nullptr) {
        LOG(WARNING) << "Failed to register buffer ring for the inbound listener.";
        Close();
        return -1;
    }

    char *ptr = in_buf_;
    // inbound_ring_size must be the power of 2.
    int br_mask = buf_ring_size - 1;
    for (size_t idx = 0; idx < buf_ring_size; idx++) {
        io_uring_buf_ring_add(in_buf_ring_, ptr, buf_length, idx, br_mask, idx);
        ptr += buf_length;
    }
    io_uring_buf_ring_advance(in_buf_ring_, buf_ring_size);

    write_buf_pool_ =
            std::make_unique<RingWriteBufferPool>(write_buf_slots, &ring_);

    if (write_buf_pool_->buf_pool_.empty()) {
        return -1;
    }

    wakeup_event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_event_fd_ < 0) {
        const int saved_errno = errno;
        LOG(ERROR) << "Failed to create the brpc worker wakeup eventfd, errno: "
                   << saved_errno << " (" << strerror(saved_errno) << ")";
        return -saved_errno;
    }
    ret = ArmEventFdPoll();
    if (ret != 0) {
        return ret;
    }
    ret = SubmitAll();
    if (ret < 0) {
        return ret;
    }

    return 0;
}

int RingListener::Register(SocketRegisterData *data) {
    brpc::Socket *sock = data->sock_;
    int fd = sock->fd();
    CHECK(fd>=0);

    auto it = reg_fds_.find(fd);
    if (it != reg_fds_.end()) {
        LOG(ERROR) << "Socket " << *sock << " has been registered before.";
        int ret = SubmitRecv(sock);
        if (ret < 0) {
            return -1;
        }
        // reg_fd already stored.
        sock->bound_g_ = task_group_;
        data->Notify(true);
        return 0;
    }

    sock->reg_fd_idx_ = -1;
    int ret = -1;

    if (free_reg_fd_idx_.empty()) {
        // All registered file slots have been taken. Cannot register the socket's
        // fd.
        ret = SubmitRecv(sock);
        if (ret < 0) {
            return -1;
        }
        reg_fds_.try_emplace(fd, -1);
        sock->bound_g_ = task_group_;
        data->Notify(true);
    } else {
        uint16_t fd_idx = free_reg_fd_idx_.back();
        free_reg_fd_idx_.pop_back();
        sock->reg_fd_ = fd;
        sock->reg_fd_idx_ = fd_idx;
        ret = SubmitRegisterFile(data, &sock->reg_fd_, fd_idx);
        if (ret < 0) {
            // Register fd fails. No sqe available.
            sock->reg_fd_ = -1;
            sock->reg_fd_idx_ = -1;
            free_reg_fd_idx_.emplace_back(fd_idx);
            return -1;
        }
        // The caller will be notified when the socket is submitted to io_uring.
    }

    return 0;
}

int RingListener::SubmitRecv(brpc::Socket *sock) {
    io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        LOG(ERROR) << "IO uring submission queue is full for the inbound "
                "listener, group: "
             << task_group_->group_id_;
        return -1;
    }
    int fd_idx = sock->reg_fd_idx_;
    int sfd = fd_idx >= 0 ? fd_idx : sock->fd();
    io_uring_prep_recv_multishot(sqe, sfd, NULL, 0, 0);
    auto *recv_data = new SocketRecvData(sock->id());
    auto data = reinterpret_cast<uint64_t>(recv_data);
    data = data << 16;
    data |= OpCodeToInt(OpCode::Recv);
    io_uring_sqe_set_data64(sqe, data);

    sqe->buf_group = 0;
    sqe->flags |= IOSQE_BUFFER_SELECT;
    if (fd_idx >= 0) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    // sqe->ioprio |= IORING_RECVSEND_BUNDLE;

    ++submit_cnt_;
    while (submit_cnt_ != 0) {
        SubmitAll();
    }
    return 0;
}

int RingListener::SubmitFixedWrite(brpc::Socket *sock, uint16_t ring_buf_idx, uint32_t ring_buf_size) {
    io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        LOG(ERROR)
            << "IO uring submission queue is full for the ring listener, group: "
            << task_group_->group_id_;
        return -1;
    }

    int fd_idx = -1;
    // Use registered index if this socket is bound to this group and ring.
    if (bthread::tls_task_group == sock->bound_g_) {
        fd_idx = sock->reg_fd_idx_;
    }
    int sfd = fd_idx >= 0 ? fd_idx : sock->fd();
    const char *write_buf = write_buf_pool_->GetBuf(ring_buf_idx);
    io_uring_prep_write_fixed(sqe, sfd, write_buf, ring_buf_size, 0, ring_buf_idx);

    uint64_t data = reinterpret_cast<uint64_t>(sock);
    data = data << 16;
    data |= OpCodeToInt(OpCode::FixedWrite);
    io_uring_sqe_set_data64(sqe, data);
    if (fd_idx >= 0) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }

    ++submit_cnt_;
    return 0;
}

int RingListener::SubmitNonFixedWrite(brpc::Socket *sock) {
    io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        LOG(ERROR)
          << "IO uring submission queue is full for the ring listener, group: "
          << task_group_->group_id_;
        return -1;
    }

    int fd_idx = -1;
    // Use registered index if this socket is bound to this group and ring.
    if (bthread::tls_task_group == sock->bound_g_) {
        fd_idx = sock->reg_fd_idx_;
    }
    int sfd = fd_idx >= 0 ? fd_idx : sock->fd();
    CHECK(sock->iovecs_.size() <= IOV_MAX);
    io_uring_prep_writev(sqe, sfd, sock->iovecs_.data(), sock->iovecs_.size(),
                         0);

    uint64_t data = reinterpret_cast<uint64_t>(sock);
    data = data << 16;
    data |= OpCodeToInt(OpCode::NonFixedWrite);
    io_uring_sqe_set_data64(sqe, data);

    if (fd_idx >= 0) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }

    ++submit_cnt_;
    return 0;
}

int RingListener::SubmitWaitingNonFixedWrite(brpc::Socket *sock) {
    io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        LOG(ERROR)
              << "IO uring submission queue is full for the ring listener, group: "
              << task_group_->group_id_;
        uint64_t data = 0;
        data |= OpCodeToInt(OpCode::WaitingNonFixedWrite);
        if (SubmitBacklog(sock, data)) {
            return 0;
        }
        return -1;
    }

    int fd_idx = -1;
    // Use registered index if this socket is bound to this group and ring.
    if (bthread::tls_task_group == sock->bound_g_) {
        fd_idx = sock->reg_fd_idx_;
    }
    int sfd = fd_idx >= 0 ? fd_idx : sock->fd();

    CHECK(sock->iovecs_.size() <= IOV_MAX);

    io_uring_prep_writev(sqe, sfd, sock->iovecs_.data(), sock->iovecs_.size(),
                         0);

    uint64_t data = reinterpret_cast<uint64_t>(sock);
    data = data << 16;
    data |= OpCodeToInt(OpCode::WaitingNonFixedWrite);
    io_uring_sqe_set_data64(sqe, data);

    if (fd_idx >= 0) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }

    ++submit_cnt_;
    return 0;
}

int RingListener::SubmitFsync(RingFsyncData *args) {
    io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        LOG(ERROR)
          << "IO uring submission queue is full for the ring listener, group: "
          << task_group_->group_id_;
        return -1;
    }

    io_uring_prep_fsync(sqe, args->fd_, 0);
    uint64_t data = reinterpret_cast<uint64_t>(args);
    data = data << 16;
    data |= OpCodeToInt(OpCode::Fsync);
    io_uring_sqe_set_data64(sqe, data);
    ++submit_cnt_;
    // Submit immediately since fsync latency matters.
    SubmitAll();
    return 0;
}

int RingListener::SubmitAll() {
    if (submit_cnt_ == 0) {
        return 0;
    }

    int ret = io_uring_submit(&ring_);
    if (ret >= 0) {
        submit_cnt_ = submit_cnt_ >= ret ? submit_cnt_ - ret : 0;
        if (submit_cnt_ != 0) {
            LOG(WARNING) << "Unable to submit all the sqes to IOuring, ret: " << ret
                << ", left: " << submit_cnt_;
        }
    } else {
        // IO uring submission failed. Wait for the next submit.
        LOG(FATAL) << "Failed to flush the IO uring submission queue for the "
                "inbound listener, ret: " << ret;
    }
    return ret;
}

int RingListener::ArmEventFdPoll() {
    io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        LOG(ERROR) << "Failed to get an SQE for the brpc worker wakeup eventfd";
        return -EAGAIN;
    }

    io_uring_prep_poll_multishot(sqe, wakeup_event_fd_, POLLIN);
    io_uring_sqe_set_data64(sqe, OpCodeToInt(OpCode::SchedulerWakeup));
    ++submit_cnt_;
    return 0;
}

void RingListener::DrainEventFd() {
    uint64_t value = 0;
    while (true) {
        const ssize_t nread = read(wakeup_event_fd_, &value, sizeof(value));
        if (nread == static_cast<ssize_t>(sizeof(value))) {
            return;
        }
        if (nread < 0 && errno == EINTR) {
            continue;
        }
        // EAGAIN means another notification consumer already drained the
        // counter. There is only one consumer today, but treating it as
        // drained keeps this helper safe if that implementation changes.
        if (nread < 0 && errno == EAGAIN) {
            return;
        }
        const int saved_errno = errno;
        LOG(FATAL) << "Failed to drain the brpc worker wakeup eventfd, errno: "
                   << saved_errno << " (" << strerror(saved_errno) << ")";
    }
}

void RingListener::NotifyEventFd() {
    const uint64_t one = 1;
    while (true) {
        const ssize_t nwritten = write(wakeup_event_fd_, &one, sizeof(one));
        if (nwritten == static_cast<ssize_t>(sizeof(one))) {
            return;
        }
        if (nwritten < 0 && errno == EINTR) {
            continue;
        }
        // A saturated eventfd is already readable, so the outstanding poll is
        // sufficient to wake the worker. Notifications are also coalesced by
        // TaskGroup::_notified, making this path exceptional.
        if (nwritten < 0 && errno == EAGAIN) {
            return;
        }
        const int saved_errno = errno;
        LOG(FATAL) << "Failed to notify the brpc worker wakeup eventfd, errno: "
                   << saved_errno << " (" << strerror(saved_errno) << ")";
    }
}

int RingListener::Park() {
    int ret;
    do {
        ret = io_uring_submit_and_wait(&ring_, 1);
    } while (ret == -EAGAIN);

    if (ret >= 0) {
        submit_cnt_ = submit_cnt_ >= ret ? submit_cnt_ - ret : 0;
        cqe_ready_.store(true, std::memory_order_relaxed);
        return 0;
    }
    // TaskControl interrupts worker pthreads during shutdown. Returning on
    // EINTR lets the scheduler observe the stopped parking-lot state.
    if (ret == -EINTR) {
        return ret;
    }
    LOG(FATAL) << "Failed while waiting on the brpc worker io_uring, ret: " << ret;
    return ret;
}

size_t RingListener::ExtPoll() {
    HandleBacklog();

    io_uring_cqe *cqe = nullptr;
    int ret = io_uring_peek_cqe(&ring_, &cqe);
    if (ret != 0) {
        return 0;
    }

    int processed = 0;
    unsigned int head;
    io_uring_for_each_cqe(&ring_, head, cqe) {
        HandleCqe(cqe);
        ++processed;
    }

    if (processed > 0) {
        io_uring_cq_advance(&ring_, processed);
    }
    cqe_ready_.store(false, std::memory_order_relaxed);

    return processed;
}

void RingListener::RecycleReadBuf(uint16_t bid, size_t bytes) {
    // The socket has finished processing inbound messages. Returns the borrowed
    // buffers to the buffer ring.
    int br_mask = buf_ring_size - 1;
    int buf_cnt = 0;
    while (bytes > 0) {
        char *this_buf = in_buf_ + bid * buf_length;
        io_uring_buf_ring_add(in_buf_ring_, this_buf, buf_length, bid, br_mask,
                              buf_cnt);

        bytes = bytes > buf_length ? bytes - buf_length : 0;
        bid = (bid + 1) & br_mask;
        buf_cnt++;
    }
    io_uring_buf_ring_advance(in_buf_ring_, buf_cnt);
}

void RingListener::RecycleWriteBuf(uint16_t buf_idx) {
    bthread::TaskGroup *cur_group = bthread::tls_task_group;
    if (task_group_ == cur_group) {
        write_buf_pool_->Recycle(buf_idx);
    } else {
        recycle_buf_cnt_.fetch_add(1, std::memory_order_relaxed);
        write_bufs_.enqueue(buf_idx);
        RingModule::NotifyWorker(task_group_->group_id_);
    }
}

int RingListener::SubmitRegisterFile(SocketRegisterData *register_data, int *fd, int32_t fd_idx) {
    brpc::Socket *sock = register_data->sock_;

    io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        LOG(ERROR) << "IO uring submission queue is full for the inbound "
                "listener, group: "
             << task_group_->group_id_;
        return -1;
    }

    io_uring_prep_files_update(sqe, fd, 1, fd_idx);
    uint64_t data = reinterpret_cast<uint64_t>(register_data);
    data = data << 16;
    data |= OpCodeToInt(OpCode::RegisterFile);
    io_uring_sqe_set_data64(sqe, data);
    sock->reg_fd_idx_ = fd_idx;

    ++submit_cnt_;
    return 0;
}

int RingListener::SubmitCancel(SocketUnRegisterData *unregister_data) {
    int fd = unregister_data->fd_;
    io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        LOG(ERROR) << "IO uring submission queue is full for the inbound "
                "listener, group: "
             << task_group_->group_id_;
        return -1;
    }

    int fd_idx = -1;
    auto it = reg_fds_.find(fd);
    if (it == reg_fds_.end()) {
        LOG(WARNING) << "Canceling an unregistered fd: " << fd;
    } else {
        fd_idx = it->second;
    }

    int sfd;
    uint64_t data = reinterpret_cast<uint64_t>(unregister_data);
    data <<= 16;

    int flags = 0;
    if (fd_idx >= 0) {
        flags |= IORING_ASYNC_CANCEL_FD_FIXED;
        sfd = fd_idx;
        unregister_data->fd_idx_ = fd_idx;
    } else {
        sfd = fd;
        unregister_data->fd_idx_ = UINT16_MAX;
    }

    io_uring_prep_cancel_fd(sqe, sfd, flags);
    data |= OpCodeToInt(OpCode::CancelRecv);
    io_uring_sqe_set_data64(sqe, data);
    if (fd_idx >= 0) {
        sqe->cancel_flags |= IOSQE_FIXED_FILE;
    }

    reg_fds_.erase(it);
    submit_cnt_++;
    return 0;
}

void RingListener::HandleCqe(io_uring_cqe *cqe) {
    uint64_t data = io_uring_cqe_get_data64(cqe);
    OpCode op = IntToOpCode(data & UINT8_MAX);

    switch (op) {
        case OpCode::Recv: {
            const SocketRecvData *recv_data = reinterpret_cast<SocketRecvData *>(data >> 16);
            const brpc::SocketId socket_id = recv_data->socket_id_;
            brpc::SocketUniquePtr ptr;

            if (brpc::Socket::Address(socket_id, &ptr) == 0 && ptr->fd() != -1) {
                HandleRecv(std::move(ptr), cqe);
            } else {
                if (cqe->flags & IORING_CQE_F_BUFFER) {
                    uint16_t buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                    int32_t nw = cqe->res;
                    task_group_->RecycleRingReadBuf(buf_id, nw);
                }
            }
            if (!(cqe->flags & IORING_CQE_F_MORE)) {
                delete recv_data;
            }
            break;
        }
        case OpCode::CancelRecv: {
            SocketUnRegisterData *unregister_data = reinterpret_cast<SocketUnRegisterData *>(data >> 16);
            if (cqe->res < 0) {
                LOG(ERROR) << "Failed to cancel socket recv, errno: " << cqe->res
                        << ", group: " << task_group_->group_id_
                        << ", sock: " << unregister_data->fd_;
            }
            uint16_t fd_idx = unregister_data->fd_idx_;
            // If the fd is a registered file, recycles the fixed file slot.
            if (fd_idx < UINT16_MAX) {
                free_reg_fd_idx_.emplace_back(fd_idx);
            }
            unregister_data->Notify(cqe->res);
            break;
        }
        case OpCode::RegisterFile: {
            SocketRegisterData *register_data = reinterpret_cast<SocketRegisterData *>(data >> 16);
            brpc::Socket *sock = register_data->sock_;
            if (cqe->res < 0) {
                LOG(WARNING) << "IO uring file registration failed, errno: " << cqe->res
                        << ", group: " << task_group_->group_id_
                        << ", socket: " << *sock;
                free_reg_fd_idx_.emplace_back(sock->reg_fd_idx_);
                sock->reg_fd_idx_ = -1;
            }
            sock->bound_g_ = task_group_;
            int ret = SubmitRecv(sock);
            if (ret == 0) {
                reg_fds_.try_emplace(sock->fd(), sock->reg_fd_idx_);
                register_data->Notify(true);
            } else {
                // SubmitRecv fails, no sqe available. Unregister the sock.
                if (sock->reg_fd_idx_ != -1) {
                    free_reg_fd_idx_.emplace_back(sock->reg_fd_idx_);
                    sock->reg_fd_idx_ = -1;
                }
                sock->bound_g_ = nullptr;
                register_data->Notify(false);
            }
            break;
        }
        case OpCode::FixedWrite:
        case OpCode::NonFixedWrite: {
            brpc::Socket *sock = reinterpret_cast<brpc::Socket *>(data >> 16);
            sock->RingNonFixedWriteCb(cqe->res);
            break;
        }
        case OpCode::WaitingNonFixedWrite: {
            brpc::Socket *sock = reinterpret_cast<brpc::Socket *>(data >> 16);
            sock->NotifyWaitingNonFixedWrite(cqe->res);
            break;
        }
        case OpCode::Fsync: {
            RingFsyncData *fsync_data = reinterpret_cast<RingFsyncData *>(data >> 16);
            int res = cqe->res;
            fsync_data->Notify(res);
            break;
        }
        case OpCode::SchedulerWakeup: {
            if (cqe->res < 0) {
                LOG(FATAL) << "The brpc worker wakeup poll failed, ret: "
                           << cqe->res;
            }
            DrainEventFd();
            // A multishot poll stays armed only while the CQE carries MORE.
            // Re-arm a terminated request so a later scheduler notification
            // cannot leave this worker permanently asleep.
            if (!(cqe->flags & IORING_CQE_F_MORE)) {
                const int ret = ArmEventFdPoll();
                if (ret != 0) {
                    LOG(ERROR) << "Failed to re-arm the brpc worker wakeup "
                                  "poll, ret: "
                               << ret;
                }
            }
            break;
        }
        default:
            break;
    }
}

void RingListener::HandleRecv(brpc::SocketUniquePtr sock, io_uring_cqe *cqe) {
    int32_t nw = cqe->res;
    uint16_t buf_id = UINT16_MAX;
    bool need_rearm = false;

    CHECK(sock != nullptr);

    if (nw < 0) {
        int err = -nw;
        if (err == ENOBUFS) {
            // There aren't enough buffers for the recv request. Retries the
            // request.
            uint64_t data = OpCodeToInt(OpCode::Recv);
            if (SubmitBacklog(sock.get(), data)) {
                return;
            }
        }

        if (err == EAGAIN || err == EINTR || err == ENOBUFS) {
            need_rearm = true;
        }
    } else {
        // Not having a buffer attached should only happen if we get a zero sized
        // receive, because the other end closed the connection. It cannot happen
        // otherwise, as all our receives are using provided buffers and hence
        // it's not possible to return a CQE with a non-zero result and not have a
        // buffer attached.
        if (cqe->flags & IORING_CQE_F_BUFFER) {
            buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            CHECK(nw > 0);
        }

        // If IORING_CQE_F_MORE isn't set, this multishot recv won't post any
        // further completions.
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            need_rearm = true;
        }
    }

    InboundRingBuf in_buf{sock.get(), nw, buf_id, need_rearm};
    brpc::Socket::SocketResume(std::move(sock), in_buf, task_group_);
}

void RingListener::HandleBacklog() {
    while (waiting_cnt_.load(std::memory_order_relaxed) > 0) {
        size_t cnt = waiting_socks_.TryDequeueBulk(waiting_batch_.begin(),
                                                   waiting_batch_.size());
        for (size_t idx = 0; idx < cnt; ++idx) {
            brpc::Socket *sock = waiting_batch_[idx].first;
            uint64_t data = waiting_batch_[idx].second;
            OpCode op = IntToOpCode(data & UINT8_MAX);
            switch (op) {
                case OpCode::Recv:
                    SubmitRecv(sock);
                    break;
                case OpCode::FixedWriteFinish:
                case OpCode::NonFixedWriteFinish: {
                    int nw = (int) (data >> 32);
                    sock->RingNonFixedWriteCb(nw);
                    break;
                }
                case OpCode::WaitingNonFixedWrite: {
                    SubmitWaitingNonFixedWrite(sock);
                    break;
                }
                default:
                    LOG(FATAL) << "backlog has an unsupported op, " << (int) op;
                    break;
            }
        }
        waiting_cnt_.fetch_sub(cnt, std::memory_order_release);
    }
}

bool RingListener::SubmitBacklog(brpc::Socket *sock, uint64_t data) {
    waiting_cnt_.fetch_add(1, std::memory_order_relaxed);
    bool success = waiting_socks_.TryEnqueue(std::make_pair(sock, data));
    if (!success) {
        waiting_cnt_.fetch_sub(1, std::memory_order_relaxed);
    }

    return success;
}

void RingListener::RecycleReturnedWriteBufs() {
    while (recycle_buf_cnt_.load(std::memory_order_relaxed) > 0) {
        uint16_t buf_idxes[100];
        int n = write_bufs_.try_dequeue_bulk(buf_idxes, 100);
        for (size_t idx = 0; idx < n; ++idx) {
            write_buf_pool_->Recycle(buf_idxes[idx]);
        }
        recycle_buf_cnt_.fetch_sub(n, std::memory_order_relaxed);
    }
}

#endif
