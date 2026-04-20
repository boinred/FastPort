// ServerStatsCollector.ixx
// -----------------------------------------------------------------------------
// Design Ref: server-status §3.3, §4.2 — 서버 전역 통계 집계.
// 의존성: StatsSampler (CPU/Memory 캐시), SnapshotProvider (세션 목록), IdleCountProvider.
// 반환: POD struct (protobuf 의존 없음). 프로토콜 변환은 AdminPacketHandler 담당.
// -----------------------------------------------------------------------------
module;

#include <cstdint>

export module networks.stats.server_stats_collector;

import std;
import networks.sessions.isession_stats;
import networks.stats.stats_sampler;


namespace LibNetworks::Stats
{

// Design Ref: server-status §3.1 — 서버 모드 식별. proto 의 ServerMode 와 값 일치.
export enum class ServerMode : std::uint8_t
{
    Unknown = 0,
    IOCP    = 1,
    RIO     = 2,
};


// Design Ref: server-status §4.2 — Summary 응답용 POD.
export struct SummaryData
{
    std::int64_t  uptimeMs            = 0;
    std::uint32_t activeSessionCount  = 0;
    std::uint64_t totalRxBytes        = 0;
    std::uint64_t totalTxBytes        = 0;
    std::uint64_t idleDisconnectCount = 0;
    ServerMode    serverMode          = ServerMode::Unknown;
    std::uint64_t processMemoryBytes  = 0;
    double        processCpuPercent   = 0.0;
    std::int64_t  serverTimestampMs   = 0;  // Unix epoch ms (시계 동기용)
};


// 세션 1개 정보 (SessionList 용).
export struct SessionInfoData
{
    std::uint64_t sessionId   = 0;
    std::int64_t  lastRecvMs  = 0;
    std::uint64_t rxBytes     = 0;
    std::uint64_t txBytes     = 0;
};


// 페이지 응답.
export struct SessionListData
{
    std::uint32_t                total   = 0;
    std::uint32_t                offset  = 0;
    std::vector<SessionInfoData> sessions;
};


// Design Ref: server-status §4.2 — Collector. Start/Stop 없음 (StatsSampler 가 담당).
// SnapshotProvider 와 IdleCountProvider 는 소비자가 제공 (IOCPServiceMode 등).
// IIdleAware 구현체는 선택적 — ISessionStats 만 요구.
export class ServerStatsCollector
{
public:
    // Provider 가 매 호출마다 현재 활성 세션 스냅샷을 반환.
    using SnapshotProvider = std::function<
        std::vector<std::shared_ptr<Sessions::ISessionStats>>()>;

    // Idle disconnect 카운트 제공자 (SessionIdleChecker::GetDisconnectCount). nullable.
    using IdleCountProvider = std::function<std::uint64_t()>;

    ServerStatsCollector(
        ServerMode        serverMode,
        SnapshotProvider  sessionProvider,
        IdleCountProvider idleCountProvider,
        StatsSampler*     pSampler);  // non-owning

    // 가벼운 숫자 위주 Summary (폴링 경로).
    SummaryData SnapshotSummary() const;

    // 페이지네이션 세션 목록 (명시 요청 경로).
    SessionListData SnapshotSessions(std::uint32_t offset, std::uint32_t limit) const;

    // limit 상한 (clamp).
    static constexpr std::uint32_t kMaxLimit = 1000;

private:
    ServerMode        m_ServerMode;
    SnapshotProvider  m_SessionProvider;
    IdleCountProvider m_IdleCountProvider;
    StatsSampler*     m_pSampler;    // nullable — 없으면 CPU/Memory = 0

    // 시작 시각 (steady_clock epoch-ms). Uptime 계산 기준점.
    std::int64_t m_StartSteadyMs;
};

} // namespace LibNetworks::Stats
