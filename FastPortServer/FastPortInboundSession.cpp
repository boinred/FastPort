module;

#include <utility>
#include <spdlog/spdlog.h>
#include <memory>
#include <chrono>
#include <Protocols/Commons.pb.h>
#include <Protocols/Tests.pb.h>
#include <Protocols/Benchmark.pb.h>


module fastport_inbound_session;
import commons.logger;
import commons.container; 
import commons.singleton;

using SessionContainer = LibCommons::Container<uint64_t, std::shared_ptr<LibNetworks::Sessions::InboundSession>>;

namespace
{
    // 패킷 ID 정의
    constexpr uint16_t PACKET_ID_ECHO_REQUEST = static_cast<uint16_t>(::fastport::protocols::commons::ProtocolId::PROTOCOL_ID_TESTS);
    constexpr uint16_t PACKET_ID_BENCHMARK_REQUEST = 0x1001;
    constexpr uint16_t PACKET_ID_BENCHMARK_RESPONSE = 0x1002;

    uint64_t GetCurrentTimeNs()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()).count());
    }
}

FastPortInboundSession::FastPortInboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
    : LibNetworks::Sessions::InboundSession(pSocket, std::move(pReceiveBuffer), std::move(pSendBuffer))
{

}

FastPortInboundSession::~FastPortInboundSession()
{
    LibCommons::Logger::GetInstance().LogInfo("FastPortInboundSession", "Destructor called. Session Id : {}", GetSessionId());
}

void FastPortInboundSession::OnAccepted()
{
    __super::OnAccepted();


    auto& sessions = LibCommons::SingleTon<SessionContainer>::GetInstance();
    sessions.Add(GetSessionId(), std::dynamic_pointer_cast<FastPortInboundSession>(shared_from_this()));
}

void FastPortInboundSession::OnDisconnected()
{
    __super::OnDisconnected();

    auto& sessions = LibCommons::SingleTon<SessionContainer>::GetInstance();
    sessions.Remove(GetSessionId());
}

void FastPortInboundSession::OnPacketReceived(const LibNetworks::Core::Packet& rfPacket)
{
    __super::OnPacketReceived(rfPacket);

    const auto packetId = rfPacket.GetPacketId();

    switch (packetId)
    {
    case PACKET_ID_BENCHMARK_REQUEST:
        HandleBenchmarkRequest(rfPacket);
        break;

    case PACKET_ID_ECHO_REQUEST:
        HandleEchoRequest(rfPacket);
        break;
    default:
        LibCommons::Logger::GetInstance().LogWarning("FastPortInboundSession", "OnPacketReceived, Unknown packet id : {}. Session Id : {}", packetId, GetSessionId());
        break;
    }
}

void FastPortInboundSession::HandleBenchmarkRequest(const LibNetworks::Core::Packet& rfPacket)
{
    uint64_t recvTimestamp = GetCurrentTimeNs();

    ::fastport::protocols::benchmark::BenchmarkRequest request;
    if (!rfPacket.ParseMessage(request))
    {
        LibCommons::Logger::GetInstance().LogError("FastPortInboundSession", 
            "HandleBenchmarkRequest, Failed to parse. Session Id : {}", GetSessionId());
        return;
    }

    ::fastport::protocols::benchmark::BenchmarkResponse response;
    
    auto pHeader = response.mutable_header();
    pHeader->set_request_id(request.header().request_id());
    pHeader->set_timestamp_ms(request.header().timestamp_ms());
    
    response.set_result(::fastport::protocols::commons::ResultCode::RESULT_CODE_OK);
    response.set_client_timestamp_ns(request.client_timestamp_ns());
    response.set_server_recv_timestamp_ns(recvTimestamp);
    response.set_server_send_timestamp_ns(GetCurrentTimeNs());
    response.set_sequence(request.sequence());
    response.set_payload(request.payload());

    SendMessage(PACKET_ID_BENCHMARK_RESPONSE, response);
}

void FastPortInboundSession::HandleEchoRequest(const LibNetworks::Core::Packet& rfPacket)
{
    const auto packetId = rfPacket.GetPacketId();

    LibCommons::Logger::GetInstance().LogInfo("FastPortInboundSession", 
        "HandleEchoRequest. Session Id : {}, Data Length : {}", GetSessionId(), rfPacket.GetPacketSize());

    ::fastport::protocols::tests::EchoRequest request;
    if (!rfPacket.ParseMessage(request))
    {
        LibCommons::Logger::GetInstance().LogError("FastPortInboundSession", 
            "HandleEchoRequest, Failed to parse. Session Id : {}, Packet Id : {}", GetSessionId(), packetId);
        return;
    }

    ::fastport::protocols::tests::EchoResponse response;
    auto pHeader = response.mutable_header();
    pHeader->set_request_id(request.header().request_id() + 1);
    pHeader->set_timestamp_ms(static_cast<uint64_t>(time(nullptr)));
    response.set_result(::fastport::protocols::commons::ResultCode::RESULT_CODE_OK);
    response.set_data_str(request.data_str());
    
    SendMessage(packetId, response);

    LibCommons::Logger::GetInstance().LogInfo("FastPortInboundSession", 
        "HandleEchoRequest, Response sent. Session Id : {}, Packet Id: {}, Request Id : {}", 
        GetSessionId(), packetId, request.header().request_id());
}

void FastPortInboundSession::OnSent(size_t bytesSent)
{
    __super::OnSent(bytesSent);
}
