module;

#include <memory>

export module networks.sessions.io_session;

import networks.core.io_consumer;
import networks.core.socket;
import commons.buffers.ibuffer;
import commons.buffers.circle_buffer_queue;

namespace LibNetworks::Sessions
{


export class IOSession : public Core::IIOConsumer, public std::enable_shared_from_this<IOSession>
{
public:
    IOSession() = delete;
    IOSession(const IOSession&) = delete;
    IOSession& operator=(const IOSession&) = delete;

    explicit IOSession(const std::shared_ptr<Core::Socket>& pSocket);

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
    virtual void OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped) override {}

private:
    std::unique_ptr<LibCommons::Buffers::IBuffer> m_pReceiveBuffer = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(1024);
    std::unique_ptr<LibCommons::Buffers::IBuffer> m_pSendBuffer = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(1024);


    std::shared_ptr<Core::Socket> m_pSocket = {};

    uint64_t m_SessionId = m_NextSessionId.fetch_add(1, std::memory_order_relaxed);

    inline static std::atomic<uint64_t> m_NextSessionId = 1;

};


} // namespace LibNetworks::Sessions