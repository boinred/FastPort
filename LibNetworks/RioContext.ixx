module;

#include <WinSock2.h>
#include <MSWSock.h>

export module networks.core.rio_context;

import std;

namespace LibNetworks::Core
{

/**
 * RIO 작업 유형
 */
export enum class RioOperationType
{
    Receive,
    Send
};

/**
 * RIO 비동기 작업의 컨텍스트를 저장하는 구조체
 * RIO 함수 호출 시 requestContext 인자로 전달됩니다.
 */
export struct RioContext
{
    RioOperationType OpType;
    void* pSession = nullptr; // 실제 세션 객체 포인터 (RIOSession 등)
    
    // 추가로 필요한 정보가 있다면 여기에 정의
};

} // namespace LibNetworks::Core
