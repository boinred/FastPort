// RIOInboundSession.cpp
// -----------------------------------------------------------------------------
// RIOInboundSession 의 구현.
// 세션 등록/해제 및 패킷 ID 별 핸들러 구현(에코 / 벤치마크).
// -----------------------------------------------------------------------------
module;

#include <utility>
#include <spdlog/spdlog.h>
#include <memory>
#include <chrono>
#include <Protocols/Commons.pb.h>
#include <Protocols/Tests.pb.h>
#include <Protocols/Benchmark.pb.h>


module rio_inbound_session;

import commons.logger;
import commons.container;
import commons.singleton;
import networks.admin.admin_packet_handler;


// RIOServiceMode.cpp 의 전역 핸들러 액세스.
LibNetworks::Admin::AdminPacketHandler* GetGlobalRIOAdminHandler() noexcept;


// 프로세스 전역 세션 컨테이너. id → weak 소유 shared_ptr 매핑.
// SingleTon<T> 이용 — 동일 타입의 전역 인스턴스 공유.
using SessionContainer = LibCommons::Container<uint64_t, std::shared_ptr<LibNetworks::Sessions::RIOSession>>;


namespace
{

// 패킷 ID 매핑.
// ECHO 는 Commons.proto 의 ProtocolId 와 일치시켜 기존 에코 클라이언트와 호환.
// BENCHMARK 는 전용 ID 대역(0x1001~0x1002)을 사용.
constexpr uint16_t PACKET_ID_ECHO_REQUEST       = static_cast<uint16_t>(::fastport::protocols::commons::ProtocolId::PROTOCOL_ID_TESTS);
constexpr uint16_t PACKET_ID_BENCHMARK_REQUEST  = 0x1001;
constexpr uint16_t PACKET_ID_BENCHMARK_RESPONSE = 0x1002;


// 벤치마크용 고해상도 타임스탬프(ns).
// steady_clock 이 아닌 high_resolution_clock 사용 — 게임 루프 측정 일관성 유지.
uint64_t GetCurrentTimeNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count());
}

} // anonymous namespace


RIOInboundSession::RIOInboundSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket,
                                     const LibNetworks::Core::RioBufferSlice& recvSlice,
                                     const LibNetworks::Core::RioBufferSlice& sendSlice,
                                     RIO_CQ completionQueue)
    : LibNetworks::Sessions::RIOSession(pSocket, recvSlice, sendSlice, completionQueue)
{
    // 실제 초기화는 부모 생성자에서 RIO 관련 자원(RQ/CQ 연결) 설정. 여기는 비어있음.
}


RIOInboundSession::~RIOInboundSession()
{
    // 소멸 시점 로그 — 릭 추적 시 유용.
    // RIO 버퍼 슬라이스 반환/RQ 해제는 RIOSession(부모) 소멸자가 처리.
    LibCommons::Logger::GetInstance().LogInfo("RIOInboundSession", "Destructor called. Session Id : {}", GetSessionId());
}


void RIOInboundSession::OnAccepted()
{
    // 전역 세션 맵에 등록. shared_from_this 로 자신의 shared_ptr 을 얻어 저장.
    // → 세션은 맵 + IO 콜백 양쪽에서 참조되어 수명이 보장됨.
    auto& sessions = LibCommons::SingleTon<SessionContainer>::GetInstance();
    sessions.Add(GetSessionId(), std::dynamic_pointer_cast<RIOInboundSession>(shared_from_this()));

    __super::OnAccepted();

    LibCommons::Logger::GetInstance().LogInfo("RIOInboundSession", "OnAccepted. Session Id : {}", GetSessionId());
}


void RIOInboundSession::OnDisconnected()
{
    __super::OnDisconnected();

    LibCommons::Logger::GetInstance().LogInfo("RIOInboundSession", "OnDisconnected. Session Id : {}", GetSessionId());

    // 맵에서 제거 — 이 호출 이후 맵이 갖고 있던 ref 가 사라지므로 더 이상 IO 가 없으면 소멸.
    auto& sessions = LibCommons::SingleTon<SessionContainer>::GetInstance();
    sessions.Remove(GetSessionId());
}


void RIOInboundSession::OnPacketReceived(const LibNetworks::Core::Packet& rfPacket)
{
    __super::OnPacketReceived(rfPacket);

    // 패킷 ID 기반 dispatch. 신규 패킷 추가 시 여기에 case 확장.
    const auto packetId = rfPacket.GetPacketId();

    // Design Ref: server-status §4.4 — Admin 패킷(0x8xxx) dispatch.
    if (LibNetworks::Admin::IsAdminPacketId(packetId))
    {
        if (auto* pAdmin = GetGlobalRIOAdminHandler())
        {
            if (pAdmin->HandlePacket(*this, rfPacket)) return;
        }
        LibCommons::Logger::GetInstance().LogWarning("RIOInboundSession",
            "Admin packet {:#06x} received but handler unavailable. Session Id : {}",
            packetId, GetSessionId());
        return;
    }

    switch (packetId)
    {
    case PACKET_ID_BENCHMARK_REQUEST:
        HandleBenchmarkRequest(rfPacket);
        break;

    case PACKET_ID_ECHO_REQUEST:
        HandleEchoRequest(rfPacket);
        break;

    default:
        // 알 수 없는 패킷은 로그만 — 클라이언트 연결을 끊지 않는 보수적 정책.
        // 악의적 패킷 공격 대응이 필요하면 추후 임계치/연결 종료 로직 추가.
        LibCommons::Logger::GetInstance().LogWarning("RIOInboundSession", "OnPacketReceived, Unknown packet id : {}. Session Id : {}", packetId, GetSessionId());
        break;
    }
}


void RIOInboundSession::HandleBenchmarkRequest(const LibNetworks::Core::Packet& rfPacket)
{
    // 서버 측 수신 타임스탬프 — 네트워크 왕복 레이턴시 분해용.
    uint64_t recvTimestamp = GetCurrentTimeNs();

    ::fastport::protocols::benchmark::BenchmarkRequest request;
    if (!rfPacket.ParseMessage(request))
    {
        LibCommons::Logger::GetInstance().LogError("RIOInboundSession",
            "HandleBenchmarkRequest, Failed to parse. Session Id : {}", GetSessionId());
        return;
    }

    // 응답 패킷 구성.
    ::fastport::protocols::benchmark::BenchmarkResponse response;

    // 헤더 복제: request_id / timestamp_ms — 클라이언트의 in-flight 요청 매칭용.
    auto pHeader = response.mutable_header();
    pHeader->set_request_id(request.header().request_id());
    pHeader->set_timestamp_ms(request.header().timestamp_ms());

    response.set_result(::fastport::protocols::commons::ResultCode::RESULT_CODE_OK);

    // 레이턴시 분석용 4개 타임스탬프:
    //   client_timestamp_ns : 클라이언트 송신 시각 (요청에서 복제)
    //   server_recv_timestamp_ns : 서버 수신 시각
    //   server_send_timestamp_ns : 서버 송신 시각 (이 response 구성 완료 시점)
    //   (클라이언트 수신은 클라이언트 측에서 기록)
    response.set_client_timestamp_ns(request.client_timestamp_ns());
    response.set_server_recv_timestamp_ns(recvTimestamp);
    response.set_server_send_timestamp_ns(GetCurrentTimeNs());

    // 순서 확인용 시퀀스 + 페이로드 에코.
    response.set_sequence(request.sequence());
    response.set_payload(request.payload());

    SendMessage(PACKET_ID_BENCHMARK_RESPONSE, response);
}


void RIOInboundSession::HandleEchoRequest(const LibNetworks::Core::Packet& rfPacket)
{
    const auto packetId = rfPacket.GetPacketId();

    LibCommons::Logger::GetInstance().LogInfo("RIOInboundSession", "HandleEchoRequest. Session Id : {}, Data Length : {}", GetSessionId(), rfPacket.GetPacketSize());

    ::fastport::protocols::tests::EchoRequest request;
    if (!rfPacket.ParseMessage(request))
    {
        LibCommons::Logger::GetInstance().LogError("RIOInboundSession", "HandleEchoRequest, Failed to parse. Session Id : {}, Packet Id : {}", GetSessionId(), packetId);
        return;
    }

    // 에코 응답: request_id 에 +1 (ack 의미), 나머지는 그대로 반사.
    ::fastport::protocols::tests::EchoResponse response;
    auto pHeader = response.mutable_header();
    pHeader->set_request_id(request.header().request_id() + 1);
    pHeader->set_timestamp_ms(static_cast<uint64_t>(time(nullptr)));
    response.set_result(::fastport::protocols::commons::ResultCode::RESULT_CODE_OK);
    response.set_data_str(request.data_str());

    // 에코는 요청과 동일 packetId 로 응답하는 규약.
    SendMessage(packetId, response);

    LibCommons::Logger::GetInstance().LogInfo("RIOInboundSession", "HandleEchoRequest, Response sent. Session Id : {}, Packet Id: {}, Request Id : {}", GetSessionId(), packetId, request.header().request_id());
}
