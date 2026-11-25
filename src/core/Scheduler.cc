#include "core/Scheduler.h"

#include <chrono>
#include <exception>

#include "core/Logger.h"

namespace pm {

Scheduler::Scheduler(int tickSeconds, int concurrency, DueListFn dueList,
                     CrawlFn crawl)
    : tickSeconds_(tickSeconds > 0 ? tickSeconds : 60),
      concurrency_(concurrency > 0 ? concurrency : 1),
      dueList_(std::move(dueList)),
      crawl_(std::move(crawl)) {}

Scheduler::~Scheduler() { stop(); }

void Scheduler::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return;
    started_ = true;
    stop_ = false;

    workers_.reserve(static_cast<size_t>(concurrency_));
    for (int i = 0; i < concurrency_; ++i) {
        workers_.emplace_back(&Scheduler::workerLoop, this);
    }
    ticker_ = std::thread(&Scheduler::tickLoop, this);
    logInfo("调度器已启动，采集并发数: " + std::to_string(concurrency_));
}

void Scheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ && !ticker_.joinable() && workers_.empty()) return;
        stop_ = true;
        started_ = false;
    }
    cv_.notify_all();

    if (ticker_.joinable()) ticker_.join();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();

    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    pending_.clear();
    logInfo("调度器已停止");
}

void Scheduler::tickLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) return;
        }

        try {
            for (long long id : dueList_()) {
                enqueue(id);
            }
        } catch (const std::exception& e) {
            logError(std::string("调度器获取任务失败: ") + e.what());
        }

        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(tickSeconds_),
                     [this]() { return stop_; });
        if (stop_) return;
    }
}

void Scheduler::enqueue(long long id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return;
        if (pending_.count(id) > 0) return;
        pending_.insert(id);
        queue_.push_back(id);
    }
    cv_.notify_one();
}

void Scheduler::workerLoop() {
    while (true) {
        long long id = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stop_) return;
                continue;
            }
            id = queue_.front();
            queue_.pop_front();
        }

        try {
            crawl_(id);
        } catch (const std::exception& e) {
            logError("采集任务异常 (id=" + std::to_string(id) + "): " + e.what());
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.erase(id);
        }
    }
}

}  // namespace pm
