#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace aegis::agent {

class BoundedRequestExecutor final {
public:
    using Task = std::function<void()>;

    BoundedRequestExecutor(std::size_t worker_count, std::size_t capacity);

    ~BoundedRequestExecutor() noexcept;

    BoundedRequestExecutor(const BoundedRequestExecutor&) = delete;
    BoundedRequestExecutor& operator=(const BoundedRequestExecutor&) = delete;

    void Start();

    [[nodiscard]] bool TrySubmit(Task task);

    void RequestStop() noexcept;

    void Wait();

    [[nodiscard]] std::size_t InFlightCount() const noexcept;

    [[nodiscard]] bool IsWorkerThread() const noexcept;

private:
    void WorkerLoop() noexcept;

    void WaitNoThrow() noexcept;

private:
    const std::size_t worker_count_;
    const std::size_t capacity_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Task> tasks_;
    std::vector<std::thread> workers_;
    std::size_t in_flight_count_{0};
    bool accepting_{false};
    bool stop_requested_{false};
};

} // namespace aegis::agent
