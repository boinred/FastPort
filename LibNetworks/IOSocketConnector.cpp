module;

#include <spdlog/spdlog.h>
#include <WinSock2.h>
#include <MSWSock.h>

module networks.core.io_socket_connector;

import std;
import commons.logger;

namespace LibNetworks::Core
{

std::shared_ptr<IOSocketConnector> IOSocketConnector::Create(const std::shared_ptr<Services::IOService>& pIOService, OnDoFuncCreateSession pOnDoFuncCreateSession, std::string ip, unsigned short port)
{
    auto pConnector = std::make_shared<IOSocketConnector>(pIOService, pOnDoFuncCreateSession);
    if (!pConnector->Connect(ip, port))
    {
        LibCommons::Logger::GetInstance().LogError("IOSocketConnector", "Create - Connect failed. {}:{}", ip, port);
        return nullptr;

    }

    return pConnector;
}



IOSocketConnector::IOSocketConnector(const std::shared_ptr<Services::IOService>& pIOService, OnDoFuncCreateSession pOnDoFuncCreateSession)
    : m_pIOService(pIOService), m_pOnDoFuncCreateSession(pOnDoFuncCreateSession)
{

}

void IOSocketConnector::DisConnect()
{
    if (!m_bConnected)
    {
        return;
    }

    m_Socket.Close();
    m_bConnected = false;

    LibCommons::Logger::GetInstance().LogInfo("IOSocketConnector", "IOSocketConnector DisConnected.");
}

//------------------------------------------------------------------------ 
void IOSocketConnector::OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped)
{
    if (!bSuccess)
    {
        DWORD dwError = ::GetLastError();

        LibCommons::Logger::GetInstance().LogError("SocketConnector", "OnIOCompleted: ConnectEx failed. Error : {}", dwError);
        m_Socket.Close();
        return;
    }

    m_bConnected = true;

    // TODO: 연결 성공 후 작업 처리
    for (int i = 0; i < 10; i++)
    {
        std::this_thread::sleep_for(std::chrono::seconds(60 * i));
    }

    delete pOverlapped;
}

//------------------------------------------------------------------------ 
bool IOSocketConnector::Connect(std::string ip, const unsigned short port)
{
    auto& logger = LibCommons::Logger::GetInstance();

    // 1. 소켓 생성
    m_Socket.CreateSocket();

    // 2. 주소 설정
    m_Socket.SetLocalAddress(0);

    // 3. 로컬 주소 바인드 (ConnectEx 요구사항)
    if (!m_Socket.Bind())
    {
        logger.LogError("SocketConnector", "Bind failed.");
        m_Socket.Close();

        return false;
    }

    // 4. IOCP에 소켓 연결
    if (!m_pIOService->Associate(m_Socket.GetSocket(), GetCompletionId()))
    {
        logger.LogError("SocketConnector", "Associate failed.");
        m_Socket.Close();

        return false;
    }

    // 5. ConnectEx 함수 포인터 가져오기
    GUID guidConnectEx = WSAID_CONNECTEX;
    DWORD dwBytes = 0;
    if (SOCKET_ERROR == ::WSAIoctl(m_Socket.GetSocket(), SIO_GET_EXTENSION_FUNCTION_POINTER, &guidConnectEx, sizeof(guidConnectEx), &m_lpfnConnectEx, sizeof(m_lpfnConnectEx), &dwBytes, nullptr, nullptr))
    {
        logger.LogError("SocketConnector", "Failed to get ConnectEx function. Error : {}", ::WSAGetLastError());
        m_Socket.Close();

        return false;
    }

    // 6. ConnectEx 호출
    if (!ConnectEx(ip, port))
    {
        logger.LogError("SocketConnector", "ConnectEx failed.");
        m_Socket.Close();

        return false;
    }

    return true;
}

bool IOSocketConnector::ConnectEx(std::string ip, const unsigned short port)
{
    auto& logger = LibCommons::Logger::GetInstance();

    if (m_lpfnConnectEx == nullptr)
    {
        logger.LogError("SocketConnector", "ConnectEx function pointer is null.");

        return false;
    }

    // 1. Remote 주소 설정
    sockaddr_in remoteAddr{};
    Socket::CreateSocketAddress(remoteAddr, ip, port);

    // 2. ConnectEx 호출
    OVERLAPPED* pOverlapped = new OVERLAPPED();

    bool bResult = m_lpfnConnectEx(
        m_Socket.GetSocket(),							// 소켓
        reinterpret_cast<const sockaddr*>(&remoteAddr),	// 서버 주소
        sizeof(remoteAddr),								// 주소 길이
        nullptr,										// 전송 버퍼
        0,												// 전송 버퍼 길이
        nullptr,										// 실제 전송된 바이트 수
        pOverlapped										// OVERLAPPED 구조체
    );

    if (!bResult)
    {
        int nError = ::WSAGetLastError();
        if (ERROR_IO_PENDING != nError)
        {
            logger.LogError("SocketConnector", "ConnectEx failed. Error : {}", nError);
            delete pOverlapped;
            return false;
        }
    }

    logger.LogInfo("SocketConnector", "Connecting to.");

    return true;
}



//------------------------------------ 

} // namespace LibNetworks::Core