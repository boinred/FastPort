// ISessionStats.ixx
// -----------------------------------------------------------------------------
// Design Ref: server-status §3.3 — 세션별 통계 접근 추상화.
// IOSession/RIOSession 이 구현하여 ServerStatsCollector 가 구체 타입 몰라도 수집 가능.
//
// Thread-safety: 구현체는 atomic read 로 lock-free 제공 권장.
// -----------------------------------------------------------------------------
module;

#include <cstdint>

export module networks.sessions.isession_stats;

namespace LibNetworks::Sessions
{

export struct ISessionStats
{
    virtual ~ISessionStats() = default;

    // 세션 생성 이후 누적 수신 바이트 (ok = OnIOCompleted 에서 bytes > 0 성공 경로).
    virtual std::uint64_t GetTotalRxBytes() const noexcept = 0;

    // 세션 생성 이후 누적 송신 바이트 (ok = OnIOCompleted 에서 Send 완료 성공 경로).
    virtual std::uint64_t GetTotalTxBytes() const noexcept = 0;
};

} // namespace LibNetworks::Sessions
