module;

#include <unordered_map>
#include <memory>
#include <utility>
#include <functional>
#include <type_traits>

export module commons.event_listener;


import commons.singleton;
import commons.thread_pool;

namespace LibCommons
{

export class EventListener : public SingleTon<EventListener>
{
    friend class SingleTon<EventListener>;

private:
    EventListener() = default;
    
public:
    ~EventListener()
    {
        Stop();
    }

    void Init(size_t threadCount)
    {
        if (m_pThreadPool)
        {
            return;
        }

        m_pThreadPool = std::make_unique<ThreadPool>(threadCount);
    }

    void Stop()
    {
        if (m_pThreadPool)
        {
            m_pThreadPool->Stop();
            m_pThreadPool.reset(); 
        }
    }

    // 특정 역할을(작업을) 다른 쓰레드에 전달합니다.
    void PostTask(std::function<void()> task)
    {
        if (m_pThreadPool)
        {
            m_pThreadPool->Enqueue(std::move(task));
        }
    }

private:
    std::unique_ptr<ThreadPool> m_pThreadPool;
};

} // namespace LibCommons