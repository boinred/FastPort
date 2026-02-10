module;

#include <cstdint>
#include <google/protobuf/message.h>

#include <WinSock2.h>

export module networks.sessions.inetwork_session;

namespace LibNetworks::Sessions
{

/**
 * 네트워크 세션 인터페이스 (IOCP, RIO 공통)
 */
export class INetworkSession
{
public:
    virtual ~INetworkSession() = default;

    // 메시지를 상대방에게 전송합니다.
    virtual void SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage) = 0;

    // 세션 고유 식별자 조회
    virtual uint64_t GetSessionId() const = 0;

    // 연결 수락/완료 이벤트 처리 훅
    virtual void OnAccepted() = 0;
    virtual void OnConnected() = 0;

    // 연결 종료 이벤트 처리 훅
    virtual void OnDisconnected() = 0;

    // (Outbound 전용) ConnectEx용 OVERLAPPED 포인터 반환
    virtual OVERLAPPED* GetConnectOverlappedPtr() { return nullptr; }
};

} // namespace LibNetworks::Sessions
