// AdminPacketHandler.ixx
// -----------------------------------------------------------------------------
// Design Ref: server-status §4.3 — Admin 패킷 처리. Collector 를 DI 로 주입받아
// Summary/SessionList 요청에 응답. 세션은 HandlePacket(session, packet) 만 호출.
// -----------------------------------------------------------------------------
module;

#include <cstdint>

export module networks.admin.admin_packet_handler;

import std;
import networks.sessions.inetwork_session;
import networks.core.packet;
import networks.stats.server_stats_collector;


namespace LibNetworks::Admin
{

// Design Ref: server-status §3.2 — Admin 패킷 ID.
export constexpr std::uint16_t kPacketId_SummaryRequest   = 0x8001;
export constexpr std::uint16_t kPacketId_SummaryResponse  = 0x8002;
export constexpr std::uint16_t kPacketId_SessionListReq   = 0x8003;
export constexpr std::uint16_t kPacketId_SessionListRes   = 0x8004;

// Admin 대역: 0x8000 ~ 0x8FFF.
export inline bool IsAdminPacketId(std::uint16_t id) noexcept
{
    return (id & 0xF000) == 0x8000;
}


export class AdminPacketHandler
{
public:
    explicit AdminPacketHandler(Stats::ServerStatsCollector& collector);

    // 패킷 ID 가 admin 대역이면 true (처리 완료). 아니면 false (호출자가 다른 dispatch).
    bool HandlePacket(Sessions::INetworkSession& sender,
                      const Core::Packet& packet);

private:
    void HandleSummaryRequest(Sessions::INetworkSession& sender, const Core::Packet& packet);
    void HandleSessionListRequest(Sessions::INetworkSession& sender, const Core::Packet& packet);

    Stats::ServerStatsCollector& m_Collector;
};

} // namespace LibNetworks::Admin
