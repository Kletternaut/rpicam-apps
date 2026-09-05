/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <variant>

class RuntimeDataStore
{
public:
    using Value = std::variant<bool, int64_t, double, std::string>;
    using Snapshot = std::map<std::string, Value>;

    void Set(const std::string &key, Value value);
    bool Delete(const std::string &key);
    void Clear();

    Snapshot SnapshotValues() const;
    uint64_t Generation() const;

private:
    mutable std::mutex mutex_;
    Snapshot values_;
    uint64_t generation_ = 0;
};
