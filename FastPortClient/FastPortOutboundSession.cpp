module;

#include <utility>
#include <spdlog/spdlog.h>

module fastport_outbound_session;

import std; 
import commons.logger;
import commons.container; 
import commons.singleton;

using SessionContainer = LibCommons::Container<uint64_t, std::shared_ptr<LibNetworks::Sessions::OutboundSession>>;

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

    auto& sessions = LibCommons::SingleTon<SessionContainer>::GetInstance();
    sessions.Add(GetSessionId(), std::dynamic_pointer_cast<FastPortOutboundSession>(shared_from_this()));

    std::string msg = "Hello FastPort Server!";

    SendBuffer(msg.c_str(), msg.size());

    LibCommons::Logger::GetInstance().LogInfo("FastPortOutboundSession", "OnConnected, SendBuffer, Session Id : {}, Sent Message : {}, Size : {}", GetSessionId(), msg, msg.size());
}

void FastPortOutboundSession::OnDisconnected()
{
    __super::OnDisconnected();

    auto& sessions = LibCommons::SingleTon<SessionContainer>::GetInstance();
    sessions.Remove(GetSessionId());
}

void FastPortOutboundSession::OnReceive(const char* pData, size_t dataLength)
{
    __super::OnReceive(pData, dataLength);

    SendBuffer(pData, dataLength);

    LibCommons::Logger::GetInstance().LogInfo("FastPortOutboundSession", "OnReceive, SendBuffer, Session Id : {}, Sent Message : {}, Size : {}", GetSessionId(), pData, dataLength);
}

void FastPortOutboundSession::OnSent(size_t bytesSent)
{
    __super::OnSent(bytesSent);
}
