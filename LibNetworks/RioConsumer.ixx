module;

#include <Windows.h>

export module networks.core.rio_consumer;

import networks.core.rio_context;

namespace LibNetworks::Core
{

/**
 * RIO 작업 완료 통지를 받는 인터페이스
 */
export class IRioConsumer
{
public:
    virtual ~IRioConsumer() = default;

    // RIO 작업 완료 시 호출됩니다.
    virtual void OnIOCompleted(bool bSuccess, DWORD bytesTransferred, RioOperationType opType) = 0;
};

} // namespace LibNetworks::Core
