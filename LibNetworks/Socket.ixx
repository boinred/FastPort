module;

#include <WinSock2.h>
#include <MSWSock.h>

export module networks.core.socket;

import std;


namespace LibNetworks::Core
{


export class Socket
{
public:

    // Winsock 라이브러리 초기화
    static void Initialize();
    // IP 문자열과 포트 번호로 sockaddr_in 구조체 생성
    static bool CreateSocketAddress(sockaddr_in& rfSockAddr, const std::string ip, const unsigned short port);
public:
    // 기본 생성자
    Socket() = default;
    // 소멸자
    ~Socket() = default;

    // 기존 SOCKET 핸들 소유권 이동 생성
    Socket(SOCKET& rfSocket) : m_Socket(std::move(rfSocket)) {}

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    // 이동 생성자
    Socket(Socket&&) noexcept = default;

    // 이동 대입 연산자
    Socket& operator=(Socket&&) noexcept;
    // 소켓 유효성 확인
    explicit operator bool() const;

    // TCP 소켓 생성
    void CreateSocket();

    // 소켓 닫기
    void Close();

    // 로컬 주소 정보 설정
    void SetLocalAddress(const unsigned short port);

    // 원격 주소 정보 설정
    void SetRemoteAddress(const std::string ip, const unsigned short port);

    // 소켓 주소 바인딩
    bool Bind();

    // 연결 수신 대기 상태 전환
    bool Listen(unsigned int maxConnectionCount);

    // 소켓 주소 구조체 반환
    const sockaddr_in& GetAddress() const { return m_SockAddr; }
    sockaddr_in& GetAddress()
    {
        const Socket& rfThis = *this;
        return const_cast<sockaddr_in&>(rfThis.GetAddress());
    }


    // 내부 SOCKET 핸들 반환
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


} // namespace LibNetworks::Core