// SessionIdleChecker.ixx
// -----------------------------------------------------------------------------
// Design Ref: session-idle-timeout §3.3, §4.3 — SessionIdleChecker 공개 인터페이스.
//
// 역할:
//   - TimerQueue::GetInstance() 에 periodic tick 을 등록.
//   - SnapshotProvider 콜백을 통해 IIdleAware 세션 스냅샷 수집 (타입 불변 non-template).
//   - threshold 초과한 세션에 RequestDisconnect(IdleTimeout) 호출.
//
// Thread-safety:
//   - Start/Stop 은 소유자(단일 스레드)에서 호출.
//   - OnTick 은 TimerQueue 워커 스레드에서 호출 → m_Running atomic read 로 late check.
//   - Shutdown 시 TimerQueue::Cancel(wait=true) 로 진행 중 tick 완료 대기.
// -----------------------------------------------------------------------------
module;

#include <cstdint>

export module networks.sessions.idle_checker;

import std;
import networks.sessions.iidle_aware;


namespace LibNetworks::Sessions
{

// Design Ref: §3.3 — idle 검사 설정. 소유자가 생성자 주입.
export struct IdleCheckerConfig
{
    std::chrono::milliseconds thresholdMs    { 10000 };  // idle 간주 임계값
    std::chrono::milliseconds tickIntervalMs { 1000 };   // 주기 검사 간격
    bool                      enabled        { true };   // false 면 Start 가 no-op
};


// Design Ref: §4.3 — non-template IdleChecker. IIdleAware 인터페이스만 의존.
export class SessionIdleChecker
{
public:
    // 매 tick 마다 현재 활성 세션의 IIdleAware 스냅샷을 반환해야 함.
    // 호출자(IOCPServiceMode)가 SessionContainer 를 참조하는 람다로 주입.
    using SnapshotProvider = std::function<std::vector<std::shared_ptr<IIdleAware>>()>;

    SessionIdleChecker(IdleCheckerConfig cfg, SnapshotProvider provider);
    ~SessionIdleChecker();

    SessionIdleChecker(const SessionIdleChecker&)            = delete;
    SessionIdleChecker& operator=(const SessionIdleChecker&) = delete;
    SessionIdleChecker(SessionIdleChecker&&)                 = delete;
    SessionIdleChecker& operator=(SessionIdleChecker&&)      = delete;

    // TimerQueue 에 SchedulePeriodic 등록. enabled=false 면 no-op.
    // 이중 호출은 idempotent.
    void Start();

    // 진행 중 tick 완료까지 대기 후 정리. 이중 호출은 idempotent.
    void Stop();

    // 현재 설정 조회 (immutable).
    const IdleCheckerConfig& GetConfig() const noexcept { return m_Config; }

    // 지금까지 idle 로 disconnect 요청한 세션 수 (누적, 관측용).
    std::uint64_t GetDisconnectCount() const noexcept
    {
        return m_DisconnectCount.load(std::memory_order_relaxed);
    }

private:
    // TimerQueue tick 콜백 본체. Start 에서 람다 캡처로 호출.
    void OnTick();

    IdleCheckerConfig               m_Config;
    SnapshotProvider                m_Provider;
    std::atomic<std::uint64_t>      m_TimerId         { 0 };  // 0 = kInvalidTimerId
    std::atomic<bool>               m_Running         { false };
    std::atomic<std::uint64_t>      m_DisconnectCount { 0 };
};

} // namespace LibNetworks::Sessions
