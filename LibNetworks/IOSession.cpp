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

}

void IOSession::SendBuffer(const char* pData, size_t dataLength)
{
    if (!pData || dataLength == 0 || !m_pSendBuffer)
    {
        return;
    }

    if (!m_pSendBuffer->Write(pData, dataLength))
    {
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
    wsaBuf.buf = m_RecvTempBuffer.data();
    wsaBuf.len = static_cast<ULONG>(m_RecvTempBuffer.size());

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

    const size_t bytesToSend = std::min<size_t>(m_pSendBuffer->CanReadSize(), 64 * 1024);

    m_SendOverlapped.Buffers.resize(bytesToSend);

    if (!m_pSendBuffer->Peek(m_SendOverlapped.Buffers.data(), bytesToSend))
    {
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
    if (pOverlapped == &m_RecvOverlapped.Overlapped)
    {
        m_RecvInProgress.store(false);

        if (!bSuccess)
        {
            return;
        }

        if (bytesTransferred > 0)
        {
            OnReceive(m_RecvTempBuffer.data(), bytesTransferred);
            (void)PostRecv();
        }

        return;
    }

    if (pOverlapped == &m_SendOverlapped.Overlapped)
    {
        m_SendInProgress.store(false);

        if (!bSuccess)
        {
            return;
        }

        if (m_pSendBuffer)
        {
            (void)m_pSendBuffer->Consume(bytesTransferred);
        }

        const bool hasPending = m_pSendBuffer && (m_pSendBuffer->CanReadSize() > 0);

        OnSent(bytesTransferred);

        if (hasPending)
        {
            (void)TryPostSendFromQueue();
        }

        return;
    }
}

} // namespace LibNetworks::Sessions