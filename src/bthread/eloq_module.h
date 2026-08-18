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

#ifndef ELOQ_MODULE_H
#define ELOQ_MODULE_H

#include <atomic>
#include <cstddef>
#include <shared_mutex>
#include <string>

namespace eloq {
    inline std::shared_mutex module_mutex;

    /**
     * Identifies what a module is, independently of when it registers. For the
     * infrastructure modules the enumerator is also the module's slot in the
     * registry, so a module always occupies the same slot: the mapping survives
     * a module being absent (e.g. RingModule when io_uring is off) and a module
     * restarting (e.g. EloqStore reopening, which happens during normal
     * startup).
     *
     * kRuntime is the API-layer runtime driving client requests — the mongo
     * service executor in EloqDoc, the mariadb thread pool in EloqSQL. Unlike
     * the infrastructure types it is not a per-process singleton: a converged
     * binary can host several runtimes at once, so kRuntime maps to a range of
     * registry slots ([kRuntimeSlotBegin, kRegistrySlotCount)) rather than one,
     * and a registering runtime takes the first free slot in that range.
     */
    enum class ModuleType : size_t {
        kRing = 0,
        kTxService = 1,
        kEloqStore = 2,
        kRuntime = 3,
    };

    inline constexpr size_t kModuleTypeCount = 4;

    /** First registry slot of the runtime range. */
    inline constexpr size_t kRuntimeSlotBegin =
            static_cast<size_t>(ModuleType::kRuntime);
    /** How many runtime modules may be registered at once. */
    inline constexpr size_t kMaxRuntimeModules = 4;
    /** Total registry slots: one per infrastructure type + the runtime range. */
    inline constexpr size_t kRegistrySlotCount =
            kRuntimeSlotBegin + kMaxRuntimeModules;

    /** @brief Stable name of a module type, e.g. "eloqstore". */
    const char *ModuleTypeName(ModuleType type);

    /**
     * @brief Maps a module type name back to its enumerator.
     * @return true if the name is known, in which case *type is set.
     */
    bool ParseModuleTypeName(const std::string &name, ModuleType *type);

    class EloqModule {
    public:
        virtual ~EloqModule() = default;

        /**
         * What this module is. Determines the module's slot in the registry
         * and the name --module_visit_order uses to refer to it.
         */
        virtual ModuleType Type() const = 0;

        /**
         * This func is called when worker starts running.
         * @param thd_id
         */
        virtual void ExtThdStart(int thd_id) = 0;

        /**
         * Called when worker stop running and sleep.
         * @param thd_id
         */
        virtual void ExtThdEnd(int thd_id) = 0;

        /**
         * How the module task is processed.
         * @param thd_id
         */
        virtual void Process(int thd_id) = 0;

        /**
         *
         * @param thd_id
         * @return whther the module has task to process.
         */
        virtual bool HasTask(int thd_id) const = 0;

        /**
         * This func is for the module to wake up the worker.
         * @param thd_id
         * @return true if the worker is running or successfully notified.
         */
        static bool NotifyWorker(int thd_id);

        std::atomic<int> registered_workers_{0};
    };

    extern int register_module(EloqModule *module);

    extern int unregister_module(EloqModule *module);
} // namespace eloq

#endif //ELOQ_MODULE_H
