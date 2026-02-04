module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <memory>
#include <functional>

export module networks.core.io_socket_listener;

import networks.core.io_consumer;
import networks.core.socket;

import networks.sessions.inbound_session;

import networks.services.io_service;

namespace LibNetworks::Core
{
    // TODO : IOSocketAcceptor로 변경 할 것!
export class IOSocketListener : public Core::IIOConsumer, std::enable_shared_from_this<IOSocketListener>
{
    // AcceptEx용 OVERLAPPED 확장 구조체
    struct AcceptOverlapped
    {
        OVERLAPPED Overlapped = {};
        SOCKET AcceptSocket = {};
        char Buffer[(sizeof(sockaddr_in) + 16) * 2] = {};  // 로컬 주소 + 원격 주소
    };
public:

    using OnDoFuncCreateSession = std::function<std::shared_ptr<Sessions::InboundSession>(const std::shared_ptr<Core::Socket>&)>;

    static std::shared_ptr<IOSocketListener> Create(Core::Socket& rfListenerSocket,
        OnDoFuncCreateSession pOnDoFuncCreateSession,
        const unsigned short listenPort,
        const unsigned long maxConnectionCount,
        const unsigned char threadCount,
        const unsigned char beginAcceptCount = 100);

    IOSocketListener() = delete;

    explicit IOSocketListener(Core::Socket& rfListenerSocket, OnDoFuncCreateSession pOnDoFuncCreateSession);

    void Shutdown();

protected:
    void OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped) override;
private:
    bool Start(const unsigned short listenPort, const unsigned long maxConnectionCount, const unsigned char threadCount, const unsigned char beginAcceptCount);

    bool ListenSocket(const unsigned short listenPort, const unsigned long maxConnectionCount);
    bool BeginAcceptEx();

private:
    Core::Socket m_ListenerSocket = {};
    std::shared_ptr<Services::IOService> m_pIOService = std::make_shared<Services::IOService>();

    bool m_bExecuted = {};

    LPFN_ACCEPTEX m_lpfnAcceptEx = {};
    LPFN_GETACCEPTEXSOCKADDRS m_lpfnGetAcceptExSockaddrs = {};

    OnDoFuncCreateSession m_pOnDoFuncCreateSession = {};

};

} // namespace LibNetworks