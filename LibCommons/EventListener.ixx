module;

#include <unordered_map>
#include <memory>
#include <utility>
#include <functional>
#include <type_traits>
#include <future>

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
    void Enqueue(std::function<void()> task)
    {
        if (m_pThreadPool)
        {
            m_pThreadPool->Enqueue(std::move(task));
        }
    }

    template<class F, class R /* std::invoke_result_t<F> */>
    auto Enqueue(F&& function) -> std::future<R>
    {
        auto pTask = std::make_shared<std::packaged_task<R()>>(std::packaged_task<R()>(std::bind(std::forward<F>(function))));

        std::future<R> res = pTask->get_future();
        
        if(m_pThreadPool)
        {
            m_pThreadPool->Enqueue([pTask]() { (*pTask)(); });
        }

        return std::move(res);
    }

private:
    std::unique_ptr<ThreadPool> m_pThreadPool;
};

} // namespace LibCommons