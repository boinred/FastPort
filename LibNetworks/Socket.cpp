module;

#include <WS2tcpip.h>
#include <MSWSock.h>
#include <mstcpip.h>
//  message : IFC 가져오기가 감지되었습니다. 발생함으로 추가.
#include <spdlog/spdlog.h>
#pragma comment(lib, "ws2_32.lib")

module networks.core.socket;

import commons.logger;

namespace LibNetworks::Core
{


void Socket::Initialize()
{
    auto& logger = LibCommons::Logger::GetInstance();

    WSADATA wsaData{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsaData) == 0)
    {
        logger.LogInfo("Socket", "Initialize, Socket Initialized.");
    }
    else
    {
        logger.LogError("Socket", "Initialize, Socket Initialize failed.");
    }
}


bool Socket::CreateSocketAddress(sockaddr_in& rfSockAddr, const std::string ip, const unsigned short port)
{
    std::memset(&rfSockAddr, 0, sizeof(sockaddr_in));

    rfSockAddr.sin_family = AF_INET;
    rfSockAddr.sin_port = port == 0 ? 0 : htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &rfSockAddr.sin_addr) <= 0)
    {
        LibCommons::Logger::GetInstance().LogError("Socket", "CreateSocketAddress, Invalid IP Address : {}", ip);

        return false;
    }
    return true;
}

Socket::operator bool() const
{
    return m_Socket != INVALID_SOCKET;
}

Socket& Socket::operator=(Socket&& rhs) noexcept
{
    return *this;
}


void Socket::CreateSocket()
{
    m_Socket = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCKET == m_Socket)
    {
        LibCommons::Logger::GetInstance().LogError("Socket", "Socket Create failed. Last Error : {}", ::GetLastError());

        Close();

        return;
    }

    LibCommons::Logger::GetInstance().LogInfo("Socket", "Socket Created.");
}

void Socket::Shutdown(int how)
{
    if (m_Socket == INVALID_SOCKET)
    {
        return;
    }

    ::shutdown(m_Socket, how);
}

void Socket::Close()
{
    if (m_Socket == INVALID_SOCKET)
    {
        return;
    }

    ::closesocket(m_Socket);
    m_Socket = INVALID_SOCKET;
}

void Socket::SetLocalAddress(const unsigned short port)
{
    m_SockAddr.sin_family = AF_INET;
    m_SockAddr.sin_port = port == 0 ? 0 : htons(port);
    m_SockAddr.sin_addr.s_addr = INADDR_ANY;
}

void Socket::SetRemoteAddress(const std::string ip, const unsigned short port)
{
    CreateSocketAddress(m_SockAddr, ip, port);
}

bool Socket::Bind()
{
    if (SOCKET_ERROR == ::bind(m_Socket, reinterpret_cast<const sockaddr*>(&GetAddress()), sizeof(sockaddr_in)))
    {
        LibCommons::Logger::GetInstance().LogError("SocketListener", "Bind failed. Error: {}", ::WSAGetLastError());

        Close();

        return false;
    }

    return true;
}

bool Socket::Listen(unsigned int maxConnectionCount)
{
    if (SOCKET_ERROR == ::listen(m_Socket, static_cast<int>(maxConnectionCount)))
    {
        LibCommons::Logger::GetInstance().LogError("SocketListener", "Listen failed. Error: {}", ::WSAGetLastError());

        Close();

        return false;
    }
    return true;
}

bool Socket::UpdateConnectContext() const
{
    if (SOCKET_ERROR == ::setsockopt(m_Socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0))
    {
        LibCommons::Logger::GetInstance().LogError("Socket", "OnIOCompleted: setsockopt(SO_UPDATE_CONNECT_CONTEXT) failed. Error: {}", ::WSAGetLastError());

        return false;
    }

    return true;
}

bool Socket::UpdateContextDisableNagleAlgorithm() const
{
    // Nagle 알고리즘 비활성화 (TCP_NODELAY 설정)
    BOOL bNoDelay = TRUE; // 켜기 (Nagle 끄기)
    int nRet = ::setsockopt(
        m_Socket,                       // 옵션을 적용할 소켓
        IPPROTO_TCP,                    // 프로토콜 레벨 (TCP)
        TCP_NODELAY,                    // 옵션 이름
        (const char*)&bNoDelay,         // 옵션 값 (Windows에서는 char* 캐스팅 필요)
        sizeof(bNoDelay)                // 옵션 값의 크기
    );

    if (SOCKET_ERROR == nRet)
    {
        LibCommons::Logger::GetInstance().LogError("Socket", "OnIOCompleted: setsockopt(TCP_NODELAY) failed. Error: {}", ::WSAGetLastError());

        return false;
    }

    return true;
}

bool Socket::UpdateContextZeroCopy() const
{
    int zero = 0;
    // 수신 버퍼 0 (Zero-copy Recv)
    if (SOCKET_ERROR == ::setsockopt(m_Socket, SOL_SOCKET, SO_RCVBUF, (const char*)&zero, sizeof(zero)))
    {
        LibCommons::Logger::GetInstance().LogError("Socket", "SetZeroCopyConfig: setsockopt(SO_RCVBUF) failed. Error: {}", ::WSAGetLastError());
        return false;
    }

    // 송신 버퍼 0 (Zero-copy Send)
    if (SOCKET_ERROR == ::setsockopt(m_Socket, SOL_SOCKET, SO_SNDBUF, (const char*)&zero, sizeof(zero)))
    {
        LibCommons::Logger::GetInstance().LogError("Socket", "SetZeroCopyConfig: setsockopt(SO_SNDBUF) failed. Error: {}", ::WSAGetLastError());
        return false;
    }

    return true;
}

bool Socket::UpdateContextKeepAlive(unsigned long idleMs, unsigned long intervalMs) const
{
    BOOL on = TRUE;
    if (SOCKET_ERROR == ::setsockopt(m_Socket, SOL_SOCKET, SO_KEEPALIVE, (const char*)&on, sizeof(on)))
    {
        LibCommons::Logger::GetInstance().LogError("Socket", "SetKeepAliveConfig: setsockopt(SO_KEEPALIVE) failed. Error: {}", ::WSAGetLastError());
        return false;
    }

    tcp_keepalive alive;
    alive.onoff = 1;
    alive.keepalivetime = idleMs;        // 유휴 시간 후 첫 탐색 시작
    alive.keepaliveinterval = intervalMs; // 응답 없을 시 재시도 간격

    DWORD dwBytes = 0;
    if (SOCKET_ERROR == ::WSAIoctl(m_Socket, SIO_KEEPALIVE_VALS, &alive, sizeof(alive), nullptr, 0, &dwBytes, nullptr, nullptr))
    {
        LibCommons::Logger::GetInstance().LogError("Socket", "SetKeepAliveConfig: WSAIoctl(SIO_KEEPALIVE_VALS) failed. Error: {}", ::WSAGetLastError());
        return false;
    }
    return true;
}

bool Socket::UpdateContextLingerConfig(bool onOff, unsigned short lingerTime) const
{
    linger ling;
    ling.l_onoff = onOff ? 1 : 0;
    ling.l_linger = lingerTime;

    if (SOCKET_ERROR == ::setsockopt(m_Socket, SOL_SOCKET, SO_LINGER, (const char*)&ling, sizeof(ling)))
    {
        LibCommons::Logger::GetInstance().LogError("Socket", "SetLingerConfig: setsockopt(SO_LINGER) failed. Error: {}", ::WSAGetLastError());
        return false;
    }
    return true;
}

bool Socket::UpdateContextReuseAddr(bool bReuse) const
{
    BOOL val = bReuse ? TRUE : FALSE;
    if (SOCKET_ERROR == ::setsockopt(m_Socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&val, sizeof(val)))
    {
        LibCommons::Logger::GetInstance().LogError("Socket", "SetReuseAddr: setsockopt(SO_REUSEADDR) failed. Error: {}", ::WSAGetLastError());
        return false;
    }
    return true;
}

bool Socket::UpdateAcceptContext(SOCKET listenSocket) const
{
    if (SOCKET_ERROR == ::setsockopt(m_Socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<char*>(&listenSocket), sizeof(SOCKET)))
    {
        LibCommons::Logger::GetInstance().LogError("Socket", "UpdateAcceptContext: setsockopt SO_UPDATE_ACCEPT_CONTEXT failed. Error: {}", ::WSAGetLastError());
        return false;
    }
    return true;
}

bool Socket::Disconnect(OVERLAPPED* pOverlapped, DWORD dwFlags)
{
    static LPFN_DISCONNECTEX lpfnDisconnectEx = nullptr;

    if (lpfnDisconnectEx == nullptr)
    {
        GUID guidDisconnectEx = WSAID_DISCONNECTEX;
        DWORD dwBytes = 0;
        if (SOCKET_ERROR == ::WSAIoctl(m_Socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
            &guidDisconnectEx, sizeof(guidDisconnectEx),
            &lpfnDisconnectEx, sizeof(lpfnDisconnectEx),
            &dwBytes, nullptr, nullptr))
        {
            LibCommons::Logger::GetInstance().LogError("Socket", "Disconnect: WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER) failed. Error: {}", ::WSAGetLastError());
            return false;
        }
    }

    if (FALSE == lpfnDisconnectEx(m_Socket, pOverlapped, dwFlags, 0))
    {
        int error = ::WSAGetLastError();
        if (error != ERROR_IO_PENDING)
        {
            LibCommons::Logger::GetInstance().LogError("Socket", "Disconnect: DisconnectEx failed. Error: {}", error);
            return false;
        }
    }

    return true;
}

} // namespace LibNetworks::Core