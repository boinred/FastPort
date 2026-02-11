module;

#include <WinSock2.h>
#include <MSWSock.h>

export module rio_inbound_session;

import networks.sessions.rio_session;
import commons.buffers.ibuffer;
import networks.core.packet;
import networks.core.packet;
import networks.core.rio_buffer_manager;

export class RIOInboundSession : public LibNetworks::Sessions::RIOSession
{
public:
    RIOInboundSession() = delete;
    RIOInboundSession(const RIOInboundSession&) = delete;
    RIOInboundSession& operator=(const RIOInboundSession&) = delete;

    explicit RIOInboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket, const LibNetworks::Core::RioBufferSlice& recvSlice, const LibNetworks::Core::RioBufferSlice& sendSlice, RIO_CQ completionQueue);

    virtual ~RIOInboundSession() override;

    void OnAccepted() override;

    void OnDisconnected() override;

protected:
    void OnPacketReceived(const LibNetworks::Core::Packet& rfPacket) override;

private:
    void HandleBenchmarkRequest(const LibNetworks::Core::Packet& rfPacket);
    void HandleEchoRequest(const LibNetworks::Core::Packet& rfPacket);
};