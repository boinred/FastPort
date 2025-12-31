module;

#include <memory>
#include <array>
#include <vector>
#include <atomic>
#include <WinSock2.h>

export module networks.sessions.io_session;

import networks.core.io_consumer;
import networks.core.socket;
import commons.buffers.ibuffer;

namespace LibNetworks::Sessions
{


export class IOSession : public Core::IIOConsumer, public std::enable_shared_from_this<IOSession>
{
public:
    IOSession() = delete;
    IOSession(const IOSession&) = delete;
    IOSession& operator=(const IOSession&) = delete;

    // 소켓/송수신 버퍼 의존성 주입 기반 세션 초기화.
    explicit IOSession(const std::shared_ptr<Core::Socket>& pSocket,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer);

    // 기본 소멸 처리.
    virtual ~IOSession() = default;

    // 세션 고유 식별자 조회.
    const uint64_t GetSessionId() const { return m_SessionId; }

    // Accept 완료 이벤트 처리 훅.
    virtual void OnAccepted() {}
    // Connect 완료 이벤트 처리 훅.
    virtual void OnConnected() {}

    // 연결 종료 이벤트 처리 훅.
    virtual void OnDisconnected() {}

    // 수신 데이터 처리 훅.
    virtual void OnReceive(const char* pData, size_t dataLength) {}
    // 송신 완료 처리 훅.
    virtual void OnSent(size_t bytesSent) {}

    // 송신 큐 적재 및 비동기 송신 트리거.
    void SendBuffer(const char* pData, size_t dataLength);

protected:
    // 지속 수신을 위한 Recv 재등록.
    void RequestReceived();

protected:
    // 세션 소켓 조회.
    const std::shared_ptr<Core::Socket> GetSocket() const { return m_pSocket; }

    // IOCP 완료 통지 처리 진입점.
    void OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped) override;

private:
    // IO 종류 구분(Recv/Send).
    enum class IOType : uint8_t
    {
        Recv,
        Send,
    };

    // IOCP용 OVERLAPPED 확장 컨텍스트.
    struct OverlappedEx
    {
        OVERLAPPED Overlapped{};
        // 송신 완료까지 유지되는 송신 페이로드 저장소.
        std::vector<char> Buffers{};
        // 이번 송신 요청 바이트 수.
        size_t RequestedBytes = 0;

        // OVERLAPPED 재사용을 위한 초기화.
        void ResetOverlapped()
        {
            std::memset(&Overlapped, 0, sizeof(Overlapped));
        }
    };

private:
    // 비동기 수신(WSARecv) 등록.
    bool PostRecv();

    // 송신 큐 기반 비동기 송신(WSASend) 등록.
    bool TryPostSendFromQueue();

private:
    // 수신 데이터 누적 버퍼(큐).
    std::unique_ptr<LibCommons::Buffers::IBuffer> m_pReceiveBuffer{};
    // 송신 데이터 누적 버퍼(큐).
    std::unique_ptr<LibCommons::Buffers::IBuffer> m_pSendBuffer{};

    // WSARecv 임시 수신 버퍼.
    std::array<char, 64 * 1024> m_RecvTempBuffer{};

    // 수신용 OVERLAPPED 컨텍스트
    OverlappedEx m_RecvOverlapped{};

    // 송신용 OVERLAPPED 컨텍스트
    OverlappedEx m_SendOverlapped{};

    // 수신 outstanding 중복 방지 플래그.
    std::atomic_bool m_RecvInProgress = false;

    // 송신 outstanding 중복 방지 플래그.
    std::atomic_bool m_SendInProgress = false;

    // 세션 소켓 핸들
    std::shared_ptr<Core::Socket> m_pSocket = {};

    // 세션 식별자.
    uint64_t m_SessionId = m_NextSessionId.fetch_add(1, std::memory_order_relaxed);

    // 세션 식별자 시퀀스.
    inline static std::atomic<uint64_t> m_NextSessionId = 1;

};


} // namespace LibNetworks::Sessions