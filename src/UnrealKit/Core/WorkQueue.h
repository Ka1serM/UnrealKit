#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>

#include "UnrealKit/Core/Error.h"

namespace UnrealKit::Core {

template <class T>
class WorkQueue {
public:
    void push(T item) {
        {
            std::lock_guard lock(mutex);
            items.push_back(std::move(item));
        }
        changed.notify_one();
    }

    void finish(std::expected<void, Core::Error> result = {}) {
        {
            std::lock_guard lock(mutex);
            this->result = std::move(result);
            done = true;
        }
        changed.notify_all();
    }

    std::expected<std::optional<T>, Core::Error> pop(std::stop_token stop) {
        std::unique_lock lock(mutex);
        while (items.empty() && !done) {
            if (stop.stop_requested())
                return cancelled();
            changed.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (!items.empty()) {
            T item = std::move(items.front());
            items.pop_front();
            return item;
        }
        if (!result)
            return std::unexpected(result.error());
        return std::nullopt;
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<T> items;
    std::expected<void, Core::Error> result;
    bool done = false;
};

}
