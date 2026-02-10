module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <memory>
#include <vector>
#include <atomic>
#include <span>
#include <google/protobuf/message.h>

export module networks.sessions.rio_session;

import networks.sessions.inetwork_session;
import networks.core.rio_extension;
import networks.core.rio_context;
import networks.core.rio_buffer_manager;
import networks.core.socket;
import networks.core.packet;
import networks.core.packet_framer;
import commons.buffers.external_circle_buffer_queue;

namespace LibNetworks::Sessions
{

/**
 * RIO (Registered I/O) 기반 네트워크 세션
 * C1001 에러 방지를 위해 상속 구조 단순화
 */
export class RIOSession : public INetworkSession, public std::enable_shared_from_this<RIOSession>
{
public:
    RIOSession(const std::shared_ptr<Core::Socket>& pSocket,
               const Core::RioBufferSlice& recvSlice,
               const Core::RioBufferSlice& sendSlice,
               RIO_CQ completionQueue);
    
    virtual ~RIOSession() override;

    bool Initialize();

    virtual void SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage) override;
    virtual uint64_t GetSessionId() const override { return m_SessionId; }

    // IRioConsumer 상속 대신 직접 호출될 함수
    void OnRioIOCompleted(bool bSuccess, DWORD bytesTransferred, Core::RioOperationType opType);

    virtual void OnAccepted() override {}
    virtual void OnConnected() override {}
    virtual void OnDisconnected() override {}

protected:
    virtual void OnPacketReceived(const Core::Packet& rfPacket) {}

private:
    void RequestRecv();
    void TryPostSendFromQueue();
    void ReadReceivedBuffers();
    void WriteToBuffers(const std::vector<std::span<std::byte>>& buffers, size_t& offset, const void* pData, size_t len);

private:
    std::shared_ptr<Core::Socket> m_pSocket;
    RIO_RQ m_RQ = RIO_INVALID_RQ;
    
    Core::RioBufferSlice m_RecvSlice;
    Core::RioBufferSlice m_SendSlice;
    
    Core::RioContext m_RecvContext;
    Core::RioContext m_SendContext;
    
    std::unique_ptr<LibCommons::Buffers::ExternalCircleBufferQueue> m_pReceiveBuffer;
    std::unique_ptr<LibCommons::Buffers::ExternalCircleBufferQueue> m_pSendBuffer;

    std::atomic<bool> m_bSendInProgress = false;
    std::atomic<bool> m_bIsDisconnected = false;

    uint64_t m_SessionId = m_NextSessionId.fetch_add(1, std::memory_order_relaxed);
    inline static std::atomic<uint64_t> m_NextSessionId = 1;
};

} // namespace LibNetworks::Sessions
