module;
#include <utility>
#include <spdlog/spdlog.h>
module networks.sessions.inbound_session;

import commons.logger;

namespace LibNetworks::Sessions
{

InboundSession::InboundSession(const std::shared_ptr<Core::Socket>& pSocket) : IOSession(std::move(pSocket))
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