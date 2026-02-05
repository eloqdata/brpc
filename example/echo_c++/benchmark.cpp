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

// Benchmark echo-server by multiple threads.

#include <pthread.h>
#include <unistd.h>

#include <vector>

#include <gflags/gflags.h>
#include <bthread/bthread.h>
#include <bvar/bvar.h>
#include <butil/logging.h>
#include <brpc/channel.h>
#include <brpc/server.h>

#include "echo.pb.h"

DEFINE_string(message, "hello world", "Message body sent to server");
DEFINE_string(attachment, "", "Carry this along with requests");
DEFINE_string(protocol, "baidu_std", "Protocol type. Defined in src/brpc/options.proto");
DEFINE_string(connection_type, "", "Connection type. Available values: single, pooled, short");
DEFINE_string(server, "0.0.0.0:8000", "IP Address of server");
DEFINE_string(load_balancer, "", "The algorithm for load balancing");
DEFINE_int32(thread_num, 50, "Number of threads to send requests");
DEFINE_bool(use_bthread, false, "Use bthread to send requests");
DEFINE_int32(timeout_ms, 100, "RPC timeout in milliseconds");
DEFINE_int32(max_retry, 3, "Max retries(not including the first RPC)");
DEFINE_bool(ssl, true, "Enable ssl");
DEFINE_bool(dont_fail, false, "Print fatal when some call failed");
DEFINE_int32(dummy_port, -1, "Launch dummy server at this port");

bvar::LatencyRecorder g_latency_recorder("echo_client");

static void* sender(void* arg) {
    brpc::Channel* channel = static_cast<brpc::Channel*>(arg);
    example::EchoService_Stub stub(channel);

    while (!brpc::IsAskedToQuit()) {
        example::EchoRequest request;
        example::EchoResponse response;
        brpc::Controller cntl;

        request.set_message(FLAGS_message);
        cntl.set_timeout_ms(FLAGS_timeout_ms);
        cntl.set_max_retry(FLAGS_max_retry);
        cntl.request_attachment().append(FLAGS_attachment);

        stub.Echo(&cntl, &request, &response, NULL);
        if (!cntl.Failed()) {
            g_latency_recorder << cntl.latency_us();
            LOG(INFO) << "Echo response: " << response.message();
        } else {
            LOG(WARNING) << "Fail to echo: " << cntl.ErrorText()
                         << " latency=" << cntl.latency_us();
            CHECK(brpc::IsAskedToQuit() || !FLAGS_dont_fail)
                << "error=" << cntl.ErrorText() << " latency=" << cntl.latency_us();
            bthread_usleep(100000);
        }
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    GFLAGS_NS::ParseCommandLineFlags(&argc, &argv, true);

    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = FLAGS_protocol;
    options.connection_type = FLAGS_connection_type;
    options.timeout_ms = FLAGS_timeout_ms;
    options.max_retry = FLAGS_max_retry;
    if (FLAGS_ssl) {
        options.mutable_ssl_options();
    }

    if (channel.Init(FLAGS_server.c_str(), FLAGS_load_balancer.c_str(), &options) != 0) {
        LOG(ERROR) << "Fail to initialize channel";
        return -1;
    }

    std::vector<bthread_t> bids;
    std::vector<pthread_t> pids;
    if (!FLAGS_use_bthread) {
        pids.resize(FLAGS_thread_num);
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (pthread_create(&pids[i], NULL, sender, &channel) != 0) {
                LOG(ERROR) << "Fail to create pthread";
                return -1;
            }
        }
    } else {
        bids.resize(FLAGS_thread_num);
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (bthread_start_background(&bids[i], NULL, sender, &channel) != 0) {
                LOG(ERROR) << "Fail to create bthread";
                return -1;
            }
        }
    }

    if (FLAGS_dummy_port >= 0) {
        brpc::StartDummyServerAt(FLAGS_dummy_port);
    }

    while (!brpc::IsAskedToQuit()) {
        sleep(1);
        LOG(INFO) << "Sending echo requests at qps=" << g_latency_recorder.qps(1)
                  << " latency=" << g_latency_recorder.latency(1);
    }

    LOG(INFO) << "Echo benchmark is going to quit";
    for (int i = 0; i < FLAGS_thread_num; ++i) {
        if (!FLAGS_use_bthread) {
            pthread_join(pids[i], NULL);
        } else {
            bthread_join(bids[i], NULL);
        }
    }

    return 0;
}
