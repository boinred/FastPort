module;

#include <utility>

module fastport_inbound_session;

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
}

void FastPortInboundSession::OnSent(size_t bytesSent)
{
    __super::OnSent(bytesSent);
}
 