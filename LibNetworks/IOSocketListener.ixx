module;

#include <memory>

export module networks.core.io_socket_listener;

import networks.core.io_consumer;

namespace LibNetworks::Core
{
export class IOSocketListener : public Core::IIOConsumer, std::enable_shared_from_this<IOSocketListener>
{
public:
    void OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped) override;

};

} // namespace LibNetworks