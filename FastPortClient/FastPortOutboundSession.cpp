module;

#include <utility>
#include <spdlog/spdlog.h>

module fastport_outbound_session;

import std; 
import commons.logger;

FastPortOutboundSession::FastPortOutboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
    : LibNetworks::Sessions::OutboundSession(pSocket, std::move(pReceiveBuffer), std::move(pSendBuffer))
{

}

FastPortOutboundSession::~FastPortOutboundSession()
{
    LibCommons::Logger::GetInstance().LogInfo("FastPortOutboundSession", "Destructor called. Session Id : {}", GetSessionId());
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
