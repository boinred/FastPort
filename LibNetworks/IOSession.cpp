module;

#include <utility>

module networks.sessions.io_session;

namespace LibNetworks::Sessions
{

IOSession::IOSession(const std::shared_ptr<Core::Socket>& pSocket) : m_pSocket(std::move(pSocket))
{

}


} // namespace LibNetworks::Sessions