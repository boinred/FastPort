module;

#include <vector>
#include <algorithm>
#include <cstring>

export module commons.buffers.circle_buffer_queue;

import std;
import commons.rwlock;
import commons.buffers.ibuffer;

namespace LibCommons::Buffers
{

export class CircleBufferQueue final : public IBuffer
{
public:
    explicit CircleBufferQueue(size_t capacity)
        : m_Capacity(capacity)
    {
        if (capacity > 0)
        {
            m_Buffer.resize(capacity);
        }
    }

    ~CircleBufferQueue() override = default;

    CircleBufferQueue(const CircleBufferQueue&) = delete;
    CircleBufferQueue& operator=(const CircleBufferQueue&) = delete;

    // 버퍼에 데이터를 씁니다.
    bool Write(const void* pData, size_t size) override
    {
        auto lock = LibCommons::WriteLockBlock(m_RWLock);

        if (m_Capacity - m_Size < size)
        {
            return false;
        }

        const char* byteData = static_cast<const char*>(pData);
        size_t writeIndex = m_Head;
        size_t firstPart = std::min(size, m_Capacity - writeIndex);

        std::memcpy(&m_Buffer[writeIndex], byteData, firstPart);

        if (size > firstPart)
        {
            std::memcpy(&m_Buffer[0], byteData + firstPart, size - firstPart);
        }

        m_Head = (m_Head + size) % m_Capacity;
        m_Size += size;

        return true;
    }

    // 버퍼에서 데이터를 읽고 제거합니다.
    bool Pop(void* outBuffer, size_t size) override
    {
        auto lock = LibCommons::WriteLockBlock(m_RWLock);

        if (m_Size < size)
        {
            return false;
        }

        char* byteBuffer = static_cast<char*>(outBuffer);
        size_t readIndex = m_Tail;
        size_t firstPart = std::min(size, m_Capacity - readIndex);

        std::memcpy(byteBuffer, &m_Buffer[readIndex], firstPart);

        if (size > firstPart)
        {
            std::memcpy(byteBuffer + firstPart, &m_Buffer[0], size - firstPart);
        }

        m_Tail = (m_Tail + size) % m_Capacity;
        m_Size -= size;

        return true;
    }

    // 버퍼에서 데이터를 읽기만 하고 제거하지 않습니다.
    bool Peek(void* outBuffer, size_t size) override
    {
        auto lock = LibCommons::ReadLockBlock(m_RWLock);

        if (m_Size < size)
        {
            return false;
        }

        char* byteBuffer = static_cast<char*>(outBuffer);
        size_t readIndex = m_Tail;
        size_t firstPart = std::min(size, m_Capacity - readIndex);

        std::memcpy(byteBuffer, &m_Buffer[readIndex], firstPart);

        if (size > firstPart)
        {
            std::memcpy(byteBuffer + firstPart, &m_Buffer[0], size - firstPart);
        }

        return true;
    }

    // 버퍼에서 데이터를 제거합니다.
    bool Consume(size_t size) override
    {
        if (auto lock = LibCommons::WriteLockBlock(m_RWLock))
        {
            if (m_Size < size)
            {
                return false;
            }

            m_Tail = (m_Tail + size) % m_Capacity;
            m_Size -= size;
        }

        return true;
    }

    size_t CanReadSize() const override
    {
        auto lock = LibCommons::ReadLockBlock(m_RWLock);
        return m_Size;
    }

    size_t CanWriteSize() const override
    {
        auto lock = LibCommons::ReadLockBlock(m_RWLock);
        return m_Capacity - m_Size;
    }

    void Clear() override
    {
        if (auto lock = LibCommons::WriteLockBlock(m_RWLock))
        {
            m_Head = 0;
            m_Tail = 0;
            m_Size = 0;
        }
    }

private:
    std::vector<char> m_Buffer;
    size_t m_Head = 0;
    size_t m_Tail = 0;
    size_t m_Size = 0;
    size_t m_Capacity = 0;
    mutable LibCommons::RWLock m_RWLock;
};

} // namespace LibCommons::Buffers
