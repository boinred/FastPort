module;

#include <WinSock2.h>
#include <MSWSock.h>

export module networks.socket;

import std;


namespace LibNetworks
{
class Socket
{
public:

    static void Initialize();
    static bool CreateSocketAddress(sockaddr_in& rfSockAddr, const std::string ip, const unsigned short port);
public:
    Socket() = default;
    ~Socket() = default;

    Socket(SOCKET& rfSocket) : m_Socket(std::move(rfSocket)) {}

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&&) noexcept = default;

    Socket& operator=(Socket&&) noexcept;
    explicit operator bool() const;

    void CreateSocket();

    void Close();

    void SetLocalAddress(const unsigned short port);
    void SetRemoteAddress(const std::string ip, const unsigned short port);


    bool Bind();

    bool Listen(unsigned int maxConnectionCount);

    const sockaddr_in& GetAddress() const { return m_SockAddr; }
    sockaddr_in& GetAddress()
    {
        const Socket& rfThis = *this;
        return const_cast<sockaddr_in&>(rfThis.GetAddress());
    }


    const SOCKET& GetSocket() const { return m_Socket; }
    SOCKET& GetSocket()
    {
        const Socket& rfThis = *this;
        return const_cast<SOCKET&>(rfThis.GetSocket());
    }
private:
    SOCKET m_Socket = INVALID_SOCKET;
    sockaddr_in m_SockAddr = {};
};


} // namespace LibNetworks