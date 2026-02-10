#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <guiddef.h>
#include <spdlog/spdlog.h>

// WSAID_MULTIPLE_RIO_QUERY_EXTENSION_FUNCTIONS GUID 직접 정의 (0x8509e081, 0x96dd, 0x4005, 0xb1, 0xe3, 0xd2, 0xdd, 0x90, 0x71, 0x7f, 0x67)
#ifndef WSAID_MULTIPLE_RIO_QUERY_EXTENSION_FUNCTIONS
#define WSAID_MULTIPLE_RIO_QUERY_EXTENSION_FUNCTIONS {0x8509e081, 0x96dd, 0x4005, {0xb1, 0xe3, 0xd2, 0xdd, 0x90, 0x71, 0x7f, 0x67}}
#endif

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

    GUID rioGuid = WSAID_MULTIPLE_RIO_QUERY_EXTENSION_FUNCTIONS;
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
        LibCommons::Logger::GetInstance().LogError("RioExtension", "Initialize: WSAIoctl(SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER) failed. Error: {}", ::WSAGetLastError());

        return false;
    }

    m_bInitialized = true;
    return true;
}

} // namespace LibNetworks::Core