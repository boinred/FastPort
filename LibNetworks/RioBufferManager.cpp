module;

#include <WinSock2.h>
#include <MSWSock.h>

module networks.core.rio_buffer_manager;

import std;
import networks.core.rio_extension;

namespace LibNetworks::Core
{

RioBufferManager::~RioBufferManager()
{
    Finalize();
}

bool RioBufferManager::Initialize(uint32_t totalSize)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (m_pBuffer != nullptr)
    {
        return true;
    }

    // RIO 버퍼는 페이지 단위로 정렬되어야 하며, 가상 메모리 할당이 권장됩니다.
    m_pBuffer = ::VirtualAlloc(nullptr, totalSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (m_pBuffer == nullptr)
    {
        return false;
    }

    m_BufferId = RioExtension::GetTable().RIORegisterBuffer(reinterpret_cast<PCHAR>(m_pBuffer), totalSize);
    if (m_BufferId == RIO_INVALID_BUFFERID)
    {
        ::VirtualFree(m_pBuffer, 0, MEM_RELEASE);
        m_pBuffer = nullptr;
        return false;
    }

    m_TotalSize = totalSize;
    m_CurrentOffset = 0;

    return true;
}

bool RioBufferManager::AllocateSlice(uint32_t size, RioBufferSlice& outSlice)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (m_CurrentOffset + size > m_TotalSize)
    {
        return false;
    }

    outSlice.BufferId = m_BufferId;
    outSlice.Offset = m_CurrentOffset;
    outSlice.Length = size;
    outSlice.pData = reinterpret_cast<char*>(m_pBuffer) + m_CurrentOffset;

    m_CurrentOffset += size;

    return true;
}

void RioBufferManager::Finalize()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (m_BufferId != RIO_INVALID_BUFFERID)
    {
        RioExtension::GetTable().RIODeregisterBuffer(m_BufferId);
        m_BufferId = RIO_INVALID_BUFFERID;
    }

    if (m_pBuffer != nullptr)
    {
        ::VirtualFree(m_pBuffer, 0, MEM_RELEASE);
        m_pBuffer = nullptr;
    }

    m_TotalSize = 0;
    m_CurrentOffset = 0;
}

} // namespace LibNetworks::Core
