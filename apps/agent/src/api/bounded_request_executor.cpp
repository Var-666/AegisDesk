#include "bounded_request_executor.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace aegis::agent {

BoundedRequestExecutor::BoundedRequestExecutor(const std::size_t worker_count, const std::size_t capacity)
    : worker_count_(worker_count)
    , capacity_(capacity) {
    if (worker_count_ == 0 || capacity_ == 0) {
        throw std::invalid_argument("request executor worker count and capacity must be greater than zero");
    }
}

BoundedRequestExecutor::~BoundedRequestExecutor() noexcept {
    RequestStop();
    WaitNoThrow();
}

void BoundedRequestExecutor::Start() {
    std::unique_lock lock(mutex_);

    if (accepting_ || !workers_.empty() || !tasks_.empty() || in_flight_count_ != 0) {
        throw std::logic_error("request executor is already running or has not finished stopping");
    }

    workers_.reserve(worker_count_);
    accepting_ = true;
    stop_requested_ = false;

    try {
        for (std::size_t index = 0; index < worker_count_; ++index) {
            workers_.emplace_back(&BoundedRequestExecutor::WorkerLoop, this);
        }
    } catch (...) {
        accepting_ = false;
        stop_requested_ = true;
        lock.unlock();
        condition_.notify_all();

        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        lock.lock();
        workers_.clear();
        stop_requested_ = false;
        throw;
    }
}

bool BoundedRequestExecutor::TrySubmit(Task task) {
    if (!task) {
        throw std::invalid_argument("request executor task must not be empty");
    }

    {
        std::scoped_lock lock(mutex_);

        if (!accepting_ || stop_requested_ || in_flight_count_ >= capacity_) {
            return false;
        }

        tasks_.push_back(std::move(task));
        ++in_flight_count_;
    }

    condition_.notify_one();
    return true;
}

void BoundedRequestExecutor::RequestStop() noexcept {
    {
        std::scoped_lock lock(mutex_);
        accepting_ = false;
        stop_requested_ = true;
    }

    condition_.notify_all();
}

void BoundedRequestExecutor::Wait() {
    RequestStop();

    std::vector<std::thread> workers;
    {
        std::scoped_lock lock(mutex_);

        for (const std::thread& worker : workers_) {
            if (worker.joinable() && worker.get_id() == std::this_thread::get_id()) {
                throw std::logic_error("request executor worker cannot wait for itself");
            }
        }

        workers = std::move(workers_);
    }

    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    std::scoped_lock lock(mutex_);
    tasks_.clear();
    in_flight_count_ = 0;
    stop_requested_ = false;
}

std::size_t BoundedRequestExecutor::InFlightCount() const noexcept {
    std::scoped_lock lock(mutex_);
    return in_flight_count_;
}

bool BoundedRequestExecutor::IsWorkerThread() const noexcept {
    const std::thread::id current_thread = std::this_thread::get_id();
    std::scoped_lock lock(mutex_);

    for (const std::thread& worker : workers_) {
        if (worker.joinable() && worker.get_id() == current_thread) {
            return true;
        }
    }

    return false;
}

void BoundedRequestExecutor::WorkerLoop() noexcept {
    while (true) {
        Task task;

        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stop_requested_ || !tasks_.empty(); });

            if (tasks_.empty()) {
                if (stop_requested_) {
                    return;
                }
                continue;
            }

            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

        try {
            task();
        } catch (const std::exception& exception) {
            std::cerr << "[agent] request executor task failed: " << exception.what() << '\n';
        } catch (...) {
            std::cerr << "[agent] request executor task failed: unknown error\n";
        }

        {
            std::scoped_lock lock(mutex_);
            --in_flight_count_;
        }
    }
}

void BoundedRequestExecutor::WaitNoThrow() noexcept {
    try {
        Wait();
    } catch (...) {}
}

} // namespace aegis::agent
