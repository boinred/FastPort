module;

#include <utility>
#include <spdlog/spdlog.h>
#include <memory>
#include <Protocols/Commons.pb.h>
#include <Protocols/Tests.pb.h>


module fastport_inbound_session;
import commons.logger;
import commons.container; 
import commons.singleton;

using SessionContainer = LibCommons::Container<uint64_t, std::shared_ptr<LibNetworks::Sessions::InboundSession>>;

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

    LibCommons::Logger::GetInstance().LogInfo("FastPortInboundSession", "OnReceive. Session Id : {}, Data Length : {}", GetSessionId(), rfPacket.GetPacketSize());

    __super::OnPacketReceived(rfPacket);

    // Process the received packet
    const auto packetId = rfPacket.GetPacketId();

    ::fastport::protocols::tests::EchoRequest request;
    if (!rfPacket.ParseMessage(request))
    {
        LibCommons::Logger::GetInstance().LogError("FastPortOutboundSession", "OnReceive, Failed to parse EchoRequest. Session Id : {}, Packet Id : {}", GetSessionId(), packetId);
        return;
    }

    ::fastport::protocols::tests::EchoResponse response;
    auto pHeader = response.mutable_header();
    pHeader->set_request_id(request.header().request_id() + 1);
    pHeader->set_timestamp_ms(static_cast<uint64_t>(time(nullptr)));
    response.set_result(::fastport::protocols::commons::ResultCode::RESULT_CODE_OK);
    response.set_data_str(request.data_str());
    SendMessage(::fastport::protocols::commons::ProtocolId::PROTOCOL_ID_TESTS, response);

    LibCommons::Logger::GetInstance().LogInfo("FastPortOutboundSession", "OnReceive, SendBuffer, Session Id : {}, Packet Id: {}, Request Id : {}", GetSessionId(), packetId, request.header().request_id());
}

void FastPortInboundSession::OnSent(size_t bytesSent)
{
    __super::OnSent(bytesSent);
}
