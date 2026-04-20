module;

#include <Windows.h>
#include <threadpoolapiset.h>
// CRITICAL: LibCommons::Logger 의 가변 템플릿 LogXxx 는 spdlog 타입(string_view_t, fmt_lib 등)을
// 직접 참조한다. `import commons.logger;` 만 사용하면 모듈 경계에서 spdlog 심볼이 완전히 노출되지
// 않아 템플릿 인스턴스화 시 MSVC C1001 ICE 를 유발한다. GMF 에서 spdlog 헤더를 include 해
// 모든 심볼을 완전히 가시화해야 한다.
#include <spdlog/spdlog.h>

module commons.timer_queue;

import std;
import commons.logger;

// Design Ref: §2 Option B (Clean) — PImpl + 상태머신 + ScopedTimer + Cancel/Shutdown.
// Plan SC: FR-01 (ScheduleOnce), FR-02 (SchedulePeriodic), FR-03 (Cancel),
//          FR-04 (람다 오버로드), FR-05 (Command 오버로드), FR-06 (Name 로깅),
//          FR-07 (RAII 정리), FR-08 (Shutdown), FR-09 (Logger 연동), FR-10 (GetInstance).

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

namespace
{
constexpr const char* kLogCategory = "TimerQueue";

// 로깅 헬퍼 — 모든 레벨을 LibCommons::Logger 로 통일.
// 호출부는 std::format 으로 메시지를 미리 구성하여 전달 → 템플릿 인스턴스화는 호출당 1회로 단순.
inline void LogTQInfo(const std::string& msg)    { LibCommons::Logger::GetInstance().LogInfo(kLogCategory, msg); }
inline void LogTQWarning(const std::string& msg) { LibCommons::Logger::GetInstance().LogWarning(kLogCategory, msg); }
inline void LogTQError(const std::string& msg)   { LibCommons::Logger::GetInstance().LogError(kLogCategory, msg); }
inline void LogTQDebug(const std::string& msg)   { LibCommons::Logger::GetInstance().LogDebug(kLogCategory, msg); }
} // anonymous namespace


// Design Ref: §3 — 단일 mutex + unordered_map. 샤딩은 후속 최적화 스코프로 분리.
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

    // Design Ref: §2.2 Cancel flow — Fast(Scheduled→Cancelled CAS) / Wait(진행 중 콜백 완료 대기) path.
    // selfCancel=true 면 TP 콜백이 자기 엔트리를 취소하는 것이므로 WaitForThreadpoolTimerCallbacks 에
    // FALSE 전달(데드락 방지).
    bool CancelImpl(TimerId id, bool selfCancel);

    // Design Ref: §3.2 QueueState — Running → ShuttingDown → Dead.
    void ShutdownImpl(bool waitForCallbacks);
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
        LogTQWarning(std::format("Schedule after Shutdown rejected. Name : {}", job.name));
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
        LogTQError(std::format("CreateThreadpoolTimer failed. Name : {}, GLE : {}",
            pRaw->job.name, ::GetLastError()));
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

    LogTQDebug(std::format("Scheduled. Id : {}, Name : {}, DelayMs : {}, Periodic : {}, IntervalMs : {}",
        id, pRaw->job.name, delay.count(), isPeriodic, interval.count()));

    return id;
}


void TimerQueue::Impl::RunEntry(detail::Entry& entry)
{
    // Design Ref: §3.2 — Scheduled → Running CAS. Q3-a 겹침 허용으로 실패해도 진행.
    detail::EntryState expected = detail::EntryState::Scheduled;
    entry.state.compare_exchange_strong(
        expected, detail::EntryState::Running,
        std::memory_order_acq_rel, std::memory_order_acquire);

    // Design Ref: §3.2 — Cancelled 상태이면 실행 스킵.
    if (expected == detail::EntryState::Cancelled)
    {
        return;
    }

    // Design Ref: §6.3 — Callback exception policy. 전역 catch-all 로 서버 보호.
    try
    {
        entry.job.invoke();
    }
    catch (const std::exception& e)
    {
        LogTQError(std::format("Callback threw. Name : {}, What : {}", entry.job.name, e.what()));
    }
    catch (...)
    {
        LogTQError(std::format("Callback threw unknown. Name : {}", entry.job.name));
    }

    if (!entry.isPeriodic)
    {
        // one-shot 엔트리: Completed 로 표기. 실제 맵 제거는 Cancel/Shutdown/소멸자에서 일괄 처리.
        entry.state.store(detail::EntryState::Completed, std::memory_order_release);
    }
    else
    {
        // periodic: Running → Scheduled 로 되돌려 다음 tick 대기.
        // Cancel 이 끼어들어 Cancelled 가 된 경우 CAS 실패 → 그대로 둠 (Cancel 이 뒤처리).
        detail::EntryState runningState = detail::EntryState::Running;
        entry.state.compare_exchange_strong(
            runningState, detail::EntryState::Scheduled,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }
}


// Design Ref: §2.2 Cancel flow — Fast/Wait path + selfCancel 데드락 방지.
bool TimerQueue::Impl::CancelImpl(TimerId id, bool selfCancel)
{
    if (id == kInvalidTimerId)
    {
        return false;
    }

    // 맵에서 소유권 이전 (찾지 못하면 false).
    std::unique_ptr<detail::Entry> pEntry;
    {
        std::lock_guard<std::mutex> lock(m_MapMutex);
        auto it = m_Entries.find(id);
        if (it == m_Entries.end())
        {
            return false;
        }
        pEntry = std::move(it->second);
        m_Entries.erase(it);
    }

    if (!pEntry)
    {
        return false;
    }

    // 상태 표기: Cancelled. 현재 실행 중인 콜백은 상태 체크로 조기 반환하지 않지만,
    // periodic 콜백의 재-Scheduled CAS 가 실패하여 추가 발사가 차단된다.
    pEntry->state.store(detail::EntryState::Cancelled, std::memory_order_release);

    // Design Ref: §4.4 — Self-cancel 데드락 방지: TRUE 대신 FALSE 전달.
    //   TRUE 전달 시 현재 실행 중인 동일 콜백의 완료를 기다리다 교착.
    PTP_TIMER h = pEntry->handle;
    pEntry->handle = nullptr;

    if (h != nullptr)
    {
        ::SetThreadpoolTimer(h, nullptr, 0, 0);
        ::WaitForThreadpoolTimerCallbacks(h, selfCancel ? FALSE : TRUE);
        ::CloseThreadpoolTimer(h);
    }

    LogTQDebug(std::format("Cancelled. Id : {}, Name : {}, SelfCancel : {}",
        id, pEntry->job.name, selfCancel));

    return true;
}


void TimerQueue::Impl::ShutdownImpl(bool waitForCallbacks)
{
    // Design Ref: §3.2 QueueState — Running → ShuttingDown (idempotent).
    detail::QueueState expected = detail::QueueState::Running;
    if (!m_QueueState.compare_exchange_strong(
            expected, detail::QueueState::ShuttingDown,
            std::memory_order_acq_rel, std::memory_order_acquire))
    {
        LogTQInfo(std::format("Shutdown skipped (already invoked). State : {}",
            static_cast<int>(expected)));
        return;
    }

    LogTQInfo(std::format("Shutdown started. WaitForCallbacks : {}", waitForCallbacks));

    // 활성 ID 목록 스냅샷 후 개별 Cancel.
    std::vector<TimerId> ids;
    {
        std::lock_guard<std::mutex> lock(m_MapMutex);
        ids.reserve(m_Entries.size());
        for (auto const& entryPair : m_Entries)
        {
            ids.push_back(entryPair.first);
        }
    }

    for (TimerId id : ids)
    {
        // waitForCallbacks=false 일 때는 Wait 를 생략(selfCancel=true 의미로 재사용).
        CancelImpl(id, /*selfCancel=*/!waitForCallbacks);
    }

    m_QueueState.store(detail::QueueState::Dead, std::memory_order_release);
    LogTQInfo(std::format("Shutdown completed. Cancelled : {}", ids.size()));
}


// ========================================================================
// TimerQueue public API
// ========================================================================

TimerQueue::TimerQueue()
    : m_pImpl(std::make_unique<Impl>())
{
    // Logger 카테고리 등록(최초 1회, 이후는 spdlog 직접 경로).
    LogTQInfo("TimerQueue constructed");
}


TimerQueue::~TimerQueue()
{
    // Design Ref: §5 Risk — 소멸 중 콜백 접근 방지. Shutdown 먼저 (waitForCallbacks=true).
    if (m_pImpl)
    {
        m_pImpl->ShutdownImpl(/*waitForCallbacks=*/true);
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
    // std::function 은 copy-constructible 필요 → shared_ptr 로 소유권 이전.
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


bool TimerQueue::Cancel(TimerId id)
{
    // 외부 호출은 기본적으로 non-self-cancel. 콜백 내부의 셀프 취소는 내부 경로에서 플래그 전달.
    return m_pImpl->CancelImpl(id, /*selfCancel=*/false);
}


void TimerQueue::Shutdown(bool waitForCallbacks)
{
    m_pImpl->ShutdownImpl(waitForCallbacks);
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
