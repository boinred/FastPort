module;

#include <utility>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <WinSock2.h>
#include <spdlog/spdlog.h>

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

void IOSession::SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage)
{
    std::ostringstream os;
    rfMessage.SerializePartialToOstream(&os);

    const Core::Packet packet(packetId, os.str());

    SendBuffer(packet.GetRawSpan());
}

void IOSession::RequestReceived()
{
    if (!PostRecv())
    {
        RequestDisconnect();
    }
}

bool IOSession::PostRecv()
{
    bool expected = false;
    if (!m_RecvInProgress.compare_exchange_strong(expected, true))
    {
        return true;
    }

    m_RecvOverlapped.ResetOverlapped();

    WSABUF wsaBuf{};
    wsaBuf.buf = m_RecvOverlapped.Buffers.data();
    wsaBuf.len = static_cast<ULONG>(m_RecvOverlapped.Buffers.size());

    DWORD flags = 0;
    DWORD bytes = 0;

    int result = ::WSARecv(m_pSocket->GetSocket(), &wsaBuf, 1, &bytes, &flags, &m_RecvOverlapped.Overlapped, nullptr);
    if (result == SOCKET_ERROR)
    {
        int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "PostRecv() WSARecv failed. Session Id : {}, Error Code : {}", GetSessionId(), err);

            m_RecvInProgress.store(false);
            return false;
        }
    }

    return true;
}

bool IOSession::TryPostSendFromQueue()
{
    // Send는 outstanding 1개만 유지
    // 1. 다른 스레드가 이미 전송 작업을 시작했는지 확인 (진입 잠금)
    bool expected = false;
    if (!m_SendInProgress.compare_exchange_strong(expected, true))
    {
        // 1. 다른 스레드가 이미 전송 작업을 시작했는지 확인 (진입 잠금)
        return true;
    }

    if (!m_pSendBuffer)
    {
        m_SendInProgress.store(false);
        return true;
    }

    // Peek은 원자적으로 m_Size만큼 버퍼를 채우고, 그 크기를 반환합니다.
    const size_t bytesToSend = m_pSendBuffer->Peek(m_SendOverlapped.Buffers);
    if (bytesToSend == 0)
    {
        m_SendInProgress.store(false);
        return true;
    }


    m_SendOverlapped.RequestedBytes = bytesToSend;
    m_SendOverlapped.ResetOverlapped();

    WSABUF wsaBuf{};
    wsaBuf.buf = m_SendOverlapped.Buffers.data();
    wsaBuf.len = static_cast<ULONG>(m_SendOverlapped.Buffers.size());

    DWORD bytes = 0;

    int result = ::WSASend(m_pSocket->GetSocket(), &wsaBuf, 1, &bytes, 0, &m_SendOverlapped.Overlapped, nullptr);
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

    m_pSendBuffer->Consume(bytesToSend);

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
        m_RecvInProgress.store(false);

        if (!bSuccess)
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "OnIOCompleted() Recv failed. Session Id : {}, Error Code : {}", GetSessionId(), GetLastError());
            RequestDisconnect();
            return;
        }

        if (bytesTransferred == 0)
        {
            LibCommons::Logger::GetInstance().LogInfo("IOSession", "OnIOCompleted() Recv 0 byte. Disconnected. Session Id : {}", GetSessionId());
            RequestDisconnect();
            return;
        }

        if (!m_pReceiveBuffer->Write(std::as_bytes(std::span(m_RecvOverlapped.Buffers.data(), bytesTransferred))))
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "OnIOCompleted() Receive buffer overflow. Session Id : {}, Bytes : {}", GetSessionId(), bytesTransferred);
            RequestDisconnect();
            return;
        }

        ReadReceivedBuffers();

        RequestReceived();

        return;
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
        if (requested > 0 && bytesTransferred > requested)
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "OnIOCompleted() Send bytesTransferred is larger than requested. Session Id : {}, Requested : {}, Transferred : {}", GetSessionId(), requested, bytesTransferred);
            bytesTransferred = static_cast<DWORD>(requested);
        }

        if (requested > 0 && bytesTransferred < requested)
        {
            LibCommons::Logger::GetInstance().LogDebug("IOSession", "OnIOCompleted() Partial send. Session Id : {}, Requested : {}, Transferred : {}", GetSessionId(), requested, bytesTransferred);
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
        auto frame = m_PacketFramer.TryPop(*m_pReceiveBuffer);
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