module;

#include <spdlog/spdlog.h>
#include <WinSock2.h>
#include <MSWSock.h>

module networks.core.io_socket_connector;

import std;
import commons.logger;
import networks.services.io_service;
import networks.core.io_consumer;


namespace LibNetworks::Core
{

static bool UpdateSocketReuseAddr(SOCKET socket)
{
    // SO_REUSEADDR 설정 (서버 리스닝 소켓 필수)
    BOOL bReuse = TRUE;
    int nRet = ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&bReuse, sizeof(bReuse));
    if (nRet == SOCKET_ERROR)
    {
        // 에러 로깅
        LibCommons::Logger::GetInstance().LogError("IOSocketConnector", "UpdateSocketReuseAddr failed. Error : {}", ::WSAGetLastError());
    }
    return nRet == 0;
}

std::shared_ptr<IOSocketConnector> IOSocketConnector::Create(const std::shared_ptr<Services::INetworkService>& pService, OnDoFuncCreateSession pOnDoFuncCreateSession, std::string ip, unsigned short port)
{
    auto pConnector = std::make_shared<IOSocketConnector>(pService, pOnDoFuncCreateSession);
    if (!pConnector->Connect(ip, port))
    {
        LibCommons::Logger::GetInstance().LogError("IOSocketConnector", "Create - Connect failed. {}:{}", ip, port);
        return nullptr;
    }

    return pConnector;
}



IOSocketConnector::IOSocketConnector(const std::shared_ptr<Services::INetworkService>& pService, OnDoFuncCreateSession pOnDoFuncCreateSession)
    : m_pService(pService), m_pOnDoFuncCreateSession(pOnDoFuncCreateSession)
{

}

void IOSocketConnector::DisConnect()
{
    if (!m_bConnected)
    {
        return;
    }

    m_pSocket->Close();
    m_bConnected = false;
    m_pSession = {};

    LibCommons::Logger::GetInstance().LogInfo("IOSocketConnector", "IOSocketConnector DisConnected.");
}

//------------------------------------------------------------------------ 

//------------------------------------------------------------------------ 
bool IOSocketConnector::Connect(std::string ip, const unsigned short port)
{
    auto& logger = LibCommons::Logger::GetInstance();

    // 1. 소켓 생성
    m_pSocket->CreateSocket(LibNetworks::Core::Socket::ENetworkMode::IOCP);

    // 2. 주소 설정
    m_pSocket->SetLocalAddress(0);

    // 3. 로컬 주소 바인드 (ConnectEx 요구사항)
    if (!m_pSocket->Bind())
    {
        logger.LogError("SocketConnector", "Bind failed.");
        DisConnect();

        return false;
    }

    m_pSession = m_pOnDoFuncCreateSession(m_pSocket);

    // 4. IOCP에 소켓 연결 (IOService인 경우에만)
    if (auto pIOService = std::dynamic_pointer_cast<LibNetworks::Services::IOService>(m_pService))
    {
        auto pIOConsumer = std::dynamic_pointer_cast<Core::IIOConsumer>(m_pSession);
        if (pIOConsumer)
        {
            if (!pIOService->Associate(m_pSocket->GetSocket(), pIOConsumer->GetCompletionId()))
            {
                logger.LogError("SocketConnector", "Connect, Associate failed. OutboundSession Id : {}", m_pSession->GetSessionId());
                DisConnect();
                return false;
            }
            LibCommons::Logger::GetInstance().LogInfo("SocketConnector", "Connect, IOSocketConnector Associate. OutboundSession Id : {}", m_pSession->GetSessionId());
        }
    }

    // 5. ConnectEx 함수 포인터 가져오기
    GUID guidConnectEx = WSAID_CONNECTEX;
    DWORD dwBytes = 0;
    if (SOCKET_ERROR == ::WSAIoctl(m_pSocket->GetSocket(), SIO_GET_EXTENSION_FUNCTION_POINTER, &guidConnectEx, sizeof(guidConnectEx), &m_lpfnConnectEx, sizeof(m_lpfnConnectEx), &dwBytes, nullptr, nullptr))
    {
        logger.LogError("SocketConnector", "Failed to get ConnectEx function. Error : {}", ::WSAGetLastError());
        DisConnect();

        return false;
    }

    // 6. ConnectEx 호출
    if (!ConnectEx(ip, port))
    {
        logger.LogError("SocketConnector", "ConnectEx failed.");
        DisConnect();

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
    bool bResult = m_lpfnConnectEx(
        m_pSocket->GetSocket(),							// 소켓
        reinterpret_cast<const sockaddr*>(&remoteAddr),	// 서버 주소
        sizeof(remoteAddr),								// 주소 길이
        nullptr,										// 전송 버퍼
        0,												// 전송 버퍼 길이
        nullptr,										// 실제 전송된 바이트 수
        m_pSession->GetConnectOverlappedPtr()			// OVERLAPPED 구조체
    );

    if (!bResult)
    {
        int nError = ::WSAGetLastError();
        if (ERROR_IO_PENDING != nError)
        {
            logger.LogError("SocketConnector", "ConnectEx failed. Error : {}", nError);
            return false;
        }
    }

    logger.LogInfo("SocketConnector", "Connecting to. IP : {}, Port : {}", ip, port);

    return true;
}



//------------------------------------ 

} // namespace LibNetworks::Core