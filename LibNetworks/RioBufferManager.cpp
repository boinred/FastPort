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

    // free list에서 동일 크기 슬라이스 재사용 시도
    for (auto it = m_FreeList.begin(); it != m_FreeList.end(); ++it)
    {
        if (it->Length == size)
        {
            outSlice = *it;
            m_FreeList.erase(it);
            ++m_AllocatedCount;
            return true;
        }
    }

    // 새 슬라이스 할당
    if (m_CurrentOffset + size > m_TotalSize)
    {
        return false;
    }

    outSlice.BufferId = m_BufferId;
    outSlice.Offset = m_CurrentOffset;
    outSlice.Length = size;
    outSlice.pData = reinterpret_cast<char*>(m_pBuffer) + m_CurrentOffset;

    m_CurrentOffset += size;
    ++m_AllocatedCount;

    return true;
}

void RioBufferManager::DeallocateSlice(const RioBufferSlice& slice)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    m_FreeList.push_back(slice);
    if (m_AllocatedCount > 0)
    {
        --m_AllocatedCount;
    }
}

uint32_t RioBufferManager::GetAllocatedCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_AllocatedCount;
}

uint32_t RioBufferManager::GetFreeCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return static_cast<uint32_t>(m_FreeList.size());
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
