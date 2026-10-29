// SPDX-License-Identifier: GPL-3.0-or-later
// A small fixed thread pool: parallel_for over an index range, and tasks.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace ud {

class ThreadPool {
public:
    explicit ThreadPool(int threads = 0) {
        int n = threads > 0 ? threads : int(std::max(1u, std::thread::hardware_concurrency()));
        for (int i = 0; i < n; ++i) m_workers.emplace_back([this] { run(); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(m_m);
            m_stop = true;
        }
        m_cv.notify_all();
        for (std::thread& t : m_workers) t.join();
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    int size() const { return int(m_workers.size()); }

    template <class F>
    auto submit(F&& f) -> std::future<decltype(f())> {
        using R = decltype(f());
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(m_m);
            m_q.push([task] { (*task)(); });
        }
        m_cv.notify_one();
        return fut;
    }

    // Runs fn(i) for i in [0, n) across the pool and waits.
    void parallelFor(int n, const std::function<void(int)>& fn) {
        if (n <= 0) return;
        if (n == 1 || m_workers.size() <= 1) {
            for (int i = 0; i < n; ++i) fn(i);
            return;
        }
        std::atomic<int> next{0};
        const int jobs = std::min<int>(n, int(m_workers.size()));
        std::vector<std::future<void>> fs;
        for (int j = 0; j < jobs; ++j)
            fs.push_back(submit([&] {
                for (int i = next++; i < n; i = next++) fn(i);
            }));
        for (auto& f : fs) f.get();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(m_m);
                m_cv.wait(lk, [&] { return m_stop || !m_q.empty(); });
                if (m_stop && m_q.empty()) return;
                job = std::move(m_q.front());
                m_q.pop();
            }
            job();
        }
    }
    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_q;
    std::mutex m_m;
    std::condition_variable m_cv;
    bool m_stop = false;
};

}  // namespace ud
