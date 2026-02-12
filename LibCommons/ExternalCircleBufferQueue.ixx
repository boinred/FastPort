module;

#include <vector>
#include <algorithm>
#include <cstring>

export module commons.buffers.external_circle_buffer_queue;

import std;
import commons.buffers.ibuffer;

namespace LibCommons::Buffers
{

/**
 * 외부에서 관리되는 메모리(std::span)를 사용하는 원형 버퍼 큐.
 * RIO와 같이 미리 등록된 메모리를 사용해야 하는 경우 유용.
 * 
 * [Thread Safety]
 * 이 클래스는 스레드 세이프하지 않습니다. 
 * 멀티 스레드 환경에서 사용할 경우 반드시 외부에서 동기화(Lock)를 보장해야 합니다.
 */
export class ExternalCircleBufferQueue final : public IBuffer
{
public:
    explicit ExternalCircleBufferQueue(std::span<std::byte> externalBuffer)
        : m_Buffer(externalBuffer), m_Capacity(externalBuffer.size())
    {
    }

    ~ExternalCircleBufferQueue() override = default;

    ExternalCircleBufferQueue(const ExternalCircleBufferQueue&) = delete;
    ExternalCircleBufferQueue& operator=(const ExternalCircleBufferQueue&) = delete;

    bool Write(std::span<const std::byte> data) override
    {
        const size_t size = data.size();
        if (m_Capacity - m_Size < size)
        {
            return false;
        }

        const std::byte* byteData = data.data();
        const size_t writeIndex = m_Head;
        const size_t firstPart = std::min(size, m_Capacity - writeIndex);

        std::memcpy(m_Buffer.data() + writeIndex, byteData, firstPart);

        if (size > firstPart)
        {
            std::memcpy(m_Buffer.data(), byteData + firstPart, size - firstPart);
        }

        m_Head = (m_Head + size) % m_Capacity;
        m_Size += size;

        return true;
    }

    bool Pop(std::span<std::byte> outBuffer) override
    {
        const size_t size = outBuffer.size();
        if (m_Size < size)
        {
            return false;
        }

        std::byte* byteBuffer = outBuffer.data();
        const size_t readIndex = m_Tail;
        const size_t firstPart = std::min(size, m_Capacity - readIndex);

        std::memcpy(byteBuffer, m_Buffer.data() + readIndex, firstPart);

        if (size > firstPart)
        {
            std::memcpy(byteBuffer + firstPart, m_Buffer.data(), size - firstPart);
        }

        m_Tail = (m_Tail + size) % m_Capacity;
        m_Size -= size;

        return true;
    }

    bool Peek(std::span<std::byte> outBuffer) override
    {
        const size_t size = outBuffer.size();
        if (m_Size < size)
        {
            return false;
        }

        std::byte* byteBuffer = outBuffer.data();
        const size_t readIndex = m_Tail;
        const size_t firstPart = std::min(size, m_Capacity - readIndex);

        std::memcpy(byteBuffer, m_Buffer.data() + readIndex, firstPart);

        if (size > firstPart)
        {
            std::memcpy(byteBuffer + firstPart, m_Buffer.data(), size - firstPart);
        }

        return true;
    }

    size_t GetReadBuffers(std::vector<std::span<const std::byte>>& outBuffers) override
    {
        outBuffers.clear();

        if (m_Size == 0)
        {
            return 0;
        }

        const size_t firstPart = std::min(m_Size, m_Capacity - m_Tail);
        outBuffers.emplace_back(m_Buffer.subspan(m_Tail, firstPart));

        if (m_Size > firstPart)
        {
            const size_t secondPart = m_Size - firstPart;
            outBuffers.emplace_back(m_Buffer.subspan(0, secondPart));
        }

        return m_Size;
    }

    bool AllocateWrite(size_t size, std::vector<std::span<std::byte>>& outBuffers) override
    {
        outBuffers.clear();

        if (m_Capacity - m_Size < size)
        {
            return false;
        }

        size_t writeIndex = m_Head;
        size_t firstPart = std::min(size, m_Capacity - writeIndex);

        outBuffers.emplace_back(m_Buffer.subspan(writeIndex, firstPart));

        if (size > firstPart)
        {
            size_t secondPart = size - firstPart;
            outBuffers.emplace_back(m_Buffer.subspan(0, secondPart));
        }

        m_Head = (m_Head + size) % m_Capacity;
        m_Size += size;

        return true;
    }

    bool Consume(size_t size) override
    {
        if (m_Size < size)
        {
            return false;
        }

        m_Tail = (m_Tail + size) % m_Capacity;
        m_Size -= size;

        return true;
    }

    size_t CanReadSize() const override
    {
        return m_Size;
    }

    size_t CanWriteSize() const override
    {
        return m_Capacity - m_Size;
    }

    void Clear() override
    {
        m_Head = 0;
        m_Tail = 0;
        m_Size = 0;
    }

    size_t GetWriteableBuffers(std::vector<std::span<std::byte>>& outBuffers) override
    {
        outBuffers.clear();

        const size_t freeSpace = m_Capacity - m_Size;
        if (freeSpace == 0)
        {
            return 0;
        }

        size_t writeIndex = m_Head;
        size_t firstPart = std::min(freeSpace, m_Capacity - writeIndex);

        outBuffers.emplace_back(m_Buffer.subspan(writeIndex, firstPart));

        if (freeSpace > firstPart)
        {
            size_t secondPart = freeSpace - firstPart;
            outBuffers.emplace_back(m_Buffer.subspan(0, secondPart));
        }

        return freeSpace;
    }

    bool CommitWrite(size_t size) override
    {
        if (m_Capacity - m_Size < size)
        {
            return false;
        }

        m_Head = (m_Head + size) % m_Capacity;
        m_Size += size;

        return true;
    }

private:
    std::span<std::byte> m_Buffer;
    size_t m_Head = 0;
    size_t m_Tail = 0;
    size_t m_Size = 0;
    size_t m_Capacity = 0;
};

} // namespace LibCommons::Buffers
