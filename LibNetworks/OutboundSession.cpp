module;

#include <utility>
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

    RequestReceived();
}

void OutboundSession::OnDisconnected()
{
    LibCommons::Logger::GetInstance().LogInfo("OutboundSession", "OnDisconnected. Session Id : {}", GetSessionId());
    //throw std::logic_error("The method or operation is not implemented.");
}

void OutboundSession::OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped)
{
    __super::OnIOCompleted(bSuccess, bytesTransferred, pOverlapped);

    if (!bSuccess)
    {
        DWORD dwError = ::GetLastError();

        LibCommons::Logger::GetInstance().LogError("OutboundSession", "OnIOCompleted: ConnectEx failed. Session Id : {}, Error : {}", GetSessionId(), dwError);

        OnDisconnected();
        return;
    }

    auto& rfConnectOverlapped = GetConnectOverlapped();
    if (pOverlapped == &rfConnectOverlapped)
    {
        if (!GetSocket()->UpdateConnectContext())
        {
            LibCommons::Logger::GetInstance().LogError("OutboundSession", "OnIOCompleted: UpdateConnectContext failed. Session Id : {}, Error : {}", GetSessionId(), ::GetLastError());

            return;
        }

        LibCommons::Logger::GetInstance().LogInfo("SocketConnector", "OnIOCompleted: Connected to server. Session Id : {}", GetSessionId());

        OnConnected();
    }

}

} // namespace LibNetworks::Sessions