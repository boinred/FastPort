module;

#include <utility>

module fastport_outbound_session;

import std; 

FastPortOutboundSession::FastPortOutboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
    : LibNetworks::Sessions::OutboundSession(pSocket, std::move(pReceiveBuffer), std::move(pSendBuffer))
{

}

void FastPortOutboundSession::OnConnected()
{
    __super::OnConnected(); 

    std::string msg = "Hello FastPort Server!";

    SendBuffer(msg.c_str(), msg.size());
}

void FastPortOutboundSession::OnDisconnected()
{
    __super::OnDisconnected();
}

void FastPortOutboundSession::OnReceive(const char* pData, size_t dataLength)
{
    __super::OnReceive(pData, dataLength);
}

void FastPortOutboundSession::OnSent(size_t bytesSent)
{
    __super::OnSent(bytesSent);
}
