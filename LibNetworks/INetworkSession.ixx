module;

#include <cstdint>
#include <google/protobuf/message.h>

#include <WinSock2.h>

export module networks.sessions.inetwork_session;

import commons.buffers.ibuffer;

namespace LibNetworks::Sessions
{

// Design Ref: session-idle-timeout §3.1 — 세션 종료 사유.
// RequestDisconnect(reason) 및 SessionIdleChecker 에서 사용. OnDisconnected 시그니처는 유지하고
// 사유는 로그/내부 기록으로만 전파 (Plan Q3 = c 결정).
export enum class DisconnectReason : std::uint8_t
{
    Normal       = 0,  // 일반 요청 / graceful close
    IdleTimeout  = 1,  // SessionIdleChecker 가 감지한 비정상 단절
    Backpressure = 2,  // 송신 큐 임계 초과 (RIOSession 에서 이미 사용 가능)
    Protocol     = 3,  // 프로토콜 위반
    Server       = 4,  // 서버 측 명시 종료 (관리자/셧다운)
};


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

protected:
    // 수신 데이터 누적 버퍼(큐).
    std::unique_ptr<LibCommons::Buffers::IBuffer> m_pReceiveBuffer{};
    // 송신 데이터 누적 버퍼(큐).
    std::unique_ptr<LibCommons::Buffers::IBuffer> m_pSendBuffer{};
};

} // namespace LibNetworks::Sessions
