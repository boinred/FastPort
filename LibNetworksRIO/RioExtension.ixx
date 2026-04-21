#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

module;

#include <WinSock2.h>
#include <MSWSock.h>

export module networks.core.rio_extension;

import std;

namespace LibNetworks::Core
{

/**
 * RIO (Registered I/O) 확장 함수들을 로드하고 관리하는 클래스
 */
export class RioExtension
{
public:
    // 소켓을 통해 RIO 함수 포인터 테이블을 로드합니다.
    static bool Initialize(SOCKET socket);

    // 로드된 RIO 함수 테이블을 반환합니다.
    static const RIO_EXTENSION_FUNCTION_TABLE& GetTable() { return m_RioTable; }

    // 초기화 여부를 확인합니다.
    static bool IsInitialized() { return m_bInitialized; }

private:
    static RIO_EXTENSION_FUNCTION_TABLE m_RioTable;
    static bool m_bInitialized;
};

} // namespace LibNetworks::Core