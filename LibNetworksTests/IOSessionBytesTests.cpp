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

    // Real Recv 성공 완료를 시뮬레이트. CommitWrite(bytes) → m_TotalRxBytes += bytes.
    void SimulateRealRecvSuccess(DWORD bytes)
    {
        m_RecvOverlapped.IsZeroByte = false;
        this->OnIOCompleted(true, bytes, &m_RecvOverlapped.Overlapped);
    }

    // Zero-byte Recv 완료를 시뮬레이트 (bytesTransferred=0). 카운터는 변하지 않아야 함.
    void SimulateZeroByteRecvComplete()
    {
        m_RecvOverlapped.IsZeroByte = true;
        this->OnIOCompleted(true, 0, &m_RecvOverlapped.Overlapped);
    }

    // Send 완료를 시뮬레이트. m_TotalTxBytes += bytes.
    void SimulateSendSuccess(DWORD bytes)
    {
        this->OnIOCompleted(true, bytes, &m_SendOverlapped.Overlapped);
    }
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

} // namespace LibNetworksTests
