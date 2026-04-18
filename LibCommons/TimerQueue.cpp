module;

#include <Windows.h>
#include <threadpoolapiset.h>
#include <spdlog/spdlog.h>

module commons.timer_queue;

import std;

// Design Ref: §2 Option B (Clean) — Scope core,schedule: Schedule/Callback 핵심 경로 구현.
// Cancel / Shutdown / Sharding / ScopedTimer 실제 연동은 다음 --scope cancel,lifecycle 에서 구현.
//
// NOTE (MSVC workaround): LibCommons::Logger 의 가변 템플릿 Log 메서드를 모듈 구현 단위에서
// 호출하면 MSVC 17.x/18.x (VC Tools 14.50.35717) 에서 C1001 ICE 발생. 이를 회피하기 위해
// spdlog 를 직접 사용한다. LibCommons::Logger 가 기본 로거를 초기화해 둔 상태를 가정.
//
// Plan SC: FR-01 (ScheduleOnce), FR-04 (람다 오버로드), FR-05 (Command 오버로드),
//          FR-06 (Name 로깅), FR-09 (Logger 연동 — spdlog 로 대체 연동).

namespace LibCommons::detail
{

enum class EntryState : std::uint8_t
{
    Scheduled = 0,
    Running   = 1,
    Completed = 2,
    Cancelled = 3,
};

enum class QueueState : std::uint8_t
{
    Running      = 0,
    ShuttingDown = 1,
    Dead         = 2,
};

struct TimerJob
{
    std::function<void()> invoke;
    std::string           name;
};

struct Entry
{
    TimerId                 id          = kInvalidTimerId;
    PTP_TIMER               handle      = nullptr;
    TimerJob                job;
    std::atomic<EntryState> state       = EntryState::Scheduled;
    bool                    isPeriodic  = false;
    Duration                interval    = Duration::zero();
    TimerQueue::Impl*       owner       = nullptr;
};

// Design Ref: §2.2 — Windows 상대 지연(100ns 단위 음수 FILETIME).
inline FILETIME MakeRelativeDueTime(Duration delay) noexcept
{
    const auto hundredNs = -static_cast<LONGLONG>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count() / 100);

    ULARGE_INTEGER ul{};
    ul.QuadPart = static_cast<ULONGLONG>(hundredNs);

    FILETIME ft{};
    ft.dwLowDateTime  = ul.LowPart;
    ft.dwHighDateTime = ul.HighPart;
    return ft;
}

} // namespace LibCommons::detail


namespace LibCommons
{

// Design Ref: §3 — 1차 구현: 단일 mutex + unordered_map. 샤딩은 다음 스코프에서 도입.
struct TimerQueue::Impl
{
    std::mutex                                                   m_MapMutex;
    std::unordered_map<TimerId, std::unique_ptr<detail::Entry>>  m_Entries;
    std::atomic<TimerId>                                         m_NextId { 1 };
    std::atomic<detail::QueueState>                              m_QueueState { detail::QueueState::Running };

    TimerId AllocateId() noexcept
    {
        TimerId id = m_NextId.fetch_add(1, std::memory_order_relaxed);
        if (id == kInvalidTimerId)
        {
            id = m_NextId.fetch_add(1, std::memory_order_relaxed);
        }
        return id;
    }

    TimerId ScheduleImpl(Duration delay, detail::TimerJob job, bool isPeriodic, Duration interval);

    void RunEntry(detail::Entry& entry);
};


namespace
{

// Design Ref: §2.2 Callback flow — TP trampoline.
void CALLBACK TpTimerCallback(PTP_CALLBACK_INSTANCE /*pInstance*/, PVOID pContext, PTP_TIMER /*pTimer*/)
{
    auto* pEntry = static_cast<detail::Entry*>(pContext);
    if (pEntry == nullptr || pEntry->owner == nullptr)
    {
        return;
    }
    pEntry->owner->RunEntry(*pEntry);
}

} // anonymous namespace


TimerId TimerQueue::Impl::ScheduleImpl(Duration delay, detail::TimerJob job, bool isPeriodic, Duration interval)
{
    // Design Ref: §6.1 Error #1 — Shutdown 후 Schedule 거부.
    if (m_QueueState.load(std::memory_order_acquire) != detail::QueueState::Running)
    {
        spdlog::warn("[TimerQueue] Schedule after Shutdown rejected. Name : {}", job.name);
        return kInvalidTimerId;
    }

    const TimerId id = AllocateId();

    auto pEntry = std::make_unique<detail::Entry>();
    pEntry->id         = id;
    pEntry->handle     = nullptr;
    pEntry->job        = std::move(job);
    pEntry->state.store(detail::EntryState::Scheduled, std::memory_order_relaxed);
    pEntry->isPeriodic = isPeriodic;
    pEntry->interval   = interval;
    pEntry->owner      = this;

    detail::Entry* pRaw = pEntry.get();

    // Design Ref: §6.1 Error #2 — CreateThreadpoolTimer 실패 시 롤백.
    PTP_TIMER hTimer = ::CreateThreadpoolTimer(TpTimerCallback, pRaw, nullptr);
    if (hTimer == nullptr)
    {
        spdlog::error("[TimerQueue] CreateThreadpoolTimer failed. Name : {}, GLE : {}",
            pRaw->job.name, ::GetLastError());
        return kInvalidTimerId;
    }

    pEntry->handle = hTimer;

    {
        std::lock_guard<std::mutex> lock(m_MapMutex);
        m_Entries.emplace(id, std::move(pEntry));
    }

    FILETIME    due    = detail::MakeRelativeDueTime(delay);
    const DWORD period = isPeriodic
        ? static_cast<DWORD>(std::max<std::int64_t>(0, interval.count()))
        : 0u;

    ::SetThreadpoolTimer(hTimer, &due, period, 0);

    spdlog::debug("[TimerQueue] Scheduled. Id : {}, Name : {}, DelayMs : {}, Periodic : {}, IntervalMs : {}",
        id, pRaw->job.name, delay.count(), isPeriodic, interval.count());

    return id;
}


void TimerQueue::Impl::RunEntry(detail::Entry& entry)
{
    // Design Ref: §3.2 — Scheduled → Running CAS. Q3-a 겹침 허용으로 실패해도 진행.
    detail::EntryState expected = detail::EntryState::Scheduled;
    entry.state.compare_exchange_strong(
        expected, detail::EntryState::Running,
        std::memory_order_acq_rel, std::memory_order_acquire);

    if (expected == detail::EntryState::Cancelled)
    {
        return;
    }

    // Design Ref: §6.3 — Callback exception policy. 전역 catch-all.
    try
    {
        entry.job.invoke();
    }
    catch (const std::exception& e)
    {
        spdlog::error("[TimerQueue] Callback threw. Name : {}, What : {}", entry.job.name, e.what());
    }
    catch (...)
    {
        spdlog::error("[TimerQueue] Callback threw unknown. Name : {}", entry.job.name);
    }

    if (!entry.isPeriodic)
    {
        entry.state.store(detail::EntryState::Completed, std::memory_order_release);
        // one-shot 엔트리는 다음 스코프의 Cancel/cleanup 에서 정리됨.
        // 이번 스코프에서는 Completed 상태로만 두고, 메모리는 TimerQueue 소멸 시 해제.
    }
    else
    {
        detail::EntryState runningState = detail::EntryState::Running;
        entry.state.compare_exchange_strong(
            runningState, detail::EntryState::Scheduled,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }
}


// ========================================================================
// TimerQueue public API
// ========================================================================

TimerQueue::TimerQueue()
    : m_pImpl(std::make_unique<Impl>())
{
}


TimerQueue::~TimerQueue()
{
    // Design Ref: §5 — Cancel/Shutdown public API 는 이번 스코프에서 stub 이지만, 소멸 시
    // 활성 PTP_TIMER 가 남아 있으면 TP 콜백이 해제된 Entry 를 참조하여 UAF 가 발생한다.
    // 따라서 소멸자에서는 최소한 OS 자원(PTP_TIMER)만 동기적으로 정리한다.
    // 전체 Shutdown/Cancel 플로우(상태 전이, selfCancel 처리 등)는 다음 cancel,lifecycle 스코프.
    if (!m_pImpl)
    {
        return;
    }

    std::vector<PTP_TIMER> handles;
    {
        std::lock_guard<std::mutex> lock(m_pImpl->m_MapMutex);
        handles.reserve(m_pImpl->m_Entries.size());
        for (auto& entryPair : m_pImpl->m_Entries)
        {
            if (entryPair.second && entryPair.second->handle != nullptr)
            {
                handles.push_back(entryPair.second->handle);
                entryPair.second->handle = nullptr;
            }
        }
    }

    for (PTP_TIMER h : handles)
    {
        ::SetThreadpoolTimer(h, nullptr, 0, 0);
        ::WaitForThreadpoolTimerCallbacks(h, TRUE);
        ::CloseThreadpoolTimer(h);
    }
}


TimerId TimerQueue::ScheduleOnce(Duration delay,
                                 std::function<void()> task,
                                 std::string_view name)
{
    detail::TimerJob job;
    job.invoke = std::move(task);
    job.name.assign(name);
    return m_pImpl->ScheduleImpl(delay, std::move(job), /*isPeriodic=*/false, Duration::zero());
}


TimerId TimerQueue::SchedulePeriodic(Duration interval,
                                     std::function<void()> task,
                                     std::string_view name)
{
    detail::TimerJob job;
    job.invoke = std::move(task);
    job.name.assign(name);
    return m_pImpl->ScheduleImpl(interval, std::move(job), /*isPeriodic=*/true, interval);
}


TimerId TimerQueue::ScheduleOnce(Duration delay, std::unique_ptr<ITimerCommand> cmd)
{
    if (!cmd)
    {
        return kInvalidTimerId;
    }

    // Design Ref: §4.1 — ITimerCommand 를 람다로 감싸 TimerJob 통일 경로.
    // std::function 은 copy-constructible 이 필요하므로 shared_ptr 로 소유권 이전.
    std::string nameCopy(cmd->Name());
    auto pCmd = std::shared_ptr<ITimerCommand>(std::move(cmd));

    return ScheduleOnce(
        delay,
        [pCmd]() { pCmd->Execute(); },
        nameCopy);
}


TimerId TimerQueue::SchedulePeriodic(Duration interval, std::unique_ptr<ITimerCommand> cmd)
{
    if (!cmd)
    {
        return kInvalidTimerId;
    }

    std::string nameCopy(cmd->Name());
    auto pCmd = std::shared_ptr<ITimerCommand>(std::move(cmd));

    return SchedulePeriodic(
        interval,
        [pCmd]() { pCmd->Execute(); },
        nameCopy);
}


// STUB — next scope cancel,lifecycle
bool TimerQueue::Cancel(TimerId /*id*/)
{
    spdlog::warn("[TimerQueue] Cancel not yet implemented in core,schedule scope");
    return false;
}


// STUB — next scope cancel,lifecycle
void TimerQueue::Shutdown(bool /*waitForCallbacks*/)
{
    spdlog::info("[TimerQueue] Shutdown not yet implemented in core,schedule scope");
}


// ========================================================================
// ScopedTimer
// ========================================================================

ScopedTimer::ScopedTimer(TimerQueue& queue, TimerId id) noexcept
    : m_pQueue(&queue), m_Id(id)
{
}


ScopedTimer::~ScopedTimer()
{
    if (m_pQueue != nullptr && m_Id != kInvalidTimerId)
    {
        m_pQueue->Cancel(m_Id);
    }
}


ScopedTimer::ScopedTimer(ScopedTimer&& other) noexcept
    : m_pQueue(other.m_pQueue), m_Id(other.m_Id)
{
    other.m_pQueue = nullptr;
    other.m_Id     = kInvalidTimerId;
}


ScopedTimer& ScopedTimer::operator=(ScopedTimer&& other) noexcept
{
    if (this != &other)
    {
        if (m_pQueue != nullptr && m_Id != kInvalidTimerId)
        {
            m_pQueue->Cancel(m_Id);
        }
        m_pQueue       = other.m_pQueue;
        m_Id           = other.m_Id;
        other.m_pQueue = nullptr;
        other.m_Id     = kInvalidTimerId;
    }
    return *this;
}


TimerId ScopedTimer::Release() noexcept
{
    TimerId id = m_Id;
    m_pQueue   = nullptr;
    m_Id       = kInvalidTimerId;
    return id;
}

} // namespace LibCommons
