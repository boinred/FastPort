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
    ThreadPool(size_t numThreads)
        : m_Stop(false)
    {
        for (size_t i = 0; i < numThreads; ++i)
        {
            m_Workers.emplace_back([this](std::stop_token stopToken)
            {
                this->WorkerThread(stopToken);
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
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            m_Tasks.emplace(std::move(task));
        }
        m_Condition.notify_one();
    }

    void Stop()
    {
        {
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            if (m_Stop) return;
            m_Stop = true;
        }
        m_Condition.notify_all();
        
        // jthread joins automatically on destruction, but we can request stop manually if needed.
        // We rely on the destructor or explicit variable check.
    }

private:
    void WorkerThread(std::stop_token stopToken)
    {
        while (true)
        {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(m_QueueMutex);
                m_Condition.wait(lock, [this, &stopToken]
                {
                    return m_Stop || !m_Tasks.empty() || stopToken.stop_requested();
                });

                if ((m_Stop || stopToken.stop_requested()) && m_Tasks.empty())
                {
                    return;
                }

                if (m_Tasks.empty())
                {
                    continue;
                }

                task = std::move(m_Tasks.front());
                m_Tasks.pop();
            }

            if (task)
            {
                task();
            }
        }
    }

private:
    std::vector<std::jthread> m_Workers;
    std::queue<std::function<void()>> m_Tasks;
    
    std::mutex m_QueueMutex;
    std::condition_variable m_Condition;
    bool m_Stop;
};

} // namespace LibCommons
