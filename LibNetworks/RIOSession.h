#pragma once

#include <WinSock2.h>
#include <MSWSock.h>
#include <memory>
#include <vector>
#include <atomic>
#include <span>
#include <google/protobuf/message.h>

// 모듈이 아닌 헤더이므로 필요한 타입들을 직접 참조하거나 전방 선언해야 함
#include "networks.core.socket" // 모듈을 헤더에서 임포트할 수 없으므로 구조적 변경 필요

namespace LibNetworks::Core {
    class Socket;
    struct RioBufferSlice;
    enum class RioOperationType;
}

namespace LibNetworks::Sessions {

class INetworkSession {
public:
    virtual ~INetworkSession() = default;
    virtual void SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage) = 0;
    virtual uint64_t GetSessionId() const = 0;
    virtual void OnAccepted() = 0;
    virtual void OnConnected() = 0;
    virtual void OnDisconnected() = 0;
    virtual OVERLAPPED* GetConnectOverlappedPtr() { return nullptr; }
};

class RIOSession : public INetworkSession, public std::enable_shared_from_this<RIOSession>
{
    // ...
};

}
