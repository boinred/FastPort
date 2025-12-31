module;

#include <utility>

module fastport_outbound_session;

FastPortOutboundSession::FastPortOutboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
    : LibNetworks::Sessions::OutboundSession(pSocket, std::move(pReceiveBuffer), std::move(pSendBuffer))
{

}
