module;

#include <memory>
export module networks.core.io_socket_connector;

import networks.core.io_consumer;

namespace LibNetworks::Core
{

class IOSocketConnector : public IIOConsumer, public std::enable_shared_from_this<IOSocketConnector>
{
public:

	void OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped) override;

};


} // namespace LibNetworks::Core