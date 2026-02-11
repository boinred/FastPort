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
 * RIO (Registered I/O) 기반 네트워크 세션 (Client에서는 사용하지 않고 Server 전용)
 * C1001 에러 방지를 위해 상속 구조 단순화
 */
export class RIOSession : public INetworkSession, public std::enable_shared_from_this<RIOSession>
{
public:
    // 세션 생성자
    RIOSession(const std::shared_ptr<Core::Socket>& pSocket, const Core::RioBufferSlice& recvSlice, const Core::RioBufferSlice& sendSlice, RIO_CQ completionQueue);

    // 세션 소멸자
    virtual ~RIOSession() override;

    // RIO 세션 초기화
    bool Initialize();

    // 패킷 메시지 전송
    virtual void SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage) override;
    // 세션 ID 조회
    virtual uint64_t GetSessionId() const override { return m_SessionId; }

    // IRioConsumer 상속 대신 직접 호출될 함수
    // RIO I/O 완료 처리
    void OnRioIOCompleted(bool bSuccess, DWORD bytesTransferred, Core::RioOperationType opType);

    // 연결 수락 이벤트 처리
    virtual void OnAccepted() override;
    // 연결 완료 이벤트 처리
    virtual void OnConnected() override;
    // 연결 종료 이벤트 처리
    virtual void OnDisconnected() override;

    // 연결 상태 확인
    bool IsConnected() const { return !m_bIsDisconnected.load(); }

protected:
    // 패킷 수신 이벤트 처리
    virtual void OnPacketReceived(const Core::Packet& rfPacket) {}

private:
    // RIO 수신 요청
    void RequestRecv();
    // 전송 큐에서 전송 요청 처리
    void TryPostSendFromQueue();
    // 수신 버퍼 읽기
    void ReadReceivedBuffers();
    // 버퍼에 데이터 기록
    void WriteToBuffers(const std::vector<std::span<std::byte>>& buffers, size_t& offset, const void* pData, size_t len);

private:
    // 연결된 소켓
    std::shared_ptr<Core::Socket> m_pSocket;
    // RIO 요청 큐
    RIO_RQ m_RQ = RIO_INVALID_RQ;

    // 수신 버퍼 슬라이스
    Core::RioBufferSlice m_RecvSlice{};
    // 송신 버퍼 슬라이스
    Core::RioBufferSlice m_SendSlice{};

    // 수신 컨텍스트
    Core::RioContext m_RecvContext{};
    // 송신 컨텍스트
    Core::RioContext m_SendContext{};

    // 전송 진행 상태
    std::atomic<bool> m_bSendInProgress = false;
    // 연결 종료 상태
    std::atomic<bool> m_bIsDisconnected = false;

    // 세션 ID
    uint64_t m_SessionId = m_NextSessionId.fetch_add(1, std::memory_order_relaxed);
    // 다음 세션 ID
    inline static std::atomic<uint64_t> m_NextSessionId = 1;
};

} // namespace LibNetworks::Sessions
