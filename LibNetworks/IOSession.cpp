module;

#include <utility>
#include <WinSock2.h>

module networks.sessions.io_session;

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

    (void)TryPostSendFromQueue();
}

void IOSession::RequestReceived()
{
    (void)PostRecv();
}

bool IOSession::PostRecv()
{
    auto pOverlapped = new OverlappedEx{};
    pOverlapped->Type = IOType::Recv;

    WSABUF wsaBuf{};
    wsaBuf.buf = m_RecvTempBuffer.data();
    wsaBuf.len = static_cast<ULONG>(m_RecvTempBuffer.size());

    DWORD flags = 0;
    DWORD bytes = 0;

    int result = ::WSARecv(
        m_pSocket->GetSocket(),
        &wsaBuf,
        1,
        &bytes,
        &flags,
        &pOverlapped->Overlapped,
        nullptr);

    if (result == SOCKET_ERROR)
    {
        int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            delete pOverlapped;
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

    std::unique_ptr<OverlappedEx> pOverlapped(new OverlappedEx{});
    pOverlapped->Type = IOType::Send;

    if (!m_pSendBuffer || m_pSendBuffer->CanReadSize() == 0)
    {
        m_SendInProgress.store(false);
        return true;
    }

    const size_t bytesToSend = std::min<size_t>(m_pSendBuffer->CanReadSize(), 64 * 1024);
    pOverlapped->SendBuffer.resize(bytesToSend);

    if (!m_pSendBuffer->Peek(pOverlapped->SendBuffer.data(), bytesToSend))
    {
        m_SendInProgress.store(false);
        return false;
    }

    pOverlapped->RequestedBytes = bytesToSend;

    WSABUF wsaBuf{};
    wsaBuf.buf = pOverlapped->SendBuffer.data();
    wsaBuf.len = static_cast<ULONG>(pOverlapped->SendBuffer.size());

    DWORD bytes = 0;
    auto raw = pOverlapped.release();

    int result = ::WSASend(
        m_pSocket->GetSocket(),
        &wsaBuf,
        1,
        &bytes,
        0,
        &raw->Overlapped,
        nullptr);

    if (result == SOCKET_ERROR)
    {
        int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            delete raw;
            m_SendInProgress.store(false);
            return false;
        }
    }

    return true;
}

void IOSession::OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped)
{
    auto pOp = reinterpret_cast<OverlappedEx*>(pOverlapped);
    std::unique_ptr<OverlappedEx> op(pOp);

    if (!op)
    {
        return;
    }

    if (!bSuccess)
    {
        if (op->Type == IOType::Send)
        {
            m_SendInProgress.store(false);
        }
        return;
    }

    switch (op->Type)
    {
    case IOType::Recv:
        if (bytesTransferred > 0)
        {
            OnReceive(m_RecvTempBuffer.data(), bytesTransferred);
            (void)PostRecv();
        }
        break;

    case IOType::Send:
    {
        
        m_pSendBuffer->Consume(bytesTransferred);
        bool hasPending = m_pSendBuffer->CanReadSize() > 0;
        OnSent(bytesTransferred);

        m_SendInProgress.store(false);

        if (hasPending)
        {
            TryPostSendFromQueue();
        }
        break;
    }
    }

} // namespace LibNetworks::Sessions