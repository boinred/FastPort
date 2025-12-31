module;

export module fastport_inbound_session;
import networks.sessions.inbound_session;
import commons.buffers.ibuffer;

export class FastPortInboundSession : public LibNetworks::Sessions::InboundSession
{
public:
    FastPortInboundSession() = delete;
    FastPortInboundSession(const FastPortInboundSession&) = delete;
    FastPortInboundSession& operator=(const FastPortInboundSession&) = delete;

    explicit FastPortInboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer);
    virtual ~FastPortInboundSession() = default;

    void OnAccepted() override;


    void OnDisconnected() override;


    void OnReceive(const char* pData, size_t dataLength) override;


    void OnSent(size_t bytesSent) override;

};