module;

#include <WinSock2.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <string>
#include <span>
#include <chrono>
#include <cstdint>

module networks.sessions.io_session;

import commons.logger;
import networks.core.packet;
import networks.core.packet_framer;
import networks.core.socket;


namespace LibNetworks::Sessions
{

namespace
{
// Design Ref: session-idle-timeout §4.2 — steady_clock 기준 epoch-ms.
// idle 비교의 기준 시간. wall clock 대신 steady_clock 사용으로 시스템 시각 변경 영향 없음.
inline std::int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // anonymous namespace


IOSession::IOSession(const std::shared_ptr<Core::Socket>& pSocket,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
    : m_pSocket(std::move(pSocket))
{
    m_pReceiveBuffer = std::move(pReceiveBuffer);
    m_pSendBuffer = std::move(pSendBuffer);

    // Recv는 고정 크기 버퍼를 재사용.
    m_RecvOverlapped.Buffers.resize(16 * 1024);
}

// # 소멸 시점 불변식 검증
IOSession::~IOSession()
{
    const int finalCount = m_OutstandingIoCount.load(std::memory_order_acquire);
    const bool wasDisconnectRequested = m_DisconnectRequested.load(std::memory_order_acquire);
    const bool wasOnDisconnectedFired = m_bOnDisconnectedFired.load(std::memory_order_acquire);

    if (finalCount != 0)
    {
        LibCommons::Logger::GetInstance().LogError("IOSession",
            "~IOSession() invariant violation. Session Id : {}, Outstanding : {}, DisconnectRequested : {}, OnDisconnectedFired : {}",
            GetSessionId(), finalCount, wasDisconnectRequested, wasOnDisconnectedFired);
        return;
    }

    LibCommons::Logger::GetInstance().LogDebug("IOSession",
        "~IOSession() Session Id : {}, DisconnectRequested : {}, OnDisconnectedFired : {}",
        GetSessionId(), wasDisconnectRequested, wasOnDisconnectedFired);
}

// # 완료 통지 종료 게이트
IOSession::IoCompletionGuard::~IoCompletionGuard() noexcept
{
    const int previousCount = Self.m_OutstandingIoCount.fetch_sub(1, std::memory_order_acq_rel);
    if (previousCount == 1 && Self.m_DisconnectRequested.load(std::memory_order_acquire))
    {
        Self.TryFireOnDisconnected();
    }
}

// # 종료 이후 송신 차단
void IOSession::SendBuffer(std::span<const std::byte> data)
{
    if (data.empty() || !m_pSendBuffer)
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "SendBuffer() Invalid parameters. Session Id : {}", GetSessionId());

        return;
    }

    if (m_DisconnectRequested.load(std::memory_order_acquire))
    {
        LibCommons::Logger::GetInstance().LogDebug("IOSession",
            "SendBuffer() skipped after disconnect request. Session Id : {}",
            GetSessionId());
        return;
    }

    if (!m_pSendBuffer->Write(data))
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "SendBuffer() Failed to write data to send buffer. Session Id : {}, Data Length : {}", GetSessionId(), data.size());

        return;
    }

    TryPostSendFromQueue();
}

// 헬퍼 함수: 버퍼 조각들에 데이터를 씁니다.
static void WriteToBuffers(const std::vector<std::span<std::byte>>& buffers, size_t& bufferIdx, size_t& offsetInSpan, const void* data, size_t size)
{
    const unsigned char* src = static_cast<const unsigned char*>(data);
    size_t remaining = size;

    while (remaining > 0 && bufferIdx < buffers.size())
    {
        std::span<std::byte> span = buffers[bufferIdx];
        size_t spaceInSpan = span.size() - offsetInSpan;
        size_t toCopy = remaining < spaceInSpan ? remaining : spaceInSpan;


        std::memcpy(span.data() + offsetInSpan, src, toCopy);

        src += toCopy;
        remaining -= toCopy;
        offsetInSpan += toCopy;

        if (offsetInSpan == span.size())
        {
            bufferIdx++;
            offsetInSpan = 0;
        }
    }
}

// # 종료 이후 메시지 차단
void IOSession::SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage)
{
    const size_t bodySize = rfMessage.ByteSizeLong();
    const size_t totalSize = Core::Packet::GetHeaderSize() + Core::Packet::GetPacketIdSize() + bodySize;

    if (m_DisconnectRequested.load(std::memory_order_acquire))
    {
        LibCommons::Logger::GetInstance().LogDebug("IOSession",
            "SendMessage() skipped after disconnect request. Session Id : {}, Packet Id : {}",
            GetSessionId(), packetId);
        return;
    }

    std::vector<std::span<std::byte>> buffers;
    // 링버퍼에 직접 공간 예약 (실패 시 전송 불가)
    if (!m_pSendBuffer->AllocateWrite(totalSize, buffers))
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "SendMessage() Send buffer overflow. Session Id : {}, Packet Size : {}", GetSessionId(), totalSize);
        RequestDisconnect();
        return;
    }

    // 헤더 및 패킷 ID 직렬화 (네트워크 바이트 오더)
    uint16_t sizeNet = htons(static_cast<uint16_t>(totalSize));
    uint16_t idNet = htons(packetId);

    size_t bufferIdx = 0;
    size_t offsetInSpan = 0;

    WriteToBuffers(buffers, bufferIdx, offsetInSpan, &sizeNet, sizeof(sizeNet));
    WriteToBuffers(buffers, bufferIdx, offsetInSpan, &idNet, sizeof(idNet));

    // Protobuf Body 직렬화
    if (bodySize > 0)
    {
        // 최적화: 버퍼가 1개이고 공간이 충분하면 SerializeToArray 사용 (가장 빠름)
        if (bufferIdx < buffers.size() && buffers[bufferIdx].size() - offsetInSpan >= bodySize)
        {
            rfMessage.SerializeToArray(buffers[bufferIdx].data() + offsetInSpan, static_cast<int>(bodySize));
        }
        else
        {
            // 버퍼가 쪼개져 있거나(Wrap around), 공간이 부족한 경우
            std::string temp; // Fallback
            rfMessage.SerializeToString(&temp);
            WriteToBuffers(buffers, bufferIdx, offsetInSpan, temp.data(), temp.size());
        }
    }


    TryPostSendFromQueue();
}

// # 최초 수신 루프 개시
void IOSession::StartReceiveLoop()
{
    if (m_DisconnectRequested.load(std::memory_order_acquire))
    {
        return;
    }

    bool expected = false;
    if (!m_ReceiveLoopStarted.compare_exchange_strong(expected, true))
    {
        return;
    }

    RequestReceived();
}

// # 수신 재등록 진입점
void IOSession::RequestReceived()
{
    if (m_DisconnectRequested.load(std::memory_order_acquire))
    {
        return;
    }

    // 이미 진행 중인지 확인
    bool expected = false;
    if (!m_RecvInProgress.compare_exchange_strong(expected, true))
    {
        return;
    }

    // 비동기 0바이트 수신(WSARecv) 등록.
    if (!RequestRecv(true))
    {
        m_RecvInProgress.store(false);
        RequestDisconnect();
    }
}

// # 수신 posting 준비
bool IOSession::PrepareRecvBuffers(bool bZeroByte)
{
    m_RecvOverlapped.ResetOverlapped();
    m_RecvOverlapped.IsZeroByte = bZeroByte;
    m_RecvOverlapped.WSABufs.clear();

    if (bZeroByte)
    {
        WSABUF wsaBuf{};
        wsaBuf.buf = nullptr;
        wsaBuf.len = 0;
        m_RecvOverlapped.WSABufs.push_back(wsaBuf);
        return true;
    }

    std::vector<std::span<std::byte>> buffers;
    size_t writableSize = 0;
    if (m_pReceiveBuffer)
    {
        writableSize = m_pReceiveBuffer->GetWriteableBuffers(buffers);
    }

    if (writableSize == 0)
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "PostRecvImpl(Real) Receive buffer full. Session Id : {}", GetSessionId());
        RequestDisconnect();
        return false;
    }

    m_RecvOverlapped.WSABufs.reserve(buffers.size());
    for (const auto& span : buffers)
    {
        WSABUF wsaBuf{};
        wsaBuf.buf = reinterpret_cast<char*>(span.data());
        wsaBuf.len = static_cast<ULONG>(span.size());
        m_RecvOverlapped.WSABufs.push_back(wsaBuf);
    }

    return true;
}

// # 수신 posting 카운터 증가
bool IOSession::RequestRecv(bool bZeroByte)
{
    if (!PrepareRecvBuffers(bZeroByte))
    {
        return false;
    }

    if (!m_pSocket)
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "RequestRecv() Socket is null. Session Id : {}", GetSessionId());
        return false;
    }

    m_OutstandingIoCount.fetch_add(1, std::memory_order_acq_rel);

    DWORD flags = 0;
    DWORD bytes = 0;

    int result = ::WSARecv(m_pSocket->GetSocket(),
        m_RecvOverlapped.WSABufs.data(),
        static_cast<DWORD>(m_RecvOverlapped.WSABufs.size()),
        &bytes,
        &flags,
        &m_RecvOverlapped.Overlapped,
        nullptr);

    if (result == SOCKET_ERROR)
    {
        int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "RequestRecv() WSARecv failed. Session Id : {}, Error Code : {}, ZeroByte : {}", GetSessionId(), err, bZeroByte);
            UndoOutstandingOnFailure("RequestRecv");
            return false;
        }
    }

    return true;
}

// # 송신 posting 준비
void IOSession::PrepareSendBuffers(const std::vector<std::span<const std::byte>>& buffers, size_t bytesToSend)
{
    m_SendOverlapped.RequestedBytes = bytesToSend;
    m_SendOverlapped.ResetOverlapped();

    // WSABUF 배열 구성
    m_SendOverlapped.WSABufs.clear();
    m_SendOverlapped.WSABufs.reserve(buffers.size());
    for (const auto& span : buffers)
    {
        WSABUF wsaBuf{};
        wsaBuf.buf = const_cast<char*>(reinterpret_cast<const char*>(span.data()));
        wsaBuf.len = static_cast<ULONG>(span.size());
        m_SendOverlapped.WSABufs.push_back(wsaBuf);
    }
}

// # 송신 posting 카운터 증가
bool IOSession::TryPostSendFromQueue()
{
    if (m_DisconnectRequested.load(std::memory_order_acquire))
    {
        return false;
    }

    // Send는 outstanding 1개만 유지
    bool expected = false;
    if (!m_SendInProgress.compare_exchange_strong(expected, true))
    {
        return true;
    }

    if (!m_pSendBuffer)
    {
        m_SendInProgress.store(false);
        return true;
    }

    std::vector<std::span<const std::byte>> buffers;
    const size_t bytesToSend = m_pSendBuffer->GetReadBuffers(buffers);

    if (bytesToSend == 0)
    {
        m_SendInProgress.store(false);
        return true;
    }

    PrepareSendBuffers(buffers, bytesToSend);

    if (!m_pSocket)
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "TryPostSendFromQueue() Socket is null. Session Id : {}", GetSessionId());
        m_SendInProgress.store(false);
        return false;
    }

    m_OutstandingIoCount.fetch_add(1, std::memory_order_acq_rel);

    DWORD bytesSent = 0;
    int result = ::WSASend(m_pSocket->GetSocket(),
        m_SendOverlapped.WSABufs.data(),
        static_cast<DWORD>(m_SendOverlapped.WSABufs.size()),
        &bytesSent,
        0,
        &m_SendOverlapped.Overlapped,
        nullptr);

    if (result == SOCKET_ERROR)
    {
        int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "TryPostSendFromQueue() WSASend failed. Session Id : {}, Error Code : {}", GetSessionId(), err);

            UndoOutstandingOnFailure("TryPostSendFromQueue");
            m_SendInProgress.store(false);
            return false;
        }
    }

    return true;
}

// # 제로바이트 수신 전환
void IOSession::HandleZeroByteRecvCompletion(DWORD bytesTransferred)
{
    if (bytesTransferred == 0)
    {
        if (!RequestRecv(false))
        {
            m_RecvInProgress.store(false);
            RequestDisconnect();
        }
        return;
    }

    m_RecvInProgress.store(false);
}

// # 실제 수신 후속 처리
void IOSession::HandleRealRecvCompletion(DWORD bytesTransferred)
{
    if (bytesTransferred == 0)
    {
        m_RecvInProgress.store(false);
        LibCommons::Logger::GetInstance().LogInfo("IOSession", "OnIOCompleted() Recv 0 byte (Real). Disconnected. Session Id : {}", GetSessionId());
        RequestDisconnect();
        return;
    }

    // Zero-Copy Recv Commit
    if (!m_pReceiveBuffer->CommitWrite(bytesTransferred))
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "OnIOCompleted() CommitWrite failed (Overflow?). Session Id : {}, Bytes : {}", GetSessionId(), bytesTransferred);
        m_RecvInProgress.store(false);
        RequestDisconnect();
        return;
    }

    // Design Ref: session-idle-timeout §3, §4.2 — 생존 증거 기록.
    // bytes > 0 수신 완료 후에만 갱신. Zero-byte Recv 는 수신 이력이 아니므로 제외.
    m_LastRecvTimeMs.store(NowMs(), std::memory_order_relaxed);

    // Design Ref: server-status §3.3 — 누적 수신 바이트.
    m_TotalRxBytes.fetch_add(bytesTransferred, std::memory_order_relaxed);

    // # 멱등성 및 Rule D1 준수: 종료 요청 상태라면 상위 레이어로 패킷을 배달하지 않는다.
    if (m_DisconnectRequested.load(std::memory_order_acquire))
    {
        LibCommons::Logger::GetInstance().LogDebug("IOSession",
            "HandleRealRecvCompletion() dropping data dispatch due to disconnect request. Session Id : {}",
            GetSessionId());
        m_RecvInProgress.store(false);
        return;
    }

    ReadReceivedBuffers();

    m_RecvInProgress.store(false);
    RequestReceived();
}

// # 수신 완료 분기
void IOSession::HandleRecvCompletion(bool bSuccess, DWORD bytesTransferred)
{
    if (!bSuccess)
    {
        m_RecvInProgress.store(false);
        LibCommons::Logger::GetInstance().LogInfo("IOSession", "OnIOCompleted() Recv failed. Session Id : {}, Error Code : {}", GetSessionId(), GetLastError());
        RequestDisconnect();
        return;
    }

    if (m_RecvOverlapped.IsZeroByte)
    {
        HandleZeroByteRecvCompletion(bytesTransferred);
        return;
    }

    HandleRealRecvCompletion(bytesTransferred);
}

// # 송신 완료 후속 처리
void IOSession::HandleSendCompletion(bool bSuccess, DWORD bytesTransferred)
{
    m_SendInProgress.store(false);

    if (!bSuccess)
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "OnIOCompleted() Send failed. Session Id : {}, Error Code : {}", GetSessionId(), GetLastError());
        RequestDisconnect();
        return;
    }

    // 전송 완료된 만큼 버퍼 비우기 (Delayed Consume)
    if (m_pSendBuffer)
    {
        m_pSendBuffer->Consume(bytesTransferred);
    }

    // Design Ref: server-status §3.3 — 누적 송신 바이트.
    m_TotalTxBytes.fetch_add(bytesTransferred, std::memory_order_relaxed);

    // # 종료 요청 이후 상위 송신 콜백 차단
    if (m_DisconnectRequested.load(std::memory_order_acquire))
    {
        LibCommons::Logger::GetInstance().LogDebug("IOSession",
            "HandleSendCompletion() skipping OnSent due to disconnect request. Session Id : {}",
            GetSessionId());
        return;
    }

    OnSent(bytesTransferred);

    const bool hasPending = m_pSendBuffer && (m_pSendBuffer->CanReadSize() > 0);
    if (hasPending && !m_DisconnectRequested.load(std::memory_order_acquire))
    {
        TryPostSendFromQueue();
    }
}

// # 완료 통지 수명 보호
void IOSession::OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped)
{
    if (!pOverlapped)
    {
        return;
    }

    IoCompletionGuard guard(*this);

    // 멤버 Overlapped 주소로 구분
    if (pOverlapped == &(m_RecvOverlapped.Overlapped))
    {
        HandleRecvCompletion(bSuccess, bytesTransferred);
        return;
    }

    if (pOverlapped == &(m_SendOverlapped.Overlapped))
    {
        HandleSendCompletion(bSuccess, bytesTransferred);
        return;
    }

    LibCommons::Logger::GetInstance().LogError("IOSession",
        "OnIOCompleted() Unknown OVERLAPPED. Session Id : {}",
        GetSessionId());
}

// # 수신 버퍼 프레이밍 처리
void IOSession::ReadReceivedBuffers()
{
    if (!m_pReceiveBuffer)
    {
        return;
    }

    auto& logger = LibCommons::Logger::GetInstance();

    while (true)
    {
        auto frame = Core::PacketFramer::TryFrameFromBuffer(*m_pReceiveBuffer);
        if (frame.Result == Core::PacketFrameResult::NeedMore)
        {
            break;
        }

        if (frame.Result == Core::PacketFrameResult::Invalid)
        {
            logger.LogError("IOSession", "ReadReceivedBuffers() Invalid packet frame. Session Id : {}", GetSessionId());
            RequestDisconnect();
            break;
        }

        if (!frame.PacketOpt.has_value())
        {
            logger.LogError("IOSession", "ReadReceivedBuffers() Packet frame ok but packet missing. Session Id : {}", GetSessionId());
            RequestDisconnect();
            break;
        }

        OnPacketReceived(*frame.PacketOpt);
    }
}

// Design Ref: session-idle-timeout §4.2 — 기존 호출자(8곳) 호환을 위한 무인자 버전.
// 내부적으로 Normal 사유로 delegation. 신규 호출부는 가능한 reason 명시 버전 사용 권장.
// # 기본 종료 요청 위임
void IOSession::RequestDisconnect()
{
    RequestDisconnect(DisconnectReason::Normal);
}

// Design Ref: session-idle-timeout §4.2, §6.3 — 이중 호출 방지 CAS + 사유별 로그.
// IdleChecker 가 tick 콜백에서 직접 호출. m_DisconnectRequested 로 race 안전.
// # 비동기 종료 전이
void IOSession::RequestDisconnect(DisconnectReason reason)
{
    auto& logger = LibCommons::Logger::GetInstance();

    bool expected = false;
    const bool firstPass = m_DisconnectRequested.compare_exchange_strong(expected, true);
    if (firstPass)
    {
        // Idle 감지 시에만 idle duration 부가 로그. 다른 사유는 간단히.
        if (reason == DisconnectReason::IdleTimeout)
        {
            const std::int64_t lastMs = m_LastRecvTimeMs.load(std::memory_order_relaxed);
            const std::int64_t idleMs = (lastMs > 0) ? (NowMs() - lastMs) : -1;
            logger.LogInfo("IOSession",
                "IdleTimeout detected. Session Id : {}, IdleMs : {}",
                GetSessionId(), idleMs);
        }

        if (!m_pSocket)
        {
            logger.LogWarning("IOSession",
                "RequestDisconnect() Socket is null. Session Id : {}, Reason : {}",
                GetSessionId(), static_cast<int>(reason));
        }
        else
        {
            m_pSocket->Shutdown(SD_BOTH);
            m_pSocket->Close();
        }

        m_RecvInProgress.store(false);
        m_SendInProgress.store(false);

        logger.LogInfo("IOSession",
            "RequestDisconnect() initiated. Session Id : {}, Reason : {}, Outstanding : {}",
            GetSessionId(), static_cast<int>(reason),
            m_OutstandingIoCount.load(std::memory_order_acquire));
    }
    else
    {
        logger.LogDebug("IOSession",
            "RequestDisconnect() idempotent skip. Session Id : {}, Reason : {}",
            GetSessionId(), static_cast<int>(reason));
    }

    if (m_OutstandingIoCount.load(std::memory_order_acquire) == 0)
    {
        TryFireOnDisconnected();
    }
}

// # 종료 콜백 단일 발화
void IOSession::TryFireOnDisconnected()
{
    bool expected = false;
    if (!m_bOnDisconnectedFired.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return;
    }

    auto& logger = LibCommons::Logger::GetInstance();
    logger.LogInfo("IOSession",
        "TryFireOnDisconnected() firing. Session Id : {}",
        GetSessionId());

    if (m_pReceiveBuffer)
    {
        m_pReceiveBuffer->Clear();
    }

    if (m_pSendBuffer)
    {
        m_pSendBuffer->Clear();
    }

    OnDisconnected();
}

// # posting 실패 카운터 복구
void IOSession::UndoOutstandingOnFailure(const char* site) noexcept
{
    const int previousCount = m_OutstandingIoCount.fetch_sub(1, std::memory_order_acq_rel);
    if (previousCount <= 0)
    {
        LibCommons::Logger::GetInstance().LogError("IOSession",
            "UndoOutstandingOnFailure() underflow. Session Id : {}, Site : {}, Previous : {}",
            GetSessionId(), site, previousCount);
    }
}



} // namespace LibNetworks::Sessions
