// RIOInboundSession.ixx
// -----------------------------------------------------------------------------
// RIO 모드에서 AcceptEx 로 수락된 인바운드 연결을 다루는 세션 클래스.
//
// 역할:
//   - LibNetworks::Sessions::RIOSession 상속 → RIO 수신/송신 루프와 패킷 프레이밍
//     기반 위에서 애플리케이션 레벨 패킷 핸들러(에코, 벤치마크)만 구현.
//   - OnAccepted / OnDisconnected 시 전역 SessionContainer 에 등록·제거.
//   - OnPacketReceived 에서 패킷 ID 기반 dispatch.
//
// 지원 패킷:
//   - ECHO_REQUEST       : 수신 데이터를 그대로 응답 (기능 검증용)
//   - BENCHMARK_REQUEST  : 타임스탬프/시퀀스/페이로드 포함 왕복 레이턴시 측정용
// -----------------------------------------------------------------------------
module;

#include <WinSock2.h>
#include <MSWSock.h>

export module rio_inbound_session;

import networks.sessions.rio_session;
import commons.buffers.ibuffer;
import networks.core.packet;
// 동일 import 반복은 의도 없음. 추후 정리 가능.
import networks.core.packet;
import networks.core.rio_buffer_manager;


// RIO 전용 인바운드 세션.
// 복사/이동 불가. 소유권은 std::shared_ptr 로만 관리된다.
export class RIOInboundSession : public LibNetworks::Sessions::RIOSession
{
public:
    RIOInboundSession() = delete;
    RIOInboundSession(const RIOInboundSession&) = delete;
    RIOInboundSession& operator=(const RIOInboundSession&) = delete;

    // RIOSession 에 필요한 소켓 + RIO 고정 버퍼 2 슬라이스(수신/송신) + CompletionQueue 전달.
    // RioBufferSlice 는 RioBufferManager 의 전역 풀에서 할당된 영역을 가리키는 view.
    explicit RIOInboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket,
                               const LibNetworks::Core::RioBufferSlice& recvSlice,
                               const LibNetworks::Core::RioBufferSlice& sendSlice,
                               RIO_CQ completionQueue);

    // 소멸자는 세션 id 로그만 남김. 실제 RIO 자원 정리는 RIOSession(부모)가 담당.
    virtual ~RIOInboundSession() override;

    // AcceptEx 완료 → 세션 등록(SessionContainer) 및 로그.
    void OnAccepted() override;

    // 세션 종료 → 컨테이너에서 제거.
    void OnDisconnected() override;

protected:
    // 패킷 프레이밍 완료 후 상위에서 호출되는 훅. 패킷 ID 로 dispatch.
    void OnPacketReceived(const LibNetworks::Core::Packet& rfPacket) override;

private:
    // 벤치마크 요청 처리: 서버 수신/송신 타임스탬프 기록 후 동일 페이로드로 응답.
    void HandleBenchmarkRequest(const LibNetworks::Core::Packet& rfPacket);

    // 에코 요청 처리: 요청의 data_str 을 그대로 응답에 반영.
    void HandleEchoRequest(const LibNetworks::Core::Packet& rfPacket);
};
