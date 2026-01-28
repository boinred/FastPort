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

    // 버퍼의 전체 용량을 반환.
    virtual const int GetMaxSize() const = 0;

    // 데이터를 버퍼에 복사.
    virtual bool Write(std::span<const std::byte> data) = 0;

    // 데이터를 버퍼에서 읽고 제거.
    virtual bool Pop(std::span<std::byte> outBuffer) = 0;

    // 데이터를 제거하지 않고 읽기.
    virtual bool Peek(std::span<std::byte> outBuffer) = 0;

    // (Scatter-Gather I/O) 읽을 수 있는 데이터의 연속된 메모리 블록들을 반환.
    // Zero-Copy 읽기를 지원하며, 반환된 버퍼들은 Consume() 호출 전까지 유효.
    virtual size_t GetReadBuffers(std::vector<std::span<const std::byte>>& outBuffers) = 0;

    // (Zero-Copy Write) 쓰기 공간을 예약하고 해당 메모리 블록들을 반환.
    // 호출 즉시 내부 Head 포인터가 이동하므로, 반환된 버퍼에 데이터를 채워야 합니다.
    virtual bool AllocateWrite(size_t size, std::vector<std::span<std::byte>>& outBuffers) = 0;

    // (Zero-Copy) 읽기 버퍼 포인터(Tail)를 수동으로 이동시켜 데이터를 제거 처리.
    // GetReadBuffers() 등으로 직접 데이터 처리 후 호출.
    virtual bool Consume(size_t size) = 0;

    // 현재 읽을 수 있는 데이터의 총 크기를 반환.
    virtual size_t CanReadSize() const = 0;

    // 현재 쓸 수 있는 여유 공간의 크기를 반환.
    virtual size_t CanWriteSize() const = 0;

    // 버퍼의 내용을 모두 비움.
    virtual void Clear() = 0;

    // 벡터로 데이터를 복사.
    virtual size_t Peek(std::vector<char>& outBuffer)
    {
        outBuffer.clear();
        return 0;
    }

    // 벡터로 데이터를 복사하며 꺼냄.
    virtual size_t Pop(std::vector<char>& outBuffer)
    {
        outBuffer.clear();
        return 0;
    }
};

}
