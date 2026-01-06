module;

#include <utility>
#include <spdlog/spdlog.h>
#include <memory>

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

void FastPortInboundSession::OnReceive(const char* pData, size_t dataLength)
{
    __super::OnReceive(pData, dataLength);

    LibCommons::Logger::GetInstance().LogInfo("FastPortInboundSession", "OnReceive. Session Id : {}, Data Length : {}", GetSessionId(), dataLength);

    SendBuffer(pData, dataLength);
}

void FastPortInboundSession::OnSent(size_t bytesSent)
{
    __super::OnSent(bytesSent);
}
 