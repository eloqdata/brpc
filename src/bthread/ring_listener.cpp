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
#include "brpc/socket.h"
#include "bthread/task_group.h"
#include "bthread/eloq_module.h"
#include "bthread/inbound_ring_buf.h"
#include "bthread/ring_write_buf_pool.h"
#include "butil/threading/platform_thread.h"

#include "ring_listener.h"

RingListener::~RingListener() {
    for (auto [fd, fd_idx]: reg_fds_) {
        SocketUnRegisterData data;
        data.fd_ = fd;
        SubmitCancel(&data);
        // Not wait here because the worker should have quit already.
    }
    SubmitAll();

    poll_status_.store(PollStatus::Closed, std::memory_order_release); {
        std::unique_lock<std::mutex> lk(mux_);
        cv_.notify_one();
    }

    if (poll_thd_.joinable()) {
        poll_thd_.join();
    }
    Close();
}

int RingListener::Init() {
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags      = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CQSIZE;
    params.cq_entries = 2048;                 // 目标 CQE 数
    unsigned sq_entries = 1024;               // 原来的 SQ 深度
    int ret = io_uring_queue_init_params(sq_entries, &ring_, &params);

    if (ret < 0) {
        LOG(WARNING) << "Failed to initialize the IO uring of the inbound "
                  "listener, errno: "
               << ret;
        Close();
        return ret;
    }
    ring_init_ = true;

    ret = io_uring_register_files_sparse(&ring_, 1024);
    if (ret < 0) {
        LOG(WARNING) << "Failed to register sparse files for the inbound listener.";
        Close();
        return ret;
    }

    free_reg_fd_idx_.reserve(1024);
    for (uint16_t f_idx = 0; f_idx < 1024; ++f_idx) {
        free_reg_fd_idx_.emplace_back(f_idx);
    }

    in_buf_ =
            (char *) std::aligned_alloc(buf_length, buf_length * buf_ring_size);
    in_buf_ring_ = io_uring_setup_buf_ring(&ring_, buf_ring_size, 0, 0, &ret);
    if (in_buf_ring_ == nullptr) {
        LOG(WARNING) << "Failed to register buffer ring for the inbound listener.";
        Close();
        return ret;
    }

    LOG(INFO) << "SQ=" << params.sq_entries << " CQ=" << params.cq_entries
              << " features=0x" << std::hex << params.features;

    char *ptr = in_buf_;
    // inbound_ring_size must be the power of 2.
    int br_mask = buf_ring_size - 1;
    for (size_t idx = 0; idx < buf_ring_size; idx++) {
        io_uring_buf_ring_add(in_buf_ring_, ptr, buf_length, idx, br_mask, idx);
        ptr += buf_length;
    }
    io_uring_buf_ring_advance(in_buf_ring_, buf_ring_size);

    write_buf_pool_ = std::make_unique<RingWriteBufferPool>(1024, &ring_);

    poll_status_.store(PollStatus::Sleep, std::memory_order_release);
    poll_thd_ = std::thread([&]() {
        std::string ring_listener = "ring_listener:";
        ring_listener.append(std::to_string(task_group_->group_id_));
        butil::PlatformThread::SetName(ring_listener.c_str());

        Run();
    });

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

void RingListener::PollAndNotify() {
    io_uring_cqe *cqe = nullptr;
    int ret = io_uring_wait_cqe(&ring_, &cqe);
    if (ret < 0) {
        LOG(ERROR) << "Listener uring wait errno: " << ret;
        return;
    }
    cqe_ready_.store(true, std::memory_order_relaxed);
    poll_status_.store(PollStatus::Sleep, std::memory_order_relaxed);
    RingModule::NotifyWorker(task_group_->group_id_);
}


size_t RingListener::ExtPoll() {
    if (!has_external_.load(std::memory_order_relaxed)) {
        has_external_.store(true, std::memory_order_release);
    }

    // has_external_ should be updated before poll_status_ is checked.
    std::atomic_thread_fence(std::memory_order_release);

    PollStatus status = PollStatus::Sleep;
    if (!poll_status_.compare_exchange_strong(status, PollStatus::ExtPoll)) {
        return 0;
    }

    HandleBacklog();

    io_uring_cqe *cqe = nullptr;
    int ret = io_uring_peek_cqe(&ring_, &cqe);
    if (ret != 0) {
        poll_status_.store(PollStatus::Sleep, std::memory_order_relaxed);
        return 0;
    }

    int processed = 0;
    unsigned int head;
    io_uring_for_each_cqe(&ring_, head, cqe) {
        HandleCqe(cqe);
        ++processed;
    }

    cqe_ready_.store(false, std::memory_order_relaxed);

    if (processed > 0) {
        io_uring_cq_advance(&ring_, processed);
    }
    cqe_ready_.store(false, std::memory_order_relaxed);
    poll_status_.store(PollStatus::Sleep, std::memory_order_relaxed);

    return processed;
}

void RingListener::ExtWakeup() {
    has_external_.store(false, std::memory_order_relaxed);
    if (poll_status_.load(std::memory_order_relaxed) != PollStatus::Sleep) {
        return;
    }
    std::unique_lock<std::mutex> lk(mux_);
    cv_.notify_one();
}

void RingListener::Run() {
    while (poll_status_.load(std::memory_order_relaxed) != PollStatus::Closed) {
        bool success = false;
        if (!has_external_.load(std::memory_order_relaxed)) {
            PollStatus status = PollStatus::Sleep;
            success = poll_status_.compare_exchange_strong(status, PollStatus::Active,
                                                           std::memory_order_acq_rel);
            if (success) {
                PollAndNotify();
            }
        }
        std::unique_lock<std::mutex> lk(mux_);
        cv_.wait(lk, [this]() {
            // wait for the worker to process the ready cqes and notify RingListener when it sleeps
            return !has_external_.load(std::memory_order_relaxed)
                   && !cqe_ready_.load(std::memory_order_relaxed) ||
                   poll_status_.load(std::memory_order_relaxed) ==
                   PollStatus::Closed;
        });
    }
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

            if (brpc::Socket::Address(socket_id, &ptr) == 0) {
                HandleRecv(std::move(ptr), cqe);
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
