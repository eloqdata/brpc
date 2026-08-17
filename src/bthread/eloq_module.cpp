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

#include "eloq_module.h"

#include "task_control.h"
#include "bthread/bthread.h"

extern "C" {
bthread::TaskControl *bthread_get_task_control();
}

extern std::array<eloq::EloqModule *, eloq::kModuleTypeCount> registered_modules;
extern std::atomic<int> registered_module_cnt;
extern std::atomic<uint64_t> registered_module_version;

namespace eloq {
    namespace {
        struct ModuleTypeNameEntry {
            ModuleType type_;
            const char *name_;
        };
        constexpr ModuleTypeNameEntry kModuleTypeNames[] = {
            {ModuleType::kRing, "ring"},
            {ModuleType::kTxService, "txservice"},
            {ModuleType::kEloqStore, "eloqstore"},
        };
        static_assert(sizeof(kModuleTypeNames) / sizeof(kModuleTypeNames[0]) ==
                      kModuleTypeCount);
    }  // namespace

    const char *ModuleTypeName(ModuleType type) {
        for (const auto &entry : kModuleTypeNames) {
            if (entry.type_ == type) {
                return entry.name_;
            }
        }
        return "unknown";
    }

    bool ParseModuleTypeName(const std::string &name, ModuleType *type) {
        for (const auto &entry : kModuleTypeNames) {
            if (name == entry.name_) {
                *type = entry.type_;
                return true;
            }
        }
        return false;
    }

    bool EloqModule::NotifyWorker(int thd_id) {
        return bthread_notify_worker(thd_id);
    }

    int register_module(EloqModule *module) {
        // The module's type is its slot, so the registry never shifts and a
        // slot always denotes the same kind of module.
        const size_t slot = static_cast<size_t>(module->Type());
        CHECK_LT(slot, registered_modules.size());
        std::unique_lock lk(module_mutex);
        // A module type is a singleton; registering a second instance while
        // the first is live would silently displace it.
        CHECK(registered_modules[slot] == nullptr)
                << "module type " << ModuleTypeName(module->Type())
                << " is already registered";
        registered_modules[slot] = module;
        registered_module_cnt.fetch_add(1, std::memory_order_release);
        registered_module_version.fetch_add(1, std::memory_order_release);
        const auto non_null_modules =
                std::count_if(registered_modules.begin(), registered_modules.end(),
                              [](EloqModule *registered_module) {
                                  return registered_module != nullptr;
                              });
        CHECK_EQ(static_cast<int>(non_null_modules),
                 registered_module_cnt.load(std::memory_order_acquire));
        return 0;
    }

    int unregister_module(EloqModule *module) {
        // Verify that the module is currently registered.
        std::shared_lock s_lk(module_mutex);
        const bool exists = std::find(registered_modules.begin(),
                                      registered_modules.end(),
                                      module) != registered_modules.end();
        if (!exists) {
            LOG(WARNING) << "Attempted to unregister a non-registered module: " << module;
            return -1;
        }
        s_lk.unlock();

        const auto concurrency = bthread_get_task_control()->concurrency();
        while (module->registered_workers_.load(std::memory_order_acquire) != concurrency) {
            for (int thd_id = 0; thd_id < concurrency; ++thd_id) {
                EloqModule::NotifyWorker(thd_id);
            }
            bthread_usleep(1000);
        }
        std::unique_lock lk(module_mutex);
        const size_t slot = static_cast<size_t>(module->Type());
        if (slot >= registered_modules.size() ||
            registered_modules[slot] != module) {
            return 0;
        }
        // Clear the slot in place. Compacting the array would renumber every
        // higher module, so a slot would stop denoting the same module across
        // an unregister -- which is what --module_visit_order addresses.
        registered_modules[slot] = nullptr;
        registered_module_cnt.fetch_sub(1, std::memory_order_release);
        registered_module_version.fetch_add(1, std::memory_order_release);
        const auto non_null_modules =
                std::count_if(registered_modules.begin(), registered_modules.end(),
                              [](EloqModule *registered_module) {
                                  return registered_module != nullptr;
                              });
        CHECK_EQ(static_cast<int>(non_null_modules),
                 registered_module_cnt.load(std::memory_order_acquire));
        lk.unlock();

        while (module->registered_workers_.load(std::memory_order_acquire) != 0) {
            bthread_usleep(5000);
            for (int thd_id = 0; thd_id < concurrency; ++thd_id) {
                EloqModule::NotifyWorker(thd_id);
            }
        }
        return 0;
    }
} // namespace eloq
