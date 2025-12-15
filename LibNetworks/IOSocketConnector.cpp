module;

module networks.core.io_socket_connector;

import std;

namespace LibNetworks::Core
{


void IOSocketConnector::OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped)
{
    throw std::logic_error("The method or operation is not implemented.");
}

} // namespace LibNetworks::Core