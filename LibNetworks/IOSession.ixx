module;

#include <memory>
#include <array>
#include <vector>
#include <atomic>
#include <mutex>
#include <WinSock2.h>

export module networks.sessions.io_session;

import networks.core.io_consumer;
import networks.core.socket;
import commons.buffers.ibuffer;

namespace LibNetworks::Sessions
{


export class IOSession : public Core::IIOConsumer, public std::enable_shared_from_this<IOSession>
{
public:
    IOSession() = delete;
    IOSession(const IOSession&) = delete;
    IOSession& operator=(const IOSession&) = delete;

    explicit IOSession(const std::shared_ptr<Core::Socket>& pSocket,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer);

    virtual ~IOSession() = default;

    const uint64_t GetSessionId() const { return m_SessionId; }

    virtual void OnAccepted() {}
    virtual void OnConnected() {}

    virtual void OnDisconnected() {}

    virtual void OnReceive(const char* pData, size_t dataLength) {}
    virtual void OnSent(size_t bytesSent) {}

    void SendBuffer(const char* pData, size_t dataLength);

protected:
    void RequestReceived();

protected:
    const std::shared_ptr<Core::Socket> GetSocket() const { return m_pSocket; }

    // IIOConsumer을(를) 통해 상속됨
    void OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped) override;

private:
    enum class IOType : uint8_t
    {
        Recv,
        Send,
    };

    struct OverlappedEx
    {
        OVERLAPPED Overlapped{};
        IOType Type{};
        std::vector<char> SendBuffer{}; // Send일 때 WSASend 완료까지 데이터 유지
        size_t RequestedBytes = 0;
    };

private:
    bool PostRecv();

    // 큐를 사용한 Send
    bool TryPostSendFromQueue();

private:
    std::unique_ptr<LibCommons::Buffers::IBuffer> m_pReceiveBuffer{};
    std::unique_ptr<LibCommons::Buffers::IBuffer> m_pSendBuffer{};

    std::array<char, 64 * 1024> m_RecvTempBuffer{};

    std::mutex m_SendLock;
    std::atomic_bool m_SendInProgress = false;

    std::shared_ptr<Core::Socket> m_pSocket = {};

    uint64_t m_SessionId = m_NextSessionId.fetch_add(1, std::memory_order_relaxed);

    inline static std::atomic<uint64_t> m_NextSessionId = 1;

};


} // namespace LibNetworks::Sessions