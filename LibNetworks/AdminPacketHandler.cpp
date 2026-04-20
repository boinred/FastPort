// AdminPacketHandler.cpp
// -----------------------------------------------------------------------------
// Design Ref: server-status §2.2, §4.3 — Admin 패킷 처리 구현.
// proto 변환을 여기서 담당 (Collector 는 protobuf 독립 유지).
// -----------------------------------------------------------------------------
module;

#include <cstdint>
#include <spdlog/spdlog.h>
#include <Protocols/Admin.pb.h>
#include <Protocols/Commons.pb.h>

module networks.admin.admin_packet_handler;

import std;
import commons.logger;
import networks.sessions.inetwork_session;
import networks.core.packet;
import networks.stats.server_stats_collector;


namespace LibNetworks::Admin
{

namespace
{
constexpr const char* kLogCategory = "AdminHandler";

inline void LogDebug(const std::string& msg)   { LibCommons::Logger::GetInstance().LogDebug(kLogCategory, msg); }
inline void LogWarning(const std::string& msg) { LibCommons::Logger::GetInstance().LogWarning(kLogCategory, msg); }
inline void LogError(const std::string& msg)   { LibCommons::Logger::GetInstance().LogError(kLogCategory, msg); }

// ServerMode → proto enum 변환.
inline ::fastport::protocols::admin::ServerMode ToProtoServerMode(Stats::ServerMode mode) noexcept
{
    switch (mode)
    {
    case Stats::ServerMode::IOCP: return ::fastport::protocols::admin::SERVER_MODE_IOCP;
    case Stats::ServerMode::RIO:  return ::fastport::protocols::admin::SERVER_MODE_RIO;
    default:                      return ::fastport::protocols::admin::SERVER_MODE_UNKNOWN;
    }
}
} // anonymous namespace


AdminPacketHandler::AdminPacketHandler(Stats::ServerStatsCollector& collector)
    : m_Collector(collector)
{
}


bool AdminPacketHandler::HandlePacket(Sessions::INetworkSession& sender,
                                      const Core::Packet& packet)
{
    const auto packetId = packet.GetPacketId();

    if (!IsAdminPacketId(packetId))
    {
        return false;
    }

    try
    {
        switch (packetId)
        {
        case kPacketId_SummaryRequest:
            HandleSummaryRequest(sender, packet);
            return true;

        case kPacketId_SessionListReq:
            HandleSessionListRequest(sender, packet);
            return true;

        default:
            LogWarning(std::format("Unknown admin packet id : {:#06x}, sessionId : {}",
                packetId, sender.GetSessionId()));
            return true;  // admin 대역이므로 consumed 로 처리 (일반 패킷 dispatch 방지)
        }
    }
    catch (const std::exception& e)
    {
        LogError(std::format("HandlePacket threw: {}", e.what()));
        return true;
    }
}


void AdminPacketHandler::HandleSummaryRequest(Sessions::INetworkSession& sender,
                                              const Core::Packet& packet)
{
    ::fastport::protocols::admin::AdminStatusSummaryRequest request;
    if (!packet.ParseMessage(request))
    {
        LogError(std::format("Summary parse failed. SessionId : {}", sender.GetSessionId()));
        return;
    }

    LogDebug(std::format("Summary request from session {}", sender.GetSessionId()));

    const auto summary = m_Collector.SnapshotSummary();

    ::fastport::protocols::admin::AdminStatusSummaryResponse response;
    auto* pHeader = response.mutable_header();
    pHeader->set_request_id(request.header().request_id());
    pHeader->set_timestamp_ms(request.header().timestamp_ms());

    response.set_result(::fastport::protocols::commons::RESULT_CODE_OK);
    response.set_server_uptime_ms(static_cast<google::protobuf::uint64>(summary.uptimeMs));
    response.set_active_session_count(summary.activeSessionCount);
    response.set_total_rx_bytes(summary.totalRxBytes);
    response.set_total_tx_bytes(summary.totalTxBytes);
    response.set_idle_disconnect_count(summary.idleDisconnectCount);
    response.set_server_mode(ToProtoServerMode(summary.serverMode));
    response.set_process_memory_bytes(summary.processMemoryBytes);
    response.set_process_cpu_percent(summary.processCpuPercent);
    response.set_server_timestamp_ms(static_cast<google::protobuf::uint64>(summary.serverTimestampMs));

    sender.SendMessage(kPacketId_SummaryResponse, response);
}


void AdminPacketHandler::HandleSessionListRequest(Sessions::INetworkSession& sender,
                                                  const Core::Packet& packet)
{
    ::fastport::protocols::admin::AdminSessionListRequest request;
    if (!packet.ParseMessage(request))
    {
        LogError(std::format("SessionList parse failed. SessionId : {}", sender.GetSessionId()));
        return;
    }

    LogDebug(std::format("SessionList request from session {}, offset={}, limit={}",
        sender.GetSessionId(), request.offset(), request.limit()));

    const auto listData = m_Collector.SnapshotSessions(request.offset(), request.limit());

    ::fastport::protocols::admin::AdminSessionListResponse response;
    auto* pHeader = response.mutable_header();
    pHeader->set_request_id(request.header().request_id());
    pHeader->set_timestamp_ms(request.header().timestamp_ms());

    response.set_result(::fastport::protocols::commons::RESULT_CODE_OK);
    response.set_total(listData.total);
    response.set_offset(listData.offset);

    for (auto const& s : listData.sessions)
    {
        auto* pInfo = response.add_sessions();
        pInfo->set_session_id(s.sessionId);
        pInfo->set_last_recv_ms(s.lastRecvMs);
        pInfo->set_rx_bytes(s.rxBytes);
        pInfo->set_tx_bytes(s.txBytes);
    }

    sender.SendMessage(kPacketId_SessionListRes, response);
}

} // namespace LibNetworks::Admin
