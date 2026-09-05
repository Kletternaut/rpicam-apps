/* SPDX-License-Identifier: BSD-2-Clause */
#include "runtime_data_store.hpp"

void RuntimeDataStore::Set(const std::string &key, Value value)
{
    if (key.empty())
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    values_[key] = std::move(value);
    ++generation_;
}

bool RuntimeDataStore::Delete(const std::string &key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const bool removed = values_.erase(key) != 0;
    if (removed)
        ++generation_;
    return removed;
}

void RuntimeDataStore::Clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!values_.empty())
    {
        values_.clear();
        ++generation_;
    }
}

RuntimeDataStore::Snapshot RuntimeDataStore::SnapshotValues() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return values_;
}

uint64_t RuntimeDataStore::Generation() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}
