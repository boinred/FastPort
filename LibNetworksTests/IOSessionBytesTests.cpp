// IOSessionBytesTests.cpp
// -----------------------------------------------------------------------------
// Design Ref: server-status §8.3 — IOSession 누적 바이트 카운터 단위 테스트 (IB-01 ~ IB-05).
// TestableIOSession 이 IOSession 을 서브클래스로 받아 OnIOCompleted 를 public 으로 노출 +
// protected 로 승격된 m_RecvOverlapped/m_SendOverlapped 의 주소를 전달해 시뮬레이션.
// 실제 소켓은 INVALID_SOCKET 상태 — OnIOCompleted 경로의 부가 동작(RequestDisconnect 등)
// 은 무해하게 실패하므로 카운터 검증만 집중.
// -----------------------------------------------------------------------------
#include "CppUnitTest.h"

#include <WinSock2.h>
#include <memory>
#include <cstdint>
#include <atomic>

import networks.sessions.io_session;
import networks.sessions.outbound_session;
import networks.core.socket;
import networks.core.packet;
import commons.buffers.circle_buffer_queue;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LibNetworksTests
{

// 테스트 전용 서브클래스 — OnIOCompleted 를 public 으로 alias + 시뮬레이션 헬퍼 제공.
// protected m_RecvOverlapped/m_SendOverlapped 에 자기 자신이 접근.
class TestableIOSession : public LibNetworks::Sessions::IOSession
{
public:
    using LibNetworks::Sessions::IOSession::IOSession;

    // 테스트용 outstanding 을 1 증가시키고 Real Recv 완료를 시뮬레이트.
    // Real Recv 성공 완료를 시뮬레이트. CommitWrite(bytes) → m_TotalRxBytes += bytes.
    void SimulateRealRecvSuccess(DWORD bytes)
    {
        DebugSetOutstandingIoCountForTest(DebugGetOutstandingIoCountForTest() + 1);
        m_RecvOverlapped.IsZeroByte = false;
        this->OnIOCompleted(true, bytes, &m_RecvOverlapped.Overlapped);
    }

    // 테스트용 outstanding 을 1 증가시키고 Zero-byte Recv 완료를 시뮬레이트.
    // Zero-byte Recv 완료를 시뮬레이트 (bytesTransferred=0). 카운터는 변하지 않아야 함.
    void SimulateZeroByteRecvComplete()
    {
        DebugSetOutstandingIoCountForTest(DebugGetOutstandingIoCountForTest() + 1);
        m_RecvOverlapped.IsZeroByte = true;
        this->OnIOCompleted(true, 0, &m_RecvOverlapped.Overlapped);
    }

    // 테스트용 outstanding 을 1 증가시키고 Send 완료를 시뮬레이트.
    // Send 완료를 시뮬레이트. m_TotalTxBytes += bytes.
    void SimulateSendSuccess(DWORD bytes)
    {
        DebugSetOutstandingIoCountForTest(DebugGetOutstandingIoCountForTest() + 1);
        this->OnIOCompleted(true, bytes, &m_SendOverlapped.Overlapped);
    }

    // 기존 outstanding 을 drain 하는 Send completion 을 시뮬레이트.
    void CompleteSendFromExistingOutstanding(DWORD bytes)
    {
        this->OnIOCompleted(true, bytes, &m_SendOverlapped.Overlapped);
    }

    // 기존 outstanding 을 drain 하는 Real Recv completion 을 시뮬레이트.
    // counter 를 증가시키지 않으므로, 호출자가 사전에 SetOutstandingIoCountForTest
    // 로 counter 를 올려둬야 한다.
    void CompleteRealRecvFromExistingOutstanding(DWORD bytes)
    {
        m_RecvOverlapped.IsZeroByte = false;
        this->OnIOCompleted(true, bytes, &m_RecvOverlapped.Overlapped);
    }

    // 테스트 전용 outstanding 강제 설정.
    void SetOutstandingIoCountForTest(int outstanding) noexcept
    {
        DebugSetOutstandingIoCountForTest(outstanding);
    }

    // 테스트 전용 outstanding 조회.
    int GetOutstandingIoCountForTest() const noexcept
    {
        return DebugGetOutstandingIoCountForTest();
    }

    // 테스트 전용 disconnect callback 횟수 조회.
    int GetDisconnectedCountForTest() const noexcept
    {
        return m_DisconnectedCount.load();
    }

    // 테스트 전용 OnPacketReceived 호출 횟수 조회.
    int GetPacketReceivedCountForTest() const noexcept
    {
        return m_PacketReceivedCount.load();
    }

    // 테스트 전용 OnSent 호출 횟수 조회.
    int GetOnSentCountForTest() const noexcept
    {
        return m_OnSentCount.load();
    }

protected:
    // 테스트 전용 disconnect callback 집계.
    void OnDisconnected() override
    {
        m_DisconnectedCount.fetch_add(1);
    }

    // 테스트 전용 packet 배달 집계 — Rule D1 drop 경로 검증에 사용.
    void OnPacketReceived(const LibNetworks::Core::Packet& /*rfPacket*/) override
    {
        m_PacketReceivedCount.fetch_add(1);
    }

    // 테스트 전용 OnSent 집계 — Rule D1 skip 경로 검증에 사용.
    void OnSent(size_t /*bytesSent*/) override
    {
        m_OnSentCount.fetch_add(1);
    }

private:
    std::atomic<int> m_DisconnectedCount { 0 };
    std::atomic<int> m_PacketReceivedCount { 0 };
    std::atomic<int> m_OnSentCount { 0 };
};

class TestableOutboundSession : public LibNetworks::Sessions::OutboundSession
{
public:
    using LibNetworks::Sessions::OutboundSession::OutboundSession;

    void MarkConnectPostedForTest() noexcept
    {
        MarkConnectIoPosted();
    }

    void UndoConnectPostedForTest() noexcept
    {
        UndoConnectIoOnPostFailure();
    }

    void CompleteConnectFromExistingOutstanding(bool success)
    {
        this->OnIOCompleted(success, 0, &GetConnectOverlapped());
    }

    int GetOutstandingIoCountForTest() const noexcept
    {
        return DebugGetOutstandingIoCountForTest();
    }

    int GetDisconnectedCountForTest() const noexcept
    {
        return m_DisconnectedCount.load();
    }

    int GetConnectedCountForTest() const noexcept
    {
        return m_ConnectedCount.load();
    }

protected:
    void OnConnected() override
    {
        m_ConnectedCount.fetch_add(1);
    }

    void OnDisconnected() override
    {
        m_DisconnectedCount.fetch_add(1);
    }

private:
    std::atomic<int> m_DisconnectedCount { 0 };
    std::atomic<int> m_ConnectedCount { 0 };
};


namespace
{
// 세션 factory — 8KB 버퍼 + 기본 (unconnected) Socket 으로 세션 생성.
// 실제 네트워크 I/O 는 하지 않으므로 소켓은 INVALID_SOCKET 상태여도 무방.
std::shared_ptr<TestableIOSession> MakeSession(std::size_t bufCap = 8 * 1024)
{
    auto pSocket = std::make_shared<LibNetworks::Core::Socket>();
    auto pRecv   = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(bufCap);
    auto pSend   = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(bufCap);
    return std::make_shared<TestableIOSession>(pSocket, std::move(pRecv), std::move(pSend));
}

std::shared_ptr<TestableOutboundSession> MakeOutboundSession(std::size_t bufCap = 8 * 1024)
{
    auto pSocket = std::make_shared<LibNetworks::Core::Socket>();
    auto pRecv   = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(bufCap);
    auto pSend   = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(bufCap);
    return std::make_shared<TestableOutboundSession>(pSocket, std::move(pRecv), std::move(pSend));
}
} // anonymous namespace


TEST_CLASS(IOSessionBytesTests)
{
public:

    // IB-01: 생성 직후 Rx / Tx 카운터 = 0.
    TEST_METHOD(IOSession_GetTotalRx_InitialZero)
    {
        auto pSession = MakeSession();

        Assert::AreEqual<std::uint64_t>(0ULL, pSession->GetTotalRxBytes());
        Assert::AreEqual<std::uint64_t>(0ULL, pSession->GetTotalTxBytes());
    }

    // IB-02: Real Recv 1024 bytes → Rx 카운터 1024.
    TEST_METHOD(IOSession_RxBytes_UpdatedOnReceive)
    {
        auto pSession = MakeSession();

        pSession->SimulateRealRecvSuccess(1024);

        Assert::AreEqual<std::uint64_t>(1024ULL, pSession->GetTotalRxBytes(),
            L"Real Recv 완료 후 Rx 가 bytesTransferred 만큼 증가해야 함");
        Assert::AreEqual<std::uint64_t>(0ULL, pSession->GetTotalTxBytes(),
            L"Recv 만으로 Tx 는 변하면 안 됨");
    }

    // IB-03: Send 512 bytes → Tx 카운터 512.
    TEST_METHOD(IOSession_TxBytes_UpdatedOnSend)
    {
        auto pSession = MakeSession();

        pSession->SimulateSendSuccess(512);

        Assert::AreEqual<std::uint64_t>(512ULL, pSession->GetTotalTxBytes(),
            L"Send 완료 후 Tx 가 bytesTransferred 만큼 증가해야 함");
        Assert::AreEqual<std::uint64_t>(0ULL, pSession->GetTotalRxBytes(),
            L"Send 만으로 Rx 는 변하면 안 됨");
    }

    // IB-04: Zero-byte Recv 완료(bytesTransferred=0) → Rx 는 변하지 않음.
    // (Zero-byte Recv 는 데이터가 없음을 통지할 뿐, 실제 수신 이벤트가 아님.)
    TEST_METHOD(IOSession_ZeroByte_NoUpdate)
    {
        auto pSession = MakeSession();

        pSession->SimulateZeroByteRecvComplete();

        Assert::AreEqual<std::uint64_t>(0ULL, pSession->GetTotalRxBytes(),
            L"Zero-byte Recv 완료는 Rx 를 갱신하지 않아야 함");
    }

    // IB-05: 3번 수신 (100 + 200 + 300) → Rx 누적 600.
    TEST_METHOD(IOSession_Cumulative_AcrossMultipleOps)
    {
        auto pSession = MakeSession();

        pSession->SimulateRealRecvSuccess(100);
        pSession->SimulateRealRecvSuccess(200);
        pSession->SimulateRealRecvSuccess(300);

        Assert::AreEqual<std::uint64_t>(600ULL, pSession->GetTotalRxBytes(),
            L"여러 번의 Recv 완료가 누적 합산되어야 함");
    }
};

TEST_CLASS(IOSessionLifetimeTests)
{
public:

    // LT-Idem-01: zero-pending 상태에서 RequestDisconnect 반복 호출 시 OnDisconnected 는 1회만 fire.
    TEST_METHOD(RequestDisconnect_Idempotent_ZeroPending_FiresOnce)
    {
        auto pSession = MakeSession();

        pSession->RequestDisconnect(LibNetworks::Sessions::DisconnectReason::Server);
        pSession->RequestDisconnect(LibNetworks::Sessions::DisconnectReason::Normal);
        pSession->RequestDisconnect(LibNetworks::Sessions::DisconnectReason::IdleTimeout);

        Assert::AreEqual(1, pSession->GetDisconnectedCountForTest(),
            L"zero-pending disconnect 는 중복 호출돼도 OnDisconnected 가 1회만 fire 되어야 함");
        Assert::AreEqual(0, pSession->GetOutstandingIoCountForTest(),
            L"zero-pending disconnect 이후 outstanding 은 0 이어야 함");
    }

    // LT-Zero-01: pending I/O 가 0 이면 RequestDisconnect 가 즉시 fast path 로 종료 callback 을 fire.
    TEST_METHOD(RequestDisconnect_ZeroPending_FastPath_FiresImmediately)
    {
        auto pSession = MakeSession();

        Assert::AreEqual(0, pSession->GetDisconnectedCountForTest(),
            L"사전 조건: disconnect callback 은 아직 호출되면 안 됨");

        pSession->RequestDisconnect(LibNetworks::Sessions::DisconnectReason::Server);

        Assert::AreEqual(1, pSession->GetDisconnectedCountForTest(),
            L"pending 이 0 이면 RequestDisconnect 가 즉시 OnDisconnected 를 fire 해야 함");
        Assert::AreEqual(0, pSession->GetOutstandingIoCountForTest(),
            L"fast path 이후 outstanding 은 0 이어야 함");
    }

    // LT-Drain-01: pending I/O 가 남아 있으면 마지막 completion 전까지는 OnDisconnected 가 fire 되면 안 됨.
    TEST_METHOD(RequestDisconnect_LastCompletion_FiresAfterDrain)
    {
        auto pSession = MakeSession();
        pSession->SetOutstandingIoCountForTest(2);

        pSession->RequestDisconnect(LibNetworks::Sessions::DisconnectReason::Server);

        Assert::AreEqual(0, pSession->GetDisconnectedCountForTest(),
            L"pending I/O 가 남아 있으면 RequestDisconnect 직후에는 OnDisconnected 가 fire 되면 안 됨");
        Assert::AreEqual(2, pSession->GetOutstandingIoCountForTest(),
            L"disconnect 요청만으로 outstanding 이 즉시 줄어들면 안 됨");

        pSession->CompleteSendFromExistingOutstanding(16);

        Assert::AreEqual(0, pSession->GetDisconnectedCountForTest(),
            L"마지막 completion 이전에는 OnDisconnected 가 fire 되면 안 됨");
        Assert::AreEqual(1, pSession->GetOutstandingIoCountForTest(),
            L"첫 completion 이후 outstanding 은 1 이어야 함");

        pSession->CompleteSendFromExistingOutstanding(16);

        Assert::AreEqual(1, pSession->GetDisconnectedCountForTest(),
            L"마지막 completion 시점에 OnDisconnected 가 정확히 1회 fire 되어야 함");
        Assert::AreEqual(0, pSession->GetOutstandingIoCountForTest(),
            L"drain 완료 후 outstanding 은 0 이어야 함");
    }

    // LT-D1-Recv-01 (검증: 79685c0) — disc 이후 Real Recv completion 이 들어와도
    // derived OnPacketReceived 로 배달되지 않고, 통계만 갱신 + last-completion fire.
    TEST_METHOD(RecvDrop_AfterDisconnect_SuppressesOnPacketReceived)
    {
        auto pSession = MakeSession();
        pSession->SetOutstandingIoCountForTest(1);

        pSession->RequestDisconnect(LibNetworks::Sessions::DisconnectReason::Server);

        Assert::AreEqual(0, pSession->GetDisconnectedCountForTest(),
            L"pending=1 상태 disc 직후에는 OnDisconnected 가 fire 되면 안 됨");
        Assert::AreEqual(0, pSession->GetPacketReceivedCountForTest(),
            L"사전 조건: OnPacketReceived 는 아직 호출되면 안 됨");

        pSession->CompleteRealRecvFromExistingOutstanding(64);

        Assert::AreEqual(0, pSession->GetPacketReceivedCountForTest(),
            L"Rule D1 (79685c0): disc 이후 Real Recv completion 은 derived 로 배달되지 않아야 함");
        Assert::AreEqual(static_cast<std::uint64_t>(64), pSession->GetTotalRxBytes(),
            L"배달은 차단되지만 TotalRxBytes 통계는 실제 수신 바이트로 갱신되어야 함");
        Assert::AreEqual(1, pSession->GetDisconnectedCountForTest(),
            L"마지막 completion 이 drain 되면 OnDisconnected 가 1회 fire 되어야 함");
        Assert::AreEqual(0, pSession->GetOutstandingIoCountForTest(),
            L"drain 완료 후 outstanding 은 0 이어야 함");
    }

    // LT-D1-Send-01 (검증: 81ed822) — disc 이후 Send completion 이 들어와도
    // derived OnSent 가 호출되지 않고, 통계만 갱신 + last-completion fire.
    TEST_METHOD(OnSent_Skipped_AfterDisconnect)
    {
        auto pSession = MakeSession();
        pSession->SetOutstandingIoCountForTest(1);

        pSession->RequestDisconnect(LibNetworks::Sessions::DisconnectReason::Server);

        Assert::AreEqual(0, pSession->GetDisconnectedCountForTest(),
            L"pending=1 상태 disc 직후에는 OnDisconnected 가 fire 되면 안 됨");
        Assert::AreEqual(0, pSession->GetOnSentCountForTest(),
            L"사전 조건: OnSent 는 아직 호출되면 안 됨");

        pSession->CompleteSendFromExistingOutstanding(32);

        Assert::AreEqual(0, pSession->GetOnSentCountForTest(),
            L"Rule D1 (81ed822): disc 이후 Send completion 의 OnSent 는 호출되지 않아야 함");
        Assert::AreEqual(static_cast<std::uint64_t>(32), pSession->GetTotalTxBytes(),
            L"OnSent 는 차단되지만 TotalTxBytes 통계는 실제 송신 바이트로 갱신되어야 함");
        Assert::AreEqual(1, pSession->GetDisconnectedCountForTest(),
            L"마지막 completion 이 drain 되면 OnDisconnected 가 1회 fire 되어야 함");
        Assert::AreEqual(0, pSession->GetOutstandingIoCountForTest(),
            L"drain 완료 후 outstanding 은 0 이어야 함");
    }

    // DT-02 smoke: counter 가 0 이 아닌 상태로 destruct 될 때 ~IOSession 의 paranoid
    // ERROR 로그 경로를 타면서도 크래시 없이 정상 종료되는지 확인.
    // 참고: 실제 ERROR 로그 내용은 Logger mock 이 없어 assert 할 수 없음. 본 테스트는
    // "defensive 경로가 AV 없이 실행된다" 는 smoke 검증에 집중.
    TEST_METHOD(Destructor_DoesNotCrash_WhenStaleCounter)
    {
        {
            auto pSession = MakeSession();
            pSession->SetOutstandingIoCountForTest(5);
            // 여기서 scope 종료 → shared_ptr 소멸 → ~IOSession 실행.
            // counter != 0 이므로 paranoid ERROR 로그 경로 진입.
            // (실제 production 에서 이 경로는 invariant 위반 신호 — 테스트에서만
            // 인위적으로 유도.)
        }
        Assert::IsTrue(true,
            L"~IOSession() 은 counter != 0 이어도 ERROR 로그만 찍고 크래시 없이 반환되어야 함");
    }

    TEST_METHOD(ConnectOutstanding_UndoOnPostFailure_RestoresZero)
    {
        auto pSession = MakeOutboundSession();

        pSession->MarkConnectPostedForTest();

        Assert::AreEqual(1, pSession->GetOutstandingIoCountForTest(),
            L"ConnectEx posting 직후 outstanding 은 1이어야 함");

        pSession->UndoConnectPostedForTest();

        Assert::AreEqual(0, pSession->GetOutstandingIoCountForTest(),
            L"ConnectEx posting 실패 rollback 후 outstanding 은 0으로 복구되어야 함");
    }

    TEST_METHOD(ConnectCompletion_LastDrain_FiresOnDisconnected)
    {
        auto pSession = MakeOutboundSession();
        pSession->MarkConnectPostedForTest();

        pSession->RequestDisconnect(LibNetworks::Sessions::DisconnectReason::Server);

        Assert::AreEqual(0, pSession->GetDisconnectedCountForTest(),
            L"pending ConnectEx 가 남아 있으면 RequestDisconnect 직후에는 OnDisconnected 가 fire 되면 안 됨");
        Assert::AreEqual(1, pSession->GetOutstandingIoCountForTest(),
            L"ConnectEx pending 동안 outstanding 은 1이어야 함");

        pSession->CompleteConnectFromExistingOutstanding(false);

        Assert::AreEqual(1, pSession->GetDisconnectedCountForTest(),
            L"ConnectEx completion drain 이후 OnDisconnected 가 정확히 1회 fire 되어야 함");
        Assert::AreEqual(0, pSession->GetOutstandingIoCountForTest(),
            L"ConnectEx completion drain 이후 outstanding 은 0이어야 함");
    }

    TEST_METHOD(ConnectCompletion_AfterDisconnect_DoesNotCallOnConnected)
    {
        auto pSession = MakeOutboundSession();
        pSession->MarkConnectPostedForTest();

        pSession->RequestDisconnect(LibNetworks::Sessions::DisconnectReason::Server);
        pSession->CompleteConnectFromExistingOutstanding(true);

        Assert::AreEqual(0, pSession->GetConnectedCountForTest(),
            L"disconnect 요청 이후 뒤늦게 도착한 connect completion 은 OnConnected 를 호출하면 안 됨");
        Assert::AreEqual(1, pSession->GetDisconnectedCountForTest(),
            L"connect completion drain 이후 OnDisconnected 는 1회 fire 되어야 함");
        Assert::AreEqual(0, pSession->GetOutstandingIoCountForTest(),
            L"drain 완료 후 outstanding 은 0이어야 함");
    }
};

} // namespace LibNetworksTests
