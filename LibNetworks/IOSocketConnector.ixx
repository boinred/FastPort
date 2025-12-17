module;

#include <memory>
#include <functional>

export module networks.core.io_socket_connector;

import networks.core.io_consumer;
import networks.core.socket; 

import networks.sessions.outbound_session;


namespace LibNetworks::Core
{

class IOSocketConnector : public IIOConsumer, public std::enable_shared_from_this<IOSocketConnector>
{
public:
    using OnDoFuncCreateSession = std::function<std::shared_ptr<Sessions::OutboundSession>(const std::shared_ptr<Core::Socket>&)>;

    IOSocketConnector() = delete;

    void OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped) override;

};


} // namespace LibNetworks::Core