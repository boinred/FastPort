// ServerStatsCollector.cpp
// -----------------------------------------------------------------------------
// Design Ref: server-status §4.2 — 집계 구현.
// -----------------------------------------------------------------------------
module;

#include <Windows.h>
#include <cstdint>
#include <spdlog/spdlog.h>

module networks.stats.server_stats_collector;

import std;
import commons.logger;
import networks.sessions.iidle_aware;
import networks.sessions.inetwork_session;


namespace LibNetworks::Stats
{

namespace
{
constexpr const char* kLogCategory = "ServerStats";

inline void LogError(const std::string& msg)
{
    LibCommons::Logger::GetInstance().LogError(kLogCategory, msg);
}

// steady_clock epoch-ms. Uptime 기준.
inline std::int64_t NowSteadyMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Unix epoch ms. 시계 동기 표시용.
inline std::int64_t NowWallMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // anonymous namespace


ServerStatsCollector::ServerStatsCollector(
    ServerMode serverMode,
    SnapshotProvider sessionProvider,
    IdleCountProvider idleCountProvider,
    StatsSampler* pSampler)
    : m_ServerMode(serverMode)
    , m_SessionProvider(std::move(sessionProvider))
    , m_IdleCountProvider(std::move(idleCountProvider))
    , m_pSampler(pSampler)
    , m_StartSteadyMs(NowSteadyMs())
{
}


SummaryData ServerStatsCollector::SnapshotSummary() const
{
    SummaryData out;
    out.serverMode        = m_ServerMode;
    out.uptimeMs          = NowSteadyMs() - m_StartSteadyMs;
    out.serverTimestampMs = NowWallMs();

    // 세션 합산.
    std::vector<std::shared_ptr<Sessions::ISessionStats>> snapshot;
    if (m_SessionProvider)
    {
        try
        {
            snapshot = m_SessionProvider();
        }
        catch (const std::exception& e)
        {
            LogError(std::format("Snapshot provider threw: {}", e.what()));
        }
    }

    out.activeSessionCount = static_cast<std::uint32_t>(snapshot.size());
    for (auto const& pSession : snapshot)
    {
        if (!pSession) continue;
        out.totalRxBytes += pSession->GetTotalRxBytes();
        out.totalTxBytes += pSession->GetTotalTxBytes();
    }

    // Idle disconnect 카운트.
    if (m_IdleCountProvider)
    {
        try
        {
            out.idleDisconnectCount = m_IdleCountProvider();
        }
        catch (...) { /* swallow */ }
    }

    // CPU/Memory (Sampler 가 캐시한 값).
    if (m_pSampler)
    {
        out.processCpuPercent  = m_pSampler->SnapshotCpuPercent();
        out.processMemoryBytes = m_pSampler->SnapshotMemoryBytes();
    }

    return out;
}


SessionListData ServerStatsCollector::SnapshotSessions(std::uint32_t offset, std::uint32_t limit) const
{
    SessionListData out;
    out.offset = offset;

    // clamp limit.
    if (limit == 0 || limit > kMaxLimit) limit = kMaxLimit;

    std::vector<std::shared_ptr<Sessions::ISessionStats>> snapshot;
    if (m_SessionProvider)
    {
        try
        {
            snapshot = m_SessionProvider();
        }
        catch (const std::exception& e)
        {
            LogError(std::format("Snapshot provider threw: {}", e.what()));
            return out;
        }
    }

    out.total = static_cast<std::uint32_t>(snapshot.size());
    if (offset >= out.total)
    {
        return out;  // empty sessions
    }

    const std::uint32_t endIdx = std::min<std::uint32_t>(offset + limit, out.total);
    out.sessions.reserve(endIdx - offset);

    for (std::uint32_t i = offset; i < endIdx; ++i)
    {
        auto const& pSession = snapshot[i];
        if (!pSession) continue;

        SessionInfoData info;
        // GetSessionId / GetLastRecvTimeMs 는 ISessionStats 에 없으므로 dynamic_cast 활용.
        // IOSession/RIOSession 의 INetworkSession/IIdleAware 를 통해 접근.
        if (auto const* pIdle = dynamic_cast<const Sessions::IIdleAware*>(pSession.get()))
        {
            info.lastRecvMs = pIdle->GetLastRecvTimeMs();
        }
        // session_id 는 ISessionStats 인터페이스 외. INetworkSession 로 시도.
        if (auto const* pNet = dynamic_cast<const Sessions::INetworkSession*>(pSession.get()))
        {
            info.sessionId = pNet->GetSessionId();
        }
        info.rxBytes = pSession->GetTotalRxBytes();
        info.txBytes = pSession->GetTotalTxBytes();
        out.sessions.push_back(info);
    }

    return out;
}

} // namespace LibNetworks::Stats
