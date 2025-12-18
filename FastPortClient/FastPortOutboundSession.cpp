module;

module fastport_outbound_session;

FastPortOutboundSession::FastPortOutboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket)
    : LibNetworks::Sessions::OutboundSession(pSocket)
{

}
