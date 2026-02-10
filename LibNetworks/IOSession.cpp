module;

#include <WinSock2.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <string>
#include <span>

module networks.sessions.io_session;

import commons.logger;
import networks.core.packet;
import networks.core.packet_framer;


namespace LibNetworks::Sessions
{

IOSession::IOSession(const std::shared_ptr<Core::Socket>& pSocket,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
    : m_pReceiveBuffer(std::move(pReceiveBuffer)),
    m_pSendBuffer(std::move(pSendBuffer)),
    m_pSocket(std::move(pSocket))
{
    // Recv는 고정 크기 버퍼를 재사용.
    m_RecvOverlapped.Buffers.resize(16 * 1024);
}

void IOSession::SendBuffer(std::span<const std::byte> data)
{
    if (data.empty() || !m_pSendBuffer)
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "SendBuffer() Invalid parameters. Session Id : {}", GetSessionId());

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

void IOSession::SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage)
{
    const size_t bodySize = rfMessage.ByteSizeLong();
    const size_t totalSize = Core::Packet::GetHeaderSize() + Core::Packet::GetPacketIdSize() + bodySize;

    if (!m_pSendBuffer)
    {
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

void IOSession::RequestReceived()
{
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


bool IOSession::RequestRecv(bool bZeroByte)
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
    }
    else
    {
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
    }

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
            return false;
        }
    }

    return true;
}

bool IOSession::TryPostSendFromQueue()
{
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

            m_SendInProgress.store(false);
            return false;
        }
    }

    return true;
}

void IOSession::OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped)
{
    if (!pOverlapped)
    {
        return;
    }

    // 멤버 Overlapped 주소로 구분
    if (pOverlapped == &(m_RecvOverlapped.Overlapped))
    {
        if (!bSuccess)
        {
            m_RecvInProgress.store(false);
            LibCommons::Logger::GetInstance().LogError("IOSession", "OnIOCompleted() Recv failed. Session Id : {}, Error Code : {}", GetSessionId(), GetLastError());
            RequestDisconnect();
            return;
        }

        // 1. Zero-byte Recv 완료 처리
        if (m_RecvOverlapped.IsZeroByte)
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
            else
            {
                m_RecvInProgress.store(false);
                return;
            }
        }
        else // 2. Real Recv 완료 처리
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

            ReadReceivedBuffers();

            m_RecvInProgress.store(false);
            RequestReceived();

            return;
        }
    }

    if (pOverlapped == &(m_SendOverlapped.Overlapped))
    {
        m_SendInProgress.store(false);

        if (!bSuccess)
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "OnIOCompleted() Send failed. Session Id : {}, Error Code : {}", GetSessionId(), GetLastError());
            RequestDisconnect();
            return;
        }

        const size_t requested = m_SendOverlapped.RequestedBytes;

        // 전송 완료된 만큼 버퍼 비우기 (Delayed Consume)
        if (m_pSendBuffer)
        {
            m_pSendBuffer->Consume(bytesTransferred);
        }

        OnSent(bytesTransferred);

        const bool hasPending = m_pSendBuffer && (m_pSendBuffer->CanReadSize() > 0);

        if (hasPending)
        {
            TryPostSendFromQueue();
        }

        return;
    }
}

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

void IOSession::RequestDisconnect()
{
    auto& logger = LibCommons::Logger::GetInstance();

    bool expected = false;
    if (!m_DisconnectRequested.compare_exchange_strong(expected, true))
    {
        return;
    }

    if (!m_pSocket)
    {
        logger.LogWarning("IOSession", "RequestDisconnect() Socket is null. Session Id : {}", GetSessionId());
        OnDisconnected();
        return;
    }

    // 강제 종료: send/recv 중단 + 즉시 close
    m_pSocket->Shutdown(SD_BOTH);
    m_pSocket->Close();

    m_RecvInProgress.store(false);
    m_SendInProgress.store(false);

    if (m_pReceiveBuffer)
    {
        m_pReceiveBuffer->Clear();
    }

    if (m_pSendBuffer)
    {
        m_pSendBuffer->Clear();
    }

    logger.LogInfo("IOSession", "RequestDisconnect() Socket closed. Session Id : {}", GetSessionId());

    OnDisconnected();
}



} // namespace LibNetworks::Sessions