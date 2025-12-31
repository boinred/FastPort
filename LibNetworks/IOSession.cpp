module;

#include <utility>
#include <WinSock2.h>
#include <spdlog/spdlog.h>

module networks.sessions.io_session;
import commons.logger;

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

void IOSession::SendBuffer(const char* pData, size_t dataLength)
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
            OnDisconnected();
            return;
        }

        // 0-byte recv는 graceful close로 간주
        if (bytesTransferred == 0)
        {
            LibCommons::Logger::GetInstance().LogInfo("IOSession", "OnIOCompleted() Recv 0 byte. Disconnected. Session Id : {}", GetSessionId());
            OnDisconnected();
            return;
        }

        if (!m_pReceiveBuffer->Write(m_RecvOverlapped.Buffers.data(), bytesTransferred))
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "OnIOCompleted() Receive buffer overflow. Session Id : {}, Bytes : {}", GetSessionId(), bytesTransferred);
            OnDisconnected();
            return;
        }

        PostRecv();
        return;
    }

    if (pOverlapped == &(m_SendOverlapped.Overlapped))
    {
        m_SendInProgress.store(false);

        if (!bSuccess)
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "OnIOCompleted() Send failed. Session Id : {}, Error Code : {}", GetSessionId(), GetLastError());
            OnDisconnected();
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
            OnDisconnected();
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

} // namespace LibNetworks::Sessions