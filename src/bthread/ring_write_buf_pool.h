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

#pragma once

#ifdef IO_URING_ENABLED

#include <cstdint>
#include <cstdlib>
#include <butil/logging.h>
#include <liburing.h>
#include <vector>
#include <sys/resource.h>

class RingWriteBufferPool {
public:
    RingWriteBufferPool(size_t pool_size, io_uring *ring) {
        mem_bulk_ = (char *) std::aligned_alloc(buf_length, buf_length * pool_size);

        std::vector<iovec> register_buf;
        register_buf.resize(pool_size);
        buf_pool_.resize(pool_size);

        for (size_t idx = 0; idx < pool_size; ++idx) {
            buf_pool_[idx] = idx;
            register_buf[idx].iov_base = mem_bulk_ + (idx * buf_length);
            register_buf[idx].iov_len = buf_length;
        }

        int ret = io_uring_register_buffers(ring, register_buf.data(),
                                            register_buf.size());
        while (ret < 0 && -ret == ENOMEM && pool_size >= 128) {
            pool_size /= 2;
            LOG(ERROR) << "Failed to register IO uring fixed buffers: ENOMEM,"
                    " trying to decrease the pool size to: " << pool_size;
            register_buf.resize(pool_size);
            buf_pool_.resize(pool_size);
            ret = io_uring_register_buffers(ring, register_buf.data(),
                                            register_buf.size());
        }
        if (ret < 0) {
            int err = -ret;

            LOG(ERROR) << "Failed to register IO uring fixed buffers. errno: "
                    << err << " (" << strerror(err) << ")";
            rlimit memlock_limit{};
            if (getrlimit(RLIMIT_MEMLOCK, &memlock_limit) == 0) {
                std::string soft_limit = memlock_limit.rlim_cur == RLIM_INFINITY
                                             ? "unlimited"
                                             : std::to_string(static_cast<unsigned long long>(memlock_limit.rlim_cur));
                std::string hard_limit = memlock_limit.rlim_max == RLIM_INFINITY
                                             ? "unlimited"
                                             : std::to_string(static_cast<unsigned long long>(memlock_limit.rlim_max));
                LOG(WARNING) << "io_uring_register_buffers encountered ENOMEM. "
                             << "Requested buffer count: " << pool_size << ", "
                             << "each buffer size: " << buf_length << ". "
                             << "Current RLIMIT_MEMLOCK soft=" << soft_limit
                             << " hard=" << hard_limit
                             << ". Raise the limit or reduce "
                                "FLAGS_io_uring_write_buffer_pool.";
            } else {
                const int saved_errno = errno;
                LOG(WARNING) << "Failed to query RLIMIT_MEMLOCK while handling "
                                "io_uring_register_files_sparse failure, errno: "
                             << saved_errno;
            }
            free(mem_bulk_);
            mem_bulk_ = nullptr;
            buf_pool_.clear();
        } else {
            LOG(INFO) << "IO uring fixed buffers registered, buffer count: "
                << pool_size;
        }
    }

    ~RingWriteBufferPool() {
        if (mem_bulk_ != nullptr) {
            free(mem_bulk_);
        }
    }

    std::pair<char *, uint16_t> Get() {
        if (buf_pool_.empty()) {
            return {nullptr, UINT16_MAX};
        } else {
            uint16_t buf_idx = buf_pool_.back();
            buf_pool_.pop_back();
            return {mem_bulk_ + (buf_idx * buf_length), buf_idx};
        }
    }

    const char *GetBuf(uint16_t buf_idx) const {
        return mem_bulk_ + (buf_idx * buf_length);
    }

    void Recycle(uint16_t buf_idx) {
        buf_pool_.emplace_back(buf_idx);
    }

    inline static size_t buf_length = sysconf(_SC_PAGESIZE);

private:
    char *mem_bulk_{nullptr};
    std::vector<uint16_t> buf_pool_;

    friend class RingListener;
};
#endif
