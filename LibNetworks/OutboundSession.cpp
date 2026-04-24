module;

#include <utility>
#include <functional>
#include <spdlog/spdlog.h>
module networks.sessions.outbound_session;
import commons.logger;

namespace LibNetworks::Sessions
{

OutboundSession::OutboundSession(const std::shared_ptr<Core::Socket>& pSocket,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
    : IOSession(std::move(pSocket), std::move(pReceiveBuffer), std::move(pSendBuffer))
{
    std::memset(&m_ConnectOverlapped, 0, sizeof(OVERLAPPED));
}

void OutboundSession::OnConnected()
{
    LibCommons::Logger::GetInstance().LogInfo("OutboundSession", "OnConnected. Session Id : {}", GetSessionId());

    StartReceiveLoop();
    NotifyActivationObserver();
}

void OutboundSession::OnDisconnected()
{
    LibCommons::Logger::GetInstance().LogInfo("OutboundSession", "OnDisconnected. Session Id : {}", GetSessionId());
    NotifyDisconnectObserver();
}

bool OutboundSession::IsConnectCompletion(const OVERLAPPED* pOverlapped) const
{
    return pOverlapped == &m_ConnectOverlapped;
}

void OutboundSession::MarkConnectIoPosted() noexcept
{
    AddOutstandingIo();
    m_ConnectIoPending.store(true, std::memory_order_release);
}

void OutboundSession::UndoConnectIoOnPostFailure() noexcept
{
    m_ConnectIoPending.store(false, std::memory_order_release);
    UndoOutstandingOnFailure("ConnectEx");
}

void OutboundSession::SetActivationObserver(std::function<void()> observer)
{
    m_ActivationObserver = std::move(observer);
}

void OutboundSession::SetDisconnectObserver(std::function<void()> observer)
{
    m_DisconnectObserver = std::move(observer);
}

void OutboundSession::NotifyActivationObserver()
{
    if (m_ActivationObserver)
    {
        m_ActivationObserver();
    }
}

void OutboundSession::NotifyDisconnectObserver()
{
    if (m_DisconnectObserver)
    {
        m_DisconnectObserver();
    }
}

void OutboundSession::ApplyConnectedSocketOptions()
{
    // 최적화 설정 적용
    GetSocket()->UpdateContextDisableNagleAlgorithm();  // Nagle Off
    GetSocket()->UpdateContextZeroCopy();               // Zero-Copy
    GetSocket()->UpdateContextKeepAlive(30000, 1000);   // Keep-Alive
}

bool OutboundSession::FinalizeConnect()
{
    if (!GetSocket()->UpdateConnectContext())
    {
        LibCommons::Logger::GetInstance().LogError("OutboundSession", "OnIOCompleted: UpdateConnectContext failed. Session Id : {}, Error : {}", GetSessionId(), ::GetLastError());
        RequestDisconnect();
        return false;
    }

    ApplyConnectedSocketOptions();

    LibCommons::Logger::GetInstance().LogInfo("SocketConnector", "OnIOCompleted: Connected to server. Session Id : {}", GetSessionId());

    OnConnected();
    return true;
}

void OutboundSession::OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped)
{
    if (!IsConnectCompletion(pOverlapped))
    {
        __super::OnIOCompleted(bSuccess, bytesTransferred, pOverlapped);
        return;
    }

	auto pOutboundSession = shared_from_this();

    IoCompletionGuard guard(pOutboundSession);
    m_ConnectIoPending.store(false, std::memory_order_release);

    if (IsDisconnectRequested())
    {
        LibCommons::Logger::GetInstance().LogDebug("OutboundSession",
            "OnIOCompleted: Connect completion ignored after disconnect request. Session Id : {}",
            GetSessionId());
        return;
    }

    if (!bSuccess)
    {
        DWORD dwError = ::GetLastError();

        LibCommons::Logger::GetInstance().LogError("OutboundSession", "OnIOCompleted: ConnectEx failed. Session Id : {}, Error : {}", GetSessionId(), dwError);

        RequestDisconnect();
        return;
    }

    FinalizeConnect();
}

} // namespace LibNetworks::Sessions
