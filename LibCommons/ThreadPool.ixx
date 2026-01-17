module;

#include <vector>
#include <queue>
#include <thread>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

export module commons.thread_pool;

import std;

namespace LibCommons
{

export class ThreadPool
{
public:
    ThreadPool(size_t numThreads) : m_bStopped(false)
    {
        for (size_t i = 0; i < numThreads; ++i)
        {
            m_Workers.emplace_back([this](std::stop_token stopToken)
                {
                    WorkerThread(stopToken);
                });
        }
    }

    ~ThreadPool()
    {
        Stop();
    }

    void Enqueue(std::function<void()> task)
    {
        {
            std::unique_lock<std::mutex> lock(m_TaskQueueMutex);
            m_TaskQueue.emplace(std::move(task));
        }
        m_Condition.notify_one();
    }

    void Stop()
    {
        {
            std::unique_lock<std::mutex> lock(m_TaskQueueMutex);
            if (m_bStopped)
            {
                return;
            }
            m_bStopped = true;
        }
        m_Condition.notify_all();
    }

    bool IsStopped() const
    {
        return m_bStopped.load();
    }

private:
    void WorkerThread(std::stop_token stoken)
    {
        for (;;)
        {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(m_TaskQueueMutex);
                m_Condition.wait(lock, [this, &stoken]
                    {
                        return m_bStopped || !m_TaskQueue.empty() || stoken.stop_requested();
                    });

                if ((m_bStopped || stoken.stop_requested()) && m_TaskQueue.empty())
                {
                    return;
                }

                if (m_TaskQueue.empty())
                {
                    continue;
                }

                task = std::move(m_TaskQueue.front());
                m_TaskQueue.pop();
            }

            if (task)
            {
                task();
            }
        }
    }

private:
    std::vector<std::jthread> m_Workers;

    std::queue<std::function<void()>> m_TaskQueue;
    std::mutex m_TaskQueueMutex;

    std::condition_variable m_Condition;

    std::atomic<bool> m_bStopped;
};

} // namespace LibCommons
