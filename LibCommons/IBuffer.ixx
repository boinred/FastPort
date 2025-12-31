module;

export module commons.buffers.ibuffer;

import std;

namespace LibCommons::Buffers
{

export class IBuffer
{
public:
    virtual ~IBuffer() = default;
    virtual const int GetMaxSize() const = 0;

    // 현재 읽을 수 있는 데이터 크기를 반환.
    virtual size_t CanReadSize() const = 0;

    // 현재 쓸 수 있는 남은 공간 크기를 반환.
    virtual size_t CanWriteSize() const = 0;

    // 버퍼에 데이터 쓰기.
    virtual bool Write(const void* pData, size_t size) = 0;

    // 버퍼에서 데이터를 읽고 제거.
    virtual bool Pop(void* outBuffer, size_t size) = 0;

    // 버퍼에서 데이터를 읽기만 하고 제거하지 않음.
    virtual bool Peek(void* outBuffer, size_t size) = 0;

    // 버퍼에서 데이터를 제거.
    virtual bool Consume(size_t size) = 0;

    // 버퍼의 내용을 모두 초기화.
    virtual void Clear() = 0;
};

} // namespace LibCommons::Buffers
