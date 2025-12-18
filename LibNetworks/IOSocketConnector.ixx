module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <memory>
#include <functional>


export module networks.core.io_socket_connector;

import std;
import networks.core.io_consumer;
import networks.core.socket; 

import networks.sessions.outbound_session;

import networks.services.io_service;


namespace LibNetworks::Core
{

export class IOSocketConnector : public IIOConsumer, public std::enable_shared_from_this<IOSocketConnector>
{
public:
    using OnDoFuncCreateSession = std::function<std::shared_ptr<Sessions::OutboundSession>(const std::shared_ptr<Core::Socket>&)>;

    static std::shared_ptr<IOSocketConnector> Create(
        const std::shared_ptr<Services::IOService>& pIOService,
        OnDoFuncCreateSession pOnDoFuncCreateSession,
        std::string ip, unsigned short port
    );

    IOSocketConnector() = delete;

    explicit IOSocketConnector(const std::shared_ptr<Services::IOService>& pIOService, OnDoFuncCreateSession pOnDoFuncCreateSession);

    void DisConnect();


protected:
    void OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped) override;

    bool Connect(std::string ip, const unsigned short port);

private:

    bool ConnectEx(std::string ip, const unsigned short port);

private:

    bool m_bConnected = false;

    Socket m_Socket = {};

    std::shared_ptr<Services::IOService> m_pIOService = {};

    LPFN_CONNECTEX m_lpfnConnectEx{};

    OnDoFuncCreateSession m_pOnDoFuncCreateSession = {};
};


} // namespace LibNetworks::Core