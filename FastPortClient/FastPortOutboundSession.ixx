module;

export module fastport_outbound_session;

import networks.sessions.outbound_session;

export class FastPortOutboundSession : public LibNetworks::Sessions::OutboundSession
{
public:
    FastPortOutboundSession() = delete;
    FastPortOutboundSession(const FastPortOutboundSession&) = delete;
    FastPortOutboundSession& operator=(const FastPortOutboundSession&) = delete;
    explicit FastPortOutboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket);
    virtual ~FastPortOutboundSession() = default;
};