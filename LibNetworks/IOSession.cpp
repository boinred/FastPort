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
import commons.event_listener;

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

void IOSession::SendBuffer(const unsigned char* pData, size_t dataLength)
{
    if (!pData || dataLength == 0 || !m_pSendBuffer)
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "SendBuffer() Invalid parameters. Session Id : {}", GetSessionId());

        return;
    }

    if (!m_pSendBuffer->Write(pData, dataLength))
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "SendBuffer() Failed to write data to send buffer. Session Id : {}, Data Length : {}", GetSessionId(), dataLength);

        return;
    }

    TryPostSendFromQueue();
}

void IOSession::SendMessage(const short packetId, const google::protobuf::Message& rfMessage)
{
    std::ostringstream os;
    rfMessage.SerializePartialToOstream(&os);

    const Core::Packet packet(packetId, os.str());

    SendBuffer(packet.GetRawData(), packet.GetPacketSize());
}

void IOSession::RequestReceived()
{
    PostRecv();
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
    bool expected = false;
    if (!m_SendInProgress.compare_exchange_strong(expected, true))
    {
        return true;
    }

    if (!m_pSendBuffer || m_pSendBuffer->CanReadSize() == 0)
    {
        m_SendInProgress.store(false);
        return true;
    }

    const size_t bytesToSend = std::min<size_t>(m_pSendBuffer->CanReadSize(), static_cast<size_t>(m_pSendBuffer->GetMaxSize()));

    m_SendOverlapped.Buffers.resize(bytesToSend);

    if (!m_pSendBuffer->Peek(m_SendOverlapped.Buffers.data(), bytesToSend))
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "TryPostSendFromQueue() Failed to peek data from send buffer. Session Id : {}, Bytes To Send : {}", GetSessionId(), bytesToSend);

        m_SendInProgress.store(false);

        return false;
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

        if (!m_pReceiveBuffer->Write(m_RecvOverlapped.Buffers.data(), bytesTransferred))
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

        if (!m_pSendBuffer->Consume(bytesTransferred))
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "OnIOCompleted() Send buffer consume failed. Session Id : {}, Bytes : {}", GetSessionId(), bytesTransferred);
            RequestDisconnect();
            return;
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

    ProcessReceiveQueue();
}

void IOSession::ProcessReceiveQueue()
{
    auto& logger = LibCommons::Logger::GetInstance();

    while (true)
    {
        const size_t canRead = m_pReceiveBuffer ? m_pReceiveBuffer->CanReadSize() : 0;
        if (canRead < Core::Packet::GetHeaderSize())
        {
            return;
        }

        unsigned char headerBuffer[Core::Packet::GetHeaderSize()]{}; // Use GetHeaderSize()
        if (!m_pReceiveBuffer->Peek(headerBuffer, Core::Packet::GetHeaderSize()))
        {
            logger.LogError("IOSession", "ProcessReceiveQueue() Peek header failed. Session Id : {}", GetSessionId());
            RequestDisconnect();
            return;
        }

        const auto bufferSize = Core::Packet::GetHeaderFromBuffer(headerBuffer);
        if (0 >= bufferSize)
        {
            logger.LogError("IOSession", "ProcessReceiveQueue() Invalid packet size. Session Id : {}, PacketSize : {}", GetSessionId(), bufferSize);
            RequestDisconnect();
            return;
        }

        if (canRead < bufferSize)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return; // wait for more data
        }

        std::vector<unsigned char> buffers;
        buffers.resize(bufferSize);

        if (!m_pReceiveBuffer->Pop(buffers.data(), bufferSize))
        {
            logger.LogError("IOSession", "ProcessReceiveQueue() Pop payload failed. Session Id : {}, Buffer Size : {}", GetSessionId(), bufferSize);
            RequestDisconnect();
            return;
        }

        // OnPacketReceived(Core::Packet(std::move(buffers)));
        auto self = shared_from_this();
        LibCommons::EventListener::GetInstance().PostTask([self, buffers = std::move(buffers)]() mutable
            {
                self->OnPacketReceived(Core::Packet(std::move(buffers)));
            });
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