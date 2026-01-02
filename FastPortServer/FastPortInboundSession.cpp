module;

#include <utility>
#include <spdlog/spdlog.h>

module fastport_inbound_session;
import commons.logger;

FastPortInboundSession::FastPortInboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
    : LibNetworks::Sessions::InboundSession(pSocket, std::move(pReceiveBuffer), std::move(pSendBuffer))
{

}

void FastPortInboundSession::OnAccepted()
{
    __super::OnAccepted();
}

void FastPortInboundSession::OnDisconnected()
{
    __super::OnDisconnected();
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
 