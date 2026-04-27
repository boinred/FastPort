module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <memory>
#include <functional>
#include <mutex>

export module networks.core.io_socket_connector;

import std;
import networks.core.io_consumer;
import networks.core.socket;

import networks.sessions.inetwork_session;
import networks.services.inetwork_service;

namespace LibNetworks::Core
{

export class IOSocketConnector : public std::enable_shared_from_this<IOSocketConnector>
{
public:
    using OnDoFuncCreateSession = std::function<std::shared_ptr<Sessions::INetworkSession>(const std::shared_ptr<Core::Socket>&)>;

    static std::shared_ptr<IOSocketConnector> Create(
        const std::shared_ptr<Services::INetworkService>& pService,
        OnDoFuncCreateSession pOnDoFuncCreateSession,
        std::string ip, unsigned short port
    );

    IOSocketConnector() = delete;

    explicit IOSocketConnector(const std::shared_ptr<Services::INetworkService>& pService, OnDoFuncCreateSession pOnDoFuncCreateSession);

    void DisConnect();
    bool HasTrackedSession() const noexcept;

protected:
    bool Connect(std::string ip, const unsigned short port);

private:
    bool ConnectEx(std::string ip, const unsigned short port);
    void ReleaseSessionAfterActivation() noexcept;
    void ReleaseSessionAfterDisconnect() noexcept;

private:
    std::shared_ptr<Socket> m_pSocket = std::make_shared<Socket>();

    std::shared_ptr<Services::INetworkService> m_pService{};

    std::shared_ptr<Sessions::INetworkSession> m_pSession{};
    std::weak_ptr<Sessions::INetworkSession> m_wpSession{};
    std::shared_ptr<IOSocketConnector> m_pLifecycleHold{};

    LPFN_CONNECTEX m_lpfnConnectEx{};

    OnDoFuncCreateSession m_pOnDoFuncCreateSession = {};
    mutable std::mutex m_StateMutex{};
};

} // namespace LibNetworks::Core
