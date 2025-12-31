module;
#include <utility>
#include <spdlog/spdlog.h>
module networks.sessions.inbound_session;

import commons.logger;

namespace LibNetworks::Sessions
{

InboundSession::InboundSession(const std::shared_ptr<Core::Socket>& pSocket,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
    : IOSession(std::move(pSocket), std::move(pReceiveBuffer), std::move(pSendBuffer))
{

}

void InboundSession::OnAccepted()
{
    LibCommons::Logger::GetInstance().LogInfo("InboundSession", "OnAccepted. Session Id : {}", GetSessionId());
}

void InboundSession::OnDisconnected()
{
    LibCommons::Logger::GetInstance().LogInfo("InboundSession", "OnDisconnected. Session Id : {}", GetSessionId());
}

} // namespace LibNetworks::Sessions