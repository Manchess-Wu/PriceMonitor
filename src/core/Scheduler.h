#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace pm {

// 定时调度器：周期性取回「到期商品」，投递到固定大小的工作线程池执行采集。
class Scheduler {
public:
    using DueListFn = std::function<std::vector<long long>()>;
    using CrawlFn = std::function<bool(long long)>;

    Scheduler(int tickSeconds, int concurrency, DueListFn dueList, CrawlFn crawl);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void start();
    void stop();

private:
    void tickLoop();
    void workerLoop();
    void enqueue(long long id);

    int tickSeconds_;
    int concurrency_;
    DueListFn dueList_;
    CrawlFn crawl_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<long long> queue_;
    std::unordered_set<long long> pending_;
    bool stop_ = false;
    bool started_ = false;

    std::thread ticker_;
    std::vector<std::thread> workers_;
};

}  // namespace pm
