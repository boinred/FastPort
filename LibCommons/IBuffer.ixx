module;

#include <cstddef>
#include <vector>

export module commons.buffers.ibuffer;

import std;

namespace LibCommons::Buffers
{

export class IBuffer
{
public:
    virtual ~IBuffer() = default;

    virtual const int GetMaxSize() const = 0;
    virtual bool Write(std::span<const std::byte> data) = 0;
    virtual bool Pop(std::span<std::byte> outBuffer) = 0;
    virtual bool Peek(std::span<std::byte> outBuffer) = 0;
    virtual size_t GetReadBuffers(std::vector<std::span<const std::byte>>& outBuffers) = 0;
    virtual bool AllocateWrite(size_t size, std::vector<std::span<std::byte>>& outBuffers) = 0;
    virtual bool Consume(size_t size) = 0;
    virtual size_t CanReadSize() const = 0;
    virtual size_t CanWriteSize() const = 0;
    virtual void Clear() = 0;

    // 원자적으로 데이터를 읽어오는 새로운 Peek 함수
    virtual size_t Peek(std::vector<char>& outBuffer)
    {
        outBuffer.clear();
        return 0;
    }

    virtual size_t Pop(std::vector<char>& outBuffer)
    {
        outBuffer.clear();
        return 0;
    }
};

}
