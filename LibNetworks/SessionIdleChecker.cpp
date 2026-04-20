// SessionIdleChecker.cpp
// -----------------------------------------------------------------------------
// Design Ref: session-idle-timeout §2.2 Data flow, §4.3, §6.2 예외 정책.
//
// 구현 전략:
//   1. Start: enabled==true && m_Running CAS false→true 성공 시 TimerQueue 에 Periodic 등록.
//   2. OnTick:
//      - m_Running late check (Stop 이 진행 중이면 조기 반환)
//      - m_Provider() 로 IIdleAware 스냅샷 수집 (예외 catch-all)
//      - 각 IIdleAware: last==0 skip, elapsed >= threshold 면 RequestDisconnect(IdleTimeout)
//      - RequestDisconnect 예외도 catch-all (한 세션 오류가 다른 세션 처리 막지 않음)
//   3. Stop: m_Running=false + TimerQueue::Cancel(wait=true) → 진행 중 tick 완료 대기.
//
// 로깅: Logger "IdleChecker" 카테고리. Logger 다중 호출 ICE 방지 위해 spdlog 헤더를 GMF 에 포함
// (CLAUDE.md 로깅 지침 준수).
// -----------------------------------------------------------------------------
module;

#include <cstdint>
#include <chrono>
#include <spdlog/spdlog.h>  // Logger 템플릿 ICE 회피 (CLAUDE.md 지침)

module networks.sessions.idle_checker;

import std;
import commons.logger;
import commons.timer_queue;
import networks.sessions.iidle_aware;
import networks.sessions.inetwork_session;  // DisconnectReason


namespace LibNetworks::Sessions
{

namespace
{
constexpr const char* kLogCategory = "IdleChecker";

// 로깅 헬퍼 — 호출부에서 std::format 으로 구성 후 전달.
// 같은 시그니처(std::string 1개) 로 템플릿 인스턴스화 수 최소화.
inline void LogInfo(const std::string& msg)    { LibCommons::Logger::GetInstance().LogInfo(kLogCategory, msg); }
inline void LogWarning(const std::string& msg) { LibCommons::Logger::GetInstance().LogWarning(kLogCategory, msg); }
inline void LogError(const std::string& msg)   { LibCommons::Logger::GetInstance().LogError(kLogCategory, msg); }

// steady_clock 기준 epoch-ms (IOSession 의 NowMs 와 동일 기준).
inline std::int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // anonymous namespace


// Design Ref: §4.3 ctor — Config/Provider 를 저장만. 실제 타이머 등록은 Start() 에서.
SessionIdleChecker::SessionIdleChecker(IdleCheckerConfig cfg, SnapshotProvider provider)
    : m_Config(cfg)
    , m_Provider(std::move(provider))
{
}


// Design Ref: §6.1 Error #7/8 — 소멸 시 자동 Stop (idempotent).
SessionIdleChecker::~SessionIdleChecker()
{
    Stop();
}


// Design Ref: §4.3, §6.1 Error #1/#2 — enabled 플래그 + 이중 호출 방지.
void SessionIdleChecker::Start()
{
    if (!m_Config.enabled)
    {
        LogInfo("Disabled, skip scheduling");
        return;
    }

    bool expected = false;
    if (!m_Running.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire))
    {
        LogInfo("Already running");
        return;
    }

    // TimerQueue 싱글톤에 Periodic tick 등록.
    // 람다는 this 를 캡처 — Stop 에서 Cancel(wait=true) 로 진행 중 콜백 종료 후에만
    // 소멸 가능하므로 this 수명 안전.
    auto& tq = LibCommons::TimerQueue::GetInstance();
    const auto id = tq.SchedulePeriodic(
        m_Config.tickIntervalMs,
        [this]() { this->OnTick(); },
        "SessionIdleChecker");

    m_TimerId.store(static_cast<std::uint64_t>(id), std::memory_order_release);

    LogInfo(std::format("Started. ThresholdMs : {}, TickIntervalMs : {}",
        m_Config.thresholdMs.count(), m_Config.tickIntervalMs.count()));
}


// Design Ref: §2.2 Shutdown sequence — Cancel(wait=true) 로 진행 중 tick 완료 대기.
void SessionIdleChecker::Stop()
{
    // 이중 호출 방지: m_Running 을 false 로 먼저 전환 (OnTick 이 late check 하도록).
    bool wasRunning = m_Running.exchange(false, std::memory_order_acq_rel);
    if (!wasRunning)
    {
        return;
    }

    const auto id = static_cast<LibCommons::TimerId>(
        m_TimerId.exchange(0, std::memory_order_acq_rel));

    if (id != LibCommons::kInvalidTimerId)
    {
        // Wait path — 진행 중 콜백 완료 대기.
        LibCommons::TimerQueue::GetInstance().Cancel(id);
    }

    LogInfo(std::format("Stopped. Disconnected total : {}", m_DisconnectCount.load()));
}


// Design Ref: §2.2 Callback flow, §6.2 Exception policy.
void SessionIdleChecker::OnTick()
{
    // 1. Late check — Stop 진행 중이면 스킵.
    if (!m_Running.load(std::memory_order_acquire))
    {
        return;
    }

    // 2. 스냅샷 수집 (Provider 예외는 catch, 다음 tick 유지).
    std::vector<std::shared_ptr<IIdleAware>> snapshot;
    try
    {
        snapshot = m_Provider();
    }
    catch (const std::exception& e)
    {
        LogError(std::format("Snapshot provider threw: {}", e.what()));
        return;
    }
    catch (...)
    {
        LogError("Snapshot provider threw unknown");
        return;
    }

    const auto   nowMs       = NowMs();
    const auto   thresholdMs = m_Config.thresholdMs.count();

    // 3. 각 세션 검사. 한 세션 예외가 다른 세션 처리 방해하지 않도록 per-session try/catch.
    for (auto const& pSession : snapshot)
    {
        if (!pSession) continue;

        const auto last = pSession->GetLastRecvTimeMs();
        if (last == 0) continue;  // 아직 수신 이력 없음 — 연결 직후 세션

        const auto elapsed = nowMs - last;
        if (elapsed < thresholdMs) continue;

        try
        {
            pSession->RequestDisconnect(DisconnectReason::IdleTimeout);
            m_DisconnectCount.fetch_add(1, std::memory_order_relaxed);
        }
        catch (const std::exception& e)
        {
            LogError(std::format("RequestDisconnect threw: {}", e.what()));
        }
        catch (...)
        {
            LogError("RequestDisconnect threw unknown");
        }
    }
}

} // namespace LibNetworks::Sessions
