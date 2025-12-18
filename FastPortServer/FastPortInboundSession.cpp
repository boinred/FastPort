module;

module fastport_inbound_session;

FastPortInboundSession::FastPortInboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket) 
    : LibNetworks::Sessions::InboundSession(pSocket)
{

}
