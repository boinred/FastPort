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
import networks.core.socket;
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

protected:
    // 테스트 전용 disconnect callback 집계.
    void OnDisconnected() override
    {
        m_DisconnectedCount.fetch_add(1);
    }

private:
    std::atomic<int> m_DisconnectedCount { 0 };
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
};

} // namespace LibNetworksTests
