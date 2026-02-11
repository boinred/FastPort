#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <guiddef.h>
#include <spdlog/spdlog.h>

module networks.core.rio_extension;


import std;
import commons.logger;

namespace LibNetworks::Core
{

RIO_EXTENSION_FUNCTION_TABLE RioExtension::m_RioTable = {};
bool RioExtension::m_bInitialized = false;

bool RioExtension::Initialize(SOCKET socket)
{
    if (m_bInitialized)
    {
        return true;
    }

    if (socket == INVALID_SOCKET)
    {
        LibCommons::Logger::GetInstance().LogError("RioExtension", "Initialize: Invalid socket provided.");
        return false;
    }


    GUID rioGuid = WSAID_MULTIPLE_RIO;
    DWORD dwBytes = 0;

    int result = ::WSAIoctl(
        socket,
        SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER,
        &rioGuid,
        sizeof(rioGuid),
        &m_RioTable,
        sizeof(m_RioTable),
        &dwBytes,
        nullptr,
        nullptr
    );

    if (result == SOCKET_ERROR)
    {
        int errorCode = ::WSAGetLastError();
        LibCommons::Logger::GetInstance().LogError("RioExtension", "Initialize: WSAIoctl(SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER) failed. Error: {}", errorCode);

        return false;
    }

    m_bInitialized = true;
    return true;
}

} // namespace LibNetworks::Core