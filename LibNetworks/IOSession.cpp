module;

#include <utility>

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

void IOSession::RequestReceived()
{
    
}

} // namespace LibNetworks::Sessions