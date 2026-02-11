module;

export module iocp_inbound_session;
import networks.sessions.inbound_session;
import commons.buffers.ibuffer;
import networks.core.packet;

export class IOCPInboundSession : public LibNetworks::Sessions::InboundSession
{
public:
    IOCPInboundSession() = delete;
    IOCPInboundSession(const IOCPInboundSession&) = delete;
    IOCPInboundSession& operator=(const IOCPInboundSession&) = delete;

    explicit IOCPInboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer);
    virtual ~IOCPInboundSession() override; 

    void OnAccepted() override;

    void OnDisconnected() override;

protected:
    void OnPacketReceived(const LibNetworks::Core::Packet& rfPacket) override;

    void OnSent(size_t bytesSent) override;

private:
    void HandleBenchmarkRequest(const LibNetworks::Core::Packet& rfPacket);
    void HandleEchoRequest(const LibNetworks::Core::Packet& rfPacket);
};