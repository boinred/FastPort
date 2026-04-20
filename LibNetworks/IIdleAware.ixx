// IIdleAware.ixx
// -----------------------------------------------------------------------------
// Design Ref: session-idle-timeout §3.2 — SessionIdleChecker 가 의존하는 세션
// 추상화. 구체 세션 타입(IOCP/RIO/Outbound)을 모르게 하여 checker 재사용.
//
// 사용 지침:
//   - GetLastRecvTimeMs: steady_clock 기준 epoch-ms. 0 이면 아직 수신 이력 없음(연결 직후).
//   - RequestDisconnect(reason): 이중 호출은 구현체가 방지 (IOSession 의 m_DisconnectRequested CAS).
//
// Thread-safety 계약:
//   - GetLastRecvTimeMs 는 tick 콜백(TimerQueue 워커) 과 수신 경로(IOCP 워커) 에서 concurrent
//     접근 → atomic read 로 구현 필수.
//   - RequestDisconnect 도 tick 콜백에서 호출되므로 구현체가 thread-safe 해야 한다.
// -----------------------------------------------------------------------------
module;

#include <cstdint>

export module networks.sessions.iidle_aware;

import networks.sessions.inetwork_session;  // DisconnectReason

namespace LibNetworks::Sessions
{

// Idle 감지 가능한 세션이 구현해야 하는 인터페이스.
// Design Ref: session-idle-timeout §3.2, §4.3.
export struct IIdleAware
{
    virtual ~IIdleAware() = default;

    // 마지막 수신 시각 (steady_clock 기준 ms). 0 은 "아직 수신 없음" 을 의미하며
    // idle 검사에서 skip 되어야 한다.
    // Thread-safety: lock-free atomic read 권장.
    virtual std::int64_t GetLastRecvTimeMs() const noexcept = 0;

    // 지정한 사유로 연결 종료를 요청. 내부적으로 이중 호출 방지 CAS 필수.
    // Thread-safety: 다중 스레드에서 동시 호출 가능.
    virtual void RequestDisconnect(DisconnectReason reason) = 0;
};

} // namespace LibNetworks::Sessions
