module;

export module fastport_inbound_session;
import networks.sessions.inbound_session;

export class FastPortInboundSession : public LibNetworks::Sessions::InboundSession
{
public:
    FastPortInboundSession() = delete;
    FastPortInboundSession(const FastPortInboundSession&) = delete;
    FastPortInboundSession& operator=(const FastPortInboundSession&) = delete;

    explicit FastPortInboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket);
    virtual ~FastPortInboundSession() = default;
};