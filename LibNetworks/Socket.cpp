module;

#include <WS2tcpip.h>

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


void Socket::Close()
{
    if (INVALID_SOCKET == m_Socket)
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


} // namespace LibNetworks::Core