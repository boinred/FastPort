module;

#include <WinSock2.h>
#include <memory>
#include <vector>
#include <atomic>
#include <span>
#include <cstdint>
#include <google/protobuf/message.h>

export module networks.sessions.io_session;

import networks.sessions.inetwork_session;
import networks.sessions.iidle_aware;
import networks.sessions.isession_stats;
import networks.core.io_consumer;
import networks.core.socket;
import networks.core.packet;
import networks.core.packet_framer;
import commons.buffers.ibuffer;

namespace LibNetworks::Sessions
{


// Design Ref: session-idle-timeout §3.2, §4.2 — IIdleAware 상속으로 SessionIdleChecker 가
// 구체 세션 타입을 몰라도 idle 감지 가능하게 만듦.
export class IOSession : public Core::IIOConsumer,
                          public INetworkSession,
                          public IIdleAware,
                          public ISessionStats,
                          public std::enable_shared_from_this<IOSession>
{
public:
    IOSession() = delete;
    IOSession(const IOSession&) = delete;
    IOSession& operator=(const IOSession&) = delete;

    // 소켓/송수신 버퍼 의존성 주입 기반 세션 초기화.
    explicit IOSession(const std::shared_ptr<Core::Socket>& pSocket,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer);

    // # 소멸 시점 불변식 검증
    virtual ~IOSession() override;

    // 세션 고유 식별자 조회.
    virtual uint64_t GetSessionId() const override { return m_SessionId; }

    // Accept 완료 이벤트 처리 훅.
    virtual void OnAccepted() override {}
    // Connect 완료 이벤트 처리 훅.
    virtual void OnConnected() override {}

    virtual void SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage) override;

    // (Outbound 전용) ConnectEx용 OVERLAPPED 포인터 반환
    virtual OVERLAPPED* GetConnectOverlappedPtr() override { return nullptr; }

    // Design Ref: session-idle-timeout §3.2 — IIdleAware 구현.
    // steady_clock 기준 epoch-ms. 0 은 수신 이력 없음 (연결 직후).
    // Thread-safety: relaxed atomic read — 정확도보다 lock-free 성능 우선.
    std::int64_t GetLastRecvTimeMs() const noexcept override
    {
        return m_LastRecvTimeMs.load(std::memory_order_relaxed);
    }

    // Design Ref: server-status §3.3 — ISessionStats 구현.
    std::uint64_t GetTotalRxBytes() const noexcept override
    {
        return m_TotalRxBytes.load(std::memory_order_relaxed);
    }
    std::uint64_t GetTotalTxBytes() const noexcept override
    {
        return m_TotalTxBytes.load(std::memory_order_relaxed);
    }

    // Design Ref: session-idle-timeout §4.2 — 사유 파라미터 오버로드.
    // IIdleAware::RequestDisconnect 구현. 내부적으로 기존 RequestDisconnect() 경로와 통합.
    void RequestDisconnect(DisconnectReason reason) override;

    // 기존 호출자(8곳) 호환 — 내부에서 Normal 사유로 delegation.
    void RequestDisconnect();

protected:
    // # 종료 콜백 단일 발화
    void TryFireOnDisconnected();

    // 세션 활성화 시 최초 1회 receive loop 시작.
    // # 최초 수신 루프 개시
    void StartReceiveLoop();

    // 연결 종료 이벤트 처리
    virtual void OnDisconnected() override {}

    // 지속 수신을 위한 Recv 재등록.
    // # 수신 재등록 진입점
    void RequestReceived();

    // 세션 소켓 조회.
    const std::shared_ptr<Core::Socket> GetSocket() const { return m_pSocket; }

    // IOCP 완료 통지 처리 진입점.
    virtual void OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped) override;

    // 수신 데이터 처리
    virtual void OnPacketReceived(const Core::Packet& rfPacket) {}

    // 송신 완료 처리
    virtual void OnSent(size_t bytesSent) {}

protected:
    // IOCP용 OVERLAPPED 확장 컨텍스트.
    // protected: 서브클래스(테스트용 TestableIOSession 등)가 OnIOCompleted 를 시뮬레이션할 때
    //            이 Overlapped 주소가 필요 — OnIOCompleted 는 pOverlapped 주소로 Recv/Send 구분.
    struct OverlappedEx
    {
        OVERLAPPED Overlapped{};
        // WSARecv/WSASend용 연속 버퍼 저장소.
        std::vector<char> Buffers{};
        // Scatter-Gather I/O를 위한 WSABUF 배열
        std::vector<WSABUF> WSABufs{};
        // 이번 요청 바이트 수.
        size_t RequestedBytes = 0;

        // Zero-byte Recv 여부 (Recv Overlapped 전용)
        bool IsZeroByte = false;

        // OVERLAPPED 재사용을 위한 초기화.
        void ResetOverlapped()
        {
            std::memset(&Overlapped, 0, sizeof(Overlapped));
        }
    };

    // 수신/송신 Overlapped — protected 로 서브클래스가 OnIOCompleted 호출 시 주소 접근 가능.
    OverlappedEx m_RecvOverlapped{};
    OverlappedEx m_SendOverlapped{};

    // # 완료 통지 수명 보호
    struct IoCompletionGuard
    {
        IOSession& Self;
        explicit IoCompletionGuard(IOSession& s) noexcept : Self(s) {}
        ~IoCompletionGuard() noexcept;
        IoCompletionGuard(const IoCompletionGuard&) = delete;
        IoCompletionGuard& operator=(const IoCompletionGuard&) = delete;
    };

private:
    // 송신 큐 적재 및 비동기 송신 트리거.
    void SendBuffer(std::span<const std::byte> data);

    void ReadReceivedBuffers();

    // Recv용 WSABUF 배열 준비.
    bool PrepareRecvBuffers(bool bZeroByte);

    // Recv 요청 공통 구현
    bool RequestRecv(bool bZeroByte);

    // Send용 WSABUF 배열 준비.
    void PrepareSendBuffers(const std::vector<std::span<const std::byte>>& buffers, size_t bytesToSend);

    // 송신 큐 기반 비동기 송신(WSASend) 등록.
    bool TryPostSendFromQueue();

    // Recv 완료 처리 분기.
    void HandleRecvCompletion(bool bSuccess, DWORD bytesTransferred);
	// Zero-byte Recv 완료 처리: 실제 데이터 수신이 아닌, recv loop 지속을 위한 완료 통지 경로.
    void HandleZeroByteRecvCompletion(DWORD bytesTransferred);
	// Real Recv 완료 처리: 실제 데이터 수신 경로. bytesTransferred > 0 일 때만 호출.
    void HandleRealRecvCompletion(DWORD bytesTransferred);

    // Send 완료 처리 분기.
    void HandleSendCompletion(bool bSuccess, DWORD bytesTransferred);

    // # posting 실패 카운터 복구
    void UndoOutstandingOnFailure(const char* site) noexcept;

private:
    // 활성화 훅에서 최초 receive loop 시작 중복 방지.
    std::atomic_bool m_ReceiveLoopStarted = false;

    // 수신 outstanding 중복 방지 플래그.
    std::atomic_bool m_RecvInProgress = false;

    // 송신 outstanding 중복 방지 플래그.
    std::atomic_bool m_SendInProgress = false;

    std::atomic_bool m_DisconnectRequested = false;

    // # outstanding I/O 카운터
    std::atomic<int> m_OutstandingIoCount { 0 };

    // # 종료 콜백 중복 차단
    std::atomic_bool m_bOnDisconnectedFired = false;

    // Design Ref: session-idle-timeout §3, §4.2 — 마지막 수신 시각 (steady_clock epoch-ms).
    // 0 은 아직 수신 이력 없음. OnIOCompleted 의 Real Recv 성공 경로(bytes > 0) 에서 갱신.
    // Thread-safety: relaxed atomic — tick 콜백과 수신 스레드에서 concurrent 접근.
    std::atomic<std::int64_t> m_LastRecvTimeMs = 0;

    // Design Ref: server-status §3.3, §4.2 — 누적 바이트 카운터.
    // OnIOCompleted 의 수신/송신 성공 경로에서 relaxed atomic fetch_add.
    std::atomic<std::uint64_t> m_TotalRxBytes { 0 };
    std::atomic<std::uint64_t> m_TotalTxBytes { 0 };

    // 세션 소켓 핸들
    std::shared_ptr<Core::Socket> m_pSocket = {};

    // 세션 식별자.
    uint64_t m_SessionId = m_NextSessionId.fetch_add(1, std::memory_order_relaxed);

    // 세션 식별자 시퀀스.
    inline static std::atomic<uint64_t> m_NextSessionId = 1;

};


} // namespace LibNetworks::Sessions
