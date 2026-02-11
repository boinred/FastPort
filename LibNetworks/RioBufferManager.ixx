module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <cstdint>
#include <condition_variable>


export module networks.core.rio_buffer_manager;

import std;
import networks.core.rio_extension;

namespace LibNetworks::Core
{

/**
 * RIO에서 사용하는 버퍼 슬라이스 정보
 */
export struct RioBufferSlice
{
    RIO_BUFFERID BufferId = RIO_INVALID_BUFFERID;
    ULONG Offset = 0;
    ULONG Length = 0;
    void* pData = nullptr;
};

/**
 * RIO 전용 등록 버퍼 관리자
 */
export class RioBufferManager
{
public:
    RioBufferManager() = default;
    ~RioBufferManager();

    // 대용량 메모리 청크를 할당하고 RIO에 등록합니다.
    bool Initialize(uint32_t totalSize);

    // 등록된 버퍼에서 슬라이스를 할당받습니다.
    bool AllocateSlice(uint32_t size, RioBufferSlice& outSlice);

    // 리소스 해제
    void Finalize();

private:
    RIO_BUFFERID m_BufferId = RIO_INVALID_BUFFERID;
    void* m_pBuffer = nullptr;
    uint32_t m_TotalSize = 0;
    uint32_t m_CurrentOffset = 0;
    std::mutex m_Mutex;
};

} // namespace LibNetworks::Core
