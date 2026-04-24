module;

#include <spdlog/spdlog.h>
#include <WinSock2.h>
#include <MSWSock.h>

module networks.core.io_socket_connector;

import std;
import commons.logger;
import networks.services.io_service;
import networks.core.io_consumer;
import networks.sessions.outbound_session;

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

bool IOSocketConnector::HasTrackedSession() const noexcept
{
    std::lock_guard<std::mutex> lock(m_StateMutex);
    return static_cast<bool>(m_pSession) || !m_wpSession.expired();
}

void IOSocketConnector::ReleaseSessionAfterActivation() noexcept
{
    std::lock_guard<std::mutex> lock(m_StateMutex);
    if (m_pSession)
    {
        m_wpSession = m_pSession;
        m_pSession.reset();
    }
    m_pLifecycleHold.reset();
}

void IOSocketConnector::ReleaseSessionAfterDisconnect() noexcept
{
    std::lock_guard<std::mutex> lock(m_StateMutex);
    m_pSession.reset();
    m_wpSession.reset();
    m_pLifecycleHold.reset();
}

void IOSocketConnector::DisConnect()
{
    std::shared_ptr<Sessions::INetworkSession> pSession;
    std::shared_ptr<Socket> pSocket;

    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (!m_pSession && m_wpSession.expired())
        {
            return;
        }

        pSession = m_pSession ? m_pSession : m_wpSession.lock();
        pSocket = m_pSocket;
    }

    if (auto pOutboundSession = std::dynamic_pointer_cast<Sessions::OutboundSession>(pSession))
    {
        pOutboundSession->RequestDisconnect(Sessions::DisconnectReason::Normal);
        return;
    }

    if (pSocket)
    {
        pSocket->Close();
    }

    ReleaseSessionAfterDisconnect();

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
    if (!m_pSession)
    {
        logger.LogError("SocketConnector", "CreateSession failed.");
        m_pSocket->Close();
        return false;
    }

    m_wpSession = m_pSession;
    m_pLifecycleHold = shared_from_this();
    auto pOutboundSession = std::dynamic_pointer_cast<Sessions::OutboundSession>(m_pSession);
    if (!pOutboundSession)
    {
        logger.LogError("SocketConnector", "Connect requires OutboundSession-compatible session.");
        DisConnect();
        return false;
    }

    const auto weakSelf = weak_from_this();
    pOutboundSession->SetActivationObserver([weakSelf]()
    {
        if (auto self = weakSelf.lock())
        {
            self->ReleaseSessionAfterActivation();
        }
    });
    pOutboundSession->SetDisconnectObserver([weakSelf]()
    {
        if (auto self = weakSelf.lock())
        {
            self->ReleaseSessionAfterDisconnect();
        }
    });

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

    auto pOutboundSession = std::dynamic_pointer_cast<Sessions::OutboundSession>(m_pSession);
    if (!pOutboundSession)
    {
        logger.LogError("SocketConnector", "ConnectEx requires OutboundSession-compatible session.");
        return false;
    }

    // 1. Remote 주소 설정
    sockaddr_in remoteAddr{};
    Socket::CreateSocketAddress(remoteAddr, ip, port);

    pOutboundSession->MarkConnectIoPosted();

    // 2. ConnectEx 호출
    bool bResult = m_lpfnConnectEx(
        m_pSocket->GetSocket(),                         // 소켓
        reinterpret_cast<const sockaddr*>(&remoteAddr), // 서버 주소
        sizeof(remoteAddr),                             // 주소 길이
        nullptr,                                        // 전송 버퍼
        0,                                              // 전송 버퍼 길이
        nullptr,                                        // 실제 전송된 바이트 수
        pOutboundSession->GetConnectOverlappedPtr()     // OVERLAPPED 구조체
    );

    if (!bResult)
    {
        int nError = ::WSAGetLastError();
        if (ERROR_IO_PENDING != nError)
        {
            pOutboundSession->UndoConnectIoOnPostFailure();
            logger.LogError("SocketConnector", "ConnectEx failed. Error : {}", nError);
            return false;
        }
    }

    logger.LogInfo("SocketConnector", "Connecting to. IP : {}, Port : {}", ip, port);

    return true;
}

//------------------------------------

} // namespace LibNetworks::Core
