module;

#include <utility>
#include <spdlog/spdlog.h>
module networks.sessions.outbound_session;
import commons.logger;

namespace LibNetworks::Sessions
{


OutboundSession::OutboundSession(const std::shared_ptr<Core::Socket>& pSocket,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
    : IOSession(std::move(pSocket), std::move(pReceiveBuffer), std::move(pSendBuffer))
{

}


void OutboundSession::OnConnected()
{
    LibCommons::Logger::GetInstance().LogInfo("OutboundSession", "OnConnected. Session Id : {}", GetSessionId());
    //throw std::logic_error("The method or operation is not implemented.");
}

void OutboundSession::OnDisconnected()
{
    LibCommons::Logger::GetInstance().LogInfo("OutboundSession", "OnDisconnected. Session Id : {}", GetSessionId());
    //throw std::logic_error("The method or operation is not implemented.");
}

} // namespace LibNetworks::Sessions