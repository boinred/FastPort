module;

#include <WinSock2.h>
#include <MSWSock.h>

#include <spdlog/spdlog.h>

module networks.core.io_socket_acceptor;

import std;
import commons.logger;
import networks.services.io_service;
import networks.core.io_consumer;

namespace LibNetworks::Core
{


std::shared_ptr<LibNetworks::Core::IOSocketAcceptor> IOSocketAcceptor::Create(Core::Socket& rfListenerSocket, OnDoFuncCreateSession pOnDoFuncCreateSession, const unsigned short listenPort, const unsigned long maxConnectionCount, const unsigned char threadCount, const unsigned char beginAcceptCount /*= 100*/)
{
    auto pAcceptor = std::make_shared<IOSocketAcceptor>(rfListenerSocket, pOnDoFuncCreateSession);
    if (!pAcceptor->Start(listenPort, maxConnectionCount, threadCount, beginAcceptCount))
    {
        LibCommons::Logger::GetInstance().LogError("IOSocketAcceptor", "Create - Start failed. Port : {}", listenPort);
        return nullptr;
    }
    return pAcceptor;

}


IOSocketAcceptor::IOSocketAcceptor(Core::Socket& rfListenerSocket, OnDoFuncCreateSession pOnDoFuncCreateSession)
    : m_ListenerSocket(std::move(rfListenerSocket)), m_pOnDoFuncCreateSession(pOnDoFuncCreateSession)
{

}

void IOSocketAcceptor::Shutdown()
{
    m_bExecuted = false;
    if (m_pService) m_pService->Stop();
    m_ListenerSocket.Close();
}

//------------------------------------------------------------------------ 
void IOSocketAcceptor::OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped)
{
    auto& logger = LibCommons::Logger::GetInstance();
    if (!pOverlapped)
    {
        logger.LogError("IOSocketAcceptor", "OnIOCompleted: OVERLAPPED is null. bSuccess : {}, BytesTransferred : {}", bSuccess, bytesTransferred);

        return;
    }

    // 1. AcceptOverlapped 구조체로 캐스팅
    AcceptOverlapped* pAcceptOverlapped = CONTAINING_RECORD(pOverlapped, AcceptOverlapped, Overlapped);
    if (!pAcceptOverlapped)
    {
        logger.LogError("IOSocketAcceptor", "OnIOCompleted: AcceptOverlapped is null. bSuccess : {}, BytesTransferred : {}", bSuccess, bytesTransferred);
        return;
    }

    if (!bSuccess)
    {
        logger.LogError("IOSocketAcceptor", "Accept failed");
        if (pAcceptOverlapped->AcceptSocket != INVALID_SOCKET)
        {
            ::closesocket(pAcceptOverlapped->AcceptSocket);
        }
    }
    else
    {
        logger.LogInfo("IOSocketAcceptor", "OnIOCompleted: Accept success. Socket : {}", pAcceptOverlapped->AcceptSocket);


        auto pSocket = std::make_shared<Socket>(pAcceptOverlapped->AcceptSocket);
        // AcceptContext 업데이트
        if (!pSocket->UpdateAcceptContext(m_ListenerSocket.GetSocket()))
        {
            logger.LogError("IOSocketAcceptor", "OnIOCompleted: UpdateAcceptContext failed. Error: {}", ::WSAGetLastError());
            pSocket->Close();
            return;
        }

        // 최적화 설정 적용
        // 1. Nagle Off (TCP_NODELAY)
        pSocket->UpdateContextDisableNagleAlgorithm();
        // 2. Zero-Copy (커널 버퍼 0)
        pSocket->UpdateContextZeroCopy();
        // 3. Keep-Alive (30초 유휴 후 1초 간격)
        pSocket->UpdateContextKeepAlive(30000, 1000);

        std::shared_ptr<Sessions::INetworkSession> pInboundSession = m_pOnDoFuncCreateSession(pSocket);
        
        // IOCP 모드인 경우 (Service가 IOService인 경우)에만 Associate 수행
        if (auto pIOService = std::dynamic_pointer_cast<LibNetworks::Services::IOService>(m_pService))
        {
            auto pIOConsumer = std::dynamic_pointer_cast<Core::IIOConsumer>(pInboundSession);
            if (pIOConsumer)
            {
                pIOService->Associate(pAcceptOverlapped->AcceptSocket, pIOConsumer->GetCompletionId());
            }
        }
        // RIO 모드인 경우 RIOSession 내부에서 소켓 처리가 이루어짐

        pInboundSession->OnAccepted();
    }

    if (m_bExecuted)
    {
        // 3. 다시 AcceptEx 요청
        if (!BeginAcceptEx())
        {
            logger.LogWarning("IOSocketAcceptor", "Failed to post next accept.");
        }
    }

    delete pAcceptOverlapped;
}

//------------------------------------------------------------------------ 

bool IOSocketAcceptor::Start(const unsigned short listenPort, const unsigned long maxConnectionCount, const unsigned char threadCount, const unsigned char beginAcceptCount)
{
    auto& logger = LibCommons::Logger::GetInstance();

    // 1. Service 시작
    if (m_pService)
    {
        if (!m_pService->Start(threadCount))
        {
            logger.LogError("IOSocketAcceptor", "Service Start failed");
            Shutdown();
            return false;
        }
    }
    else
    {
        auto pIOService = std::make_shared<Services::IOService>();
        if (!pIOService->Start(threadCount))
        {
            logger.LogError("IOSocketAcceptor", "Default IOService Start failed");
            return false;
        }
        m_pService = pIOService;
    }

    // 2. Listener를 위한 Socket 생성.
    if (!ListenSocket(m_ListenerSocketMode, listenPort, maxConnectionCount))
    {
        logger.LogError("IOSocketAcceptor", "ListenSocket is not valid.");

        Shutdown();

        return false;
    }

    m_bExecuted = true;

    // 3. 초기 AcceptEx 요청 시작
    for (int i = 0; i < beginAcceptCount; i++)
    {
        if (!BeginAcceptEx())
        {
            logger.LogWarning("IOSocketAcceptor", "Faile to post initial accept : {}", i);
        }

    }

    logger.LogInfo("IOSocketAcceptor", "Started listening on port {} with {} threads.", listenPort, threadCount);

    return true;
}

bool IOSocketAcceptor::ListenSocket(LibNetworks::Core::Socket::ENetworkMode listenSocketMode, const unsigned short listenPort, const unsigned long maxConnectionCount)
{
    auto& logger = LibCommons::Logger::GetInstance();

    // 1. 소켓 생성
    switch(listenSocketMode)
    {
    case LibNetworks::Core::Socket::ENetworkMode::IOCP:
        m_ListenerSocket.CreateSocket(LibNetworks::Core::Socket::ENetworkMode::IOCP);
        break;
    case LibNetworks::Core::Socket::ENetworkMode::RIO:
        m_ListenerSocket.CreateSocket(LibNetworks::Core::Socket::ENetworkMode::RIO);
        break;
    default:
        logger.LogError("IOSocketAcceptor", "ListenSocket: Invalid listen socket mode : {}", static_cast<int>(listenSocketMode));
        return false;
    }
    m_ListenerSocketMode = listenSocketMode;
    

    // 2. 주소 설정 (모든 인터페이스)
    m_ListenerSocket.SetLocalAddress(listenPort);

    // 2-1. SO_REUSEADDR 옵션 설정 (포트 재사용)
    if (!m_ListenerSocket.UpdateContextReuseAddr(true))
    {
        logger.LogWarning("IOSocketAcceptor", "SetReuseAddr failed. Error: {}", ::WSAGetLastError());
    }

    // 3.소켓 바인딩
    if (!m_ListenerSocket.Bind())
    {
        logger.LogError("IOSocketAcceptor", "Socket Bind failed.");

        return false;
    }

    // 4. IOCP에 소켓 연결 (Acceptor 자체는 항상 IOCP 필요)
    auto pIOService = std::dynamic_pointer_cast<LibNetworks::Services::IOService>(m_pService);
    if (!pIOService || !pIOService->Associate(m_ListenerSocket.GetSocket(), GetCompletionId()))
    {
        logger.LogError("IOSocketAcceptor", "failed to associate listen socket. (Requires IOService)");
        Shutdown();
        return false;
    }


    // 5. 리스닝 시작
    if (!m_ListenerSocket.Listen(maxConnectionCount))
    {
        logger.LogError("IOSocketAcceptor", "Socket Listen failed.");

        return false;
    }

    // 6. AcceptEx 함수 포인터 가지고 오기.
    GUID guidAcceptEx = WSAID_ACCEPTEX;
    DWORD dwBytes = 0;
    if (SOCKET_ERROR == ::WSAIoctl(m_ListenerSocket.GetSocket(), SIO_GET_EXTENSION_FUNCTION_POINTER, &guidAcceptEx, sizeof(guidAcceptEx), &m_lpfnAcceptEx, sizeof(m_lpfnAcceptEx), &dwBytes, nullptr, nullptr))
    {
        logger.LogError("IOSocketAcceptor", "Failed to get AcceptEx function. Error: {}", ::WSAGetLastError());

        Shutdown();

        return false;
    }

    // 7. GetAcceptExSockaddrs 함수 포인터 가지고 오기.
    GUID guidGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
    if (SOCKET_ERROR == ::WSAIoctl(m_ListenerSocket.GetSocket(), SIO_GET_EXTENSION_FUNCTION_POINTER, &guidGetAcceptExSockaddrs, sizeof(guidGetAcceptExSockaddrs), &m_lpfnGetAcceptExSockaddrs, sizeof(m_lpfnGetAcceptExSockaddrs), &dwBytes, nullptr, nullptr))
    {
        logger.LogError("IOSocketAcceptor", "Failed to get GetAcceptExSockaddrs function. Error : {}", ::WSAGetLastError());
        Shutdown();

        return false;
    }

    return true;
}

bool IOSocketAcceptor::BeginAcceptEx()
{
    auto& logger = LibCommons::Logger::GetInstance();
    if (!m_bExecuted)
    {
        logger.LogError("IOSocketAcceptor", "BeginAcceptEx: Listener not started");
        return false;
    }

    // 1. AcceptEx용 OVERLAPPED 확장 구조체 생성
    AcceptOverlapped* pAcceptOverlapped = new AcceptOverlapped();

    // 2. 클라이언트용 소켓 생성
    pAcceptOverlapped->AcceptSocket = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCKET == pAcceptOverlapped->AcceptSocket)
    {
        logger.LogError("IOSocketAcceptor", "BeginAcceptEx: Failed to create accept socket. Error : {}", ::WSAGetLastError());
        delete pAcceptOverlapped;
        return false;
    }

    // 3.AcceptEx 호출
    DWORD bytesReceived = 0;
    bool bResult = m_lpfnAcceptEx(
        m_ListenerSocket.GetSocket(),			// Listener Socket
        pAcceptOverlapped->AcceptSocket,		// Accept 될 소켓
        pAcceptOverlapped->Buffer,				// 수신 버퍼 (로컬 + 원격 주소)
        0,										// 수신 데이터 데이터를 받을 버퍼 크기 (0이면 연결만 대기)
        sizeof(sockaddr_in) + 16,				// 로컬 주소 길이
        sizeof(sockaddr_in) + 16,				// 원격 주소 길이
        &bytesReceived,							// 실제 수신된 바이트 수
        &(pAcceptOverlapped->Overlapped)		// OVERLAPPED 구조체
    );

    if (!bResult)
    {
        int nError = ::WSAGetLastError();
        if (ERROR_IO_PENDING != nError)
        {
            logger.LogError("IOSocketAcceptor", "BeginAcceptEx: AcceptEx failed. Error : {}", nError);

            ::closesocket(pAcceptOverlapped->AcceptSocket);
            delete pAcceptOverlapped;
            return false;
        }
    }

    logger.LogDebug("IOSocketAcceptor", "AcceptEx posted successfully.");

    return true;
}


} // namespace LibNetworks