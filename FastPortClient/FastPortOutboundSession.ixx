module;

export module fastport_outbound_session;

import networks.sessions.outbound_session;
import commons.buffers.ibuffer;

export class FastPortOutboundSession : public LibNetworks::Sessions::OutboundSession
{
public:
    FastPortOutboundSession() = delete;
    FastPortOutboundSession(const FastPortOutboundSession&) = delete;
    FastPortOutboundSession& operator=(const FastPortOutboundSession&) = delete;

    explicit FastPortOutboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer);

    virtual ~FastPortOutboundSession();

    
   
    void OnConnected() override;


    void OnDisconnected() override;


    void OnReceive(const char* pData, size_t dataLength) override;


    void OnSent(size_t bytesSent) override;

};