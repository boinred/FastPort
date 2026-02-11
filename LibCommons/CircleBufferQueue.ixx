module;

#include <vector>
#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

export module commons.buffers.circle_buffer_queue;

import std;
import commons.rwlock;
import commons.buffers.ibuffer;
import commons.logger;

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
    bool Write(std::span<const std::byte> data) override
    {
        auto lock = LibCommons::WriteLockBlock(m_RWLock);

        const size_t size = data.size();
        if (m_Capacity - m_Size < size)
        {
            LibCommons::Logger::GetInstance().LogError("CircleBufferQueue", "Write() Insufficient space. Requested Size : {}, Available Size : {}", size, m_Capacity - m_Size);

            return false;
        }

        const char* byteData = reinterpret_cast<const char*>(data.data());
        const size_t writeIndex = m_Head;
        const size_t firstPart = std::min(size, m_Capacity - writeIndex);

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
    bool Pop(std::span<std::byte> outBuffer) override
    {
        auto lock = LibCommons::WriteLockBlock(m_RWLock);

        const size_t size = outBuffer.size();
        if (m_Size < size)
        {
            LibCommons::Logger::GetInstance().LogError("CircleBufferQueue", "Pop() Insufficient data. Requested Size : {}, Available Size : {}", size, m_Size);
            return false;
        }

        char* byteBuffer = reinterpret_cast<char*>(outBuffer.data());
        const size_t readIndex = m_Tail;
        const size_t firstPart = std::min(size, m_Capacity - readIndex);

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
    bool Peek(std::span<std::byte> outBuffer) override
    {
        auto lock = LibCommons::ReadLockBlock(m_RWLock);

        const size_t size = outBuffer.size();
        if (m_Size < size)
        {
            LibCommons::Logger::GetInstance().LogError("CircleBufferQueue", "Peek() Insufficient data. Requested Size : {}, Available Size : {}", size, m_Size);

            return false;
        }

        char* byteBuffer = reinterpret_cast<char*>(outBuffer.data());
        const size_t readIndex = m_Tail;
        const size_t firstPart = std::min(size, m_Capacity - readIndex);

        std::memcpy(byteBuffer, &m_Buffer[readIndex], firstPart);

        if (size > firstPart)
        {
            std::memcpy(byteBuffer + firstPart, &m_Buffer[0], size - firstPart);
        }

        return true;
    }

    size_t GetReadBuffers(std::vector<std::span<const std::byte>>& outBuffers) override
    {
        auto lock = LibCommons::ReadLockBlock(m_RWLock);
        outBuffers.clear();

        if (m_Size == 0)
        {
            LibCommons::Logger::GetInstance().LogError("CircleBufferQueue", "GetReadBuffers() Buffer is empty.");
            return 0;
        }

        const size_t firstPart = std::min(m_Size, m_Capacity - m_Tail);
        outBuffers.emplace_back(std::as_bytes(std::span(m_Buffer.data() + m_Tail, firstPart)));

        if (m_Size > firstPart)
        {
            const size_t secondPart = m_Size - firstPart;
            outBuffers.emplace_back(std::as_bytes(std::span(m_Buffer.data(), secondPart)));
        }

        return m_Size;
    }

    // 버퍼에 쓰기 공간을 예약하고, 예약된 공간의 포인터들을 반환합니다 (Zero-Copy Write 지원)
    // 이 함수를 호출하면 즉시 Head가 이동합니다. (Commit 불필요)
    bool AllocateWrite(size_t size, std::vector<std::span<std::byte>>& outBuffers) override
    {
        auto lock = LibCommons::WriteLockBlock(m_RWLock);
        outBuffers.clear();

        if (m_Capacity - m_Size < size)
        {
            LibCommons::Logger::GetInstance().LogError("CircleBufferQueue", "AllocateWrite() Insufficient space. Requested Size : {}, Available Size : {}", size, m_Capacity - m_Size);

            return false;
        }

        size_t writeIndex = m_Head;
        size_t firstPart = std::min(size, m_Capacity - writeIndex);

        // reinterpret_cast to std::byte*
        std::byte* bufferData = reinterpret_cast<std::byte*>(m_Buffer.data());

        outBuffers.emplace_back(std::span(bufferData + writeIndex, firstPart));

        if (size > firstPart)
        {
            size_t secondPart = size - firstPart;
            outBuffers.emplace_back(std::span(bufferData, secondPart));
        }

        m_Head = (m_Head + size) % m_Capacity;
        m_Size += size;

        return true;
    }

    // 버퍼에서 데이터를 제거합니다.
    bool Consume(size_t size) override
    {
        auto lock = LibCommons::WriteLockBlock(m_RWLock);
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
        auto lock = LibCommons::WriteLockBlock(m_RWLock);
        m_Head = 0;
        m_Tail = 0;
        m_Size = 0;
    }

    // (Direct I/O Recv) 현재 쓰기 가능한 모든 연속된 메모리 블록들을 반환.
    size_t GetWriteableBuffers(std::vector<std::span<std::byte>>& outBuffers) override
    {
        auto lock = LibCommons::WriteLockBlock(m_RWLock);
        outBuffers.clear();

        const size_t freeSpace = m_Capacity - m_Size;
        if (freeSpace == 0)
        {
            LibCommons::Logger::GetInstance().LogError("CircleBufferQueue", "GetWriteableBuffers() Buffer is full.");
            return 0;
        }

        size_t writeIndex = m_Head;
        size_t firstPart = std::min(freeSpace, m_Capacity - writeIndex);

        std::byte* bufferData = reinterpret_cast<std::byte*>(m_Buffer.data());
        outBuffers.emplace_back(std::span(bufferData + writeIndex, firstPart));

        if (freeSpace > firstPart)
        {
            size_t secondPart = freeSpace - firstPart;
            outBuffers.emplace_back(std::span(bufferData, secondPart));
        }

        return freeSpace;
    }

    // (Direct I/O Recv) 쓰기 작업 완료 후, 실제로 쓴 크기만큼 포인터(Head)를 이동.
    bool CommitWrite(size_t size) override
    {
        auto lock = LibCommons::WriteLockBlock(m_RWLock);

        if (m_Capacity - m_Size < size)
        {
            // 이론상 발생하면 안 됨 (GetWriteableBuffers 이후라면)
            return false;
        }

        m_Head = (m_Head + size) % m_Capacity;
        m_Size += size;

        return true;
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
