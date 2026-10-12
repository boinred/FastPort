// Design Ref: §3.3 — TestSession (OutboundSession + RTT measurement)
// protobuf 메시지를 생성/파싱하는 멤버 구현. .pb.h 는 이 구현 단위 GMF 에서만 include
// (인터페이스 GMF 에 두면 MSVC 14.51.36231 C1116 ICE — TestSession.ixx 주석 참고).
module;

// SendMessage 는 <Windows.h> 매크로(SendMessageW)로 선언부가 치환되어 있으므로 동일하게 include 필요.
#include <WinSock2.h>

#include "Protocols/Commons.pb.h"
#include "Protocols/Tests.pb.h"
#include "Protocols/Admin.pb.h"

module test_client.test_session;

import networks.core.packet;

namespace
{

AdminSummarySnapshot ToSnapshot(const ::fastport::protocols::admin::AdminStatusSummaryResponse& response)
{
    AdminSummarySnapshot snapshot;
    switch (response.server_mode())
    {
    case ::fastport::protocols::admin::SERVER_MODE_IOCP: snapshot.ServerMode = AdminServerMode::IOCP; break;
    case ::fastport::protocols::admin::SERVER_MODE_RIO:  snapshot.ServerMode = AdminServerMode::RIO;  break;
    default: break;
    }
    snapshot.ServerUptimeMs = response.server_uptime_ms();
    snapshot.ActiveSessionCount = response.active_session_count();
    snapshot.TotalRxBytes = response.total_rx_bytes();
    snapshot.TotalTxBytes = response.total_tx_bytes();
    snapshot.IdleDisconnectCount = response.idle_disconnect_count();
    snapshot.ProcessMemoryBytes = response.process_memory_bytes();
    snapshot.ProcessCpuPercent = response.process_cpu_percent();
    snapshot.ServerTimestampMs = response.server_timestamp_ms();
    return snapshot;
}

AdminSessionListSnapshot ToSnapshot(const ::fastport::protocols::admin::AdminSessionListResponse& response)
{
    AdminSessionListSnapshot snapshot;
    snapshot.Total = response.total();
    snapshot.Offset = response.offset();
    snapshot.Sessions.reserve(static_cast<size_t>(response.sessions_size()));
    for (const auto& session : response.sessions())
    {
        snapshot.Sessions.push_back({ session.session_id(), session.last_recv_ms(), session.rx_bytes(), session.tx_bytes() });
    }
    return snapshot;
}

} // anonymous namespace

void TestSession::SendAdminSummaryRequest()
{
    ::fastport::protocols::admin::AdminStatusSummaryRequest request;
    auto pHeader = request.mutable_header();
    pHeader->set_request_id(m_NextRequestId++);
    pHeader->set_timestamp_ms(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
    SendMessage(0x8001, request);
}

void TestSession::SendAdminSessionListRequest(uint32_t offset, uint32_t limit)
{
    ::fastport::protocols::admin::AdminSessionListRequest request;
    auto pHeader = request.mutable_header();
    pHeader->set_request_id(m_NextRequestId++);
    pHeader->set_timestamp_ms(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
    request.set_offset(offset);
    request.set_limit(limit);
    SendMessage(0x8003, request);
}

void TestSession::SendEcho(const char* message)
{
    m_LastSendTime = std::chrono::steady_clock::now();
    m_SendEchoCount.fetch_add(1, std::memory_order_relaxed);

    ::fastport::protocols::tests::EchoRequest request;
    auto pHeader = request.mutable_header();
    pHeader->set_request_id(m_NextRequestId++);
    pHeader->set_timestamp_ms(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            m_LastSendTime.time_since_epoch()).count()));
    request.set_data_str(message);

    SendMessage(::fastport::protocols::commons::ProtocolId::PROTOCOL_ID_TESTS, request);
}

void TestSession::OnPacketReceived(const LibNetworks::Core::Packet& rfPacket)
{
    const auto packetId = rfPacket.GetPacketId();

    // Design Ref: server-status §5.2 — Admin 응답은 콜백으로 전달.
    if (packetId == 0x8002)
    {
        ::fastport::protocols::admin::AdminStatusSummaryResponse response;
        if (rfPacket.ParseMessage(response) && m_AdminSummaryCb)
        {
            m_AdminSummaryCb(ToSnapshot(response));
        }
        return;
    }
    if (packetId == 0x8004)
    {
        ::fastport::protocols::admin::AdminSessionListResponse response;
        if (rfPacket.ParseMessage(response) && m_AdminSessionListCb)
        {
            m_AdminSessionListCb(ToSnapshot(response));
        }
        return;
    }

    auto recvTime = std::chrono::steady_clock::now();
    double rttMs = std::chrono::duration<double, std::milli>(recvTime - m_LastSendTime).count();

    m_EchoCount.fetch_add(1, std::memory_order_relaxed);

    if (m_pMetrics)
    {
        m_pMetrics->RecordLatency(rttMs);
        m_pMetrics->RecordMessage();
    }

    // 에코 테스트 잔여 횟수가 있으면 계속 전송
    if (m_RemainingEchos.load(std::memory_order_relaxed) > 0)
    {
        m_RemainingEchos.fetch_sub(1, std::memory_order_relaxed);
        const char* payload = m_EchoPayload.empty() ? "echo" : m_EchoPayload.c_str();
        SendEcho(payload);
    }
}
