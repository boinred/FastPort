// AdminPacketHandlerTests.cpp
// -----------------------------------------------------------------------------
// Design Ref: server-status §8.6 — AdminPacketHandler dispatch 단위 테스트 (AH-01 ~ AH-05).
// FakeSession 으로 SendMessage 캡처. 실제 Collector(MockSessionStats provider) 와 함께
// 엔드-투-엔드 검증 (Collector 는 별도 테스트에서 커버, 여기선 dispatch 중심).
// -----------------------------------------------------------------------------
#include "CppUnitTest.h"

#include <WinSock2.h>
#include <memory>
#include <vector>
#include <string>
#include <utility>
#include <cstdint>

#include <google/protobuf/message.h>
#include <Protocols/Admin.pb.h>
#include <Protocols/Commons.pb.h>

import networks.admin.admin_packet_handler;
import networks.stats.server_stats_collector;
import networks.stats.stats_sampler;
import networks.sessions.inetwork_session;
import networks.sessions.isession_stats;
import networks.core.packet;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LibNetworksTests
{

// -----------------------------------------------------------------------------
// Fake / Mock helpers
// -----------------------------------------------------------------------------

// Fake INetworkSession — SendMessage 호출을 (packetId, serialized bytes) 로 캡처.
struct FakeSession : public LibNetworks::Sessions::INetworkSession
{
    std::vector<std::pair<std::uint16_t, std::string>> sentMessages;
    std::uint64_t sessionId = 7777ULL;

    void SendMessage(const std::uint16_t packetId,
                     const google::protobuf::Message& rfMessage) override
    {
        std::string wire;
        rfMessage.SerializeToString(&wire);
        sentMessages.emplace_back(packetId, std::move(wire));
    }

    std::uint64_t GetSessionId() const override { return sessionId; }
    void OnAccepted()     override {}
    void OnConnected()    override {}
    void OnDisconnected() override {}
};


// Mock ISessionStats (ServerStatsCollectorTests 와 동일 패턴, 중복 회피 위해 재정의).
struct AhMockSessionStats : public LibNetworks::Sessions::ISessionStats
{
    std::uint64_t rx = 0;
    std::uint64_t tx = 0;
    std::uint64_t GetTotalRxBytes() const noexcept override { return rx; }
    std::uint64_t GetTotalTxBytes() const noexcept override { return tx; }
};


namespace
{
// 여러 Mock 생성 헬퍼.
std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>>
MakeMocks(std::uint32_t count)
{
    std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        auto pMock = std::make_shared<AhMockSessionStats>();
        pMock->rx = 1000ULL * (static_cast<std::uint64_t>(i) + 1);
        pMock->tx = 500ULL  * (static_cast<std::uint64_t>(i) + 1);
        out.push_back(pMock);
    }
    return out;
}

// Admin.proto 의 메시지를 Packet 으로 래핑 (id + serialized body).
template <class MsgT>
LibNetworks::Core::Packet MakeAdminPacket(std::uint16_t packetId, const MsgT& msg)
{
    std::string body;
    msg.SerializeToString(&body);
    return LibNetworks::Core::Packet(packetId, std::move(body));
}
} // anonymous namespace


// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

TEST_CLASS(AdminPacketHandlerTests)
{
public:

    // AH-01: Non-admin 패킷(0x1001 Echo) → HandlePacket returns false, 아무것도 송신 안 함.
    TEST_METHOD(Handle_NonAdminPacket_ReturnsFalse)
    {
        LibNetworks::Stats::ServerStatsCollector collector(
            LibNetworks::Stats::ServerMode::IOCP,
            []() { return std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>>{}; },
            []() -> std::uint64_t { return 0ULL; },
            nullptr);

        LibNetworks::Admin::AdminPacketHandler handler(collector);

        // 0x1001 = non-admin (Echo 등 일반 대역).
        LibNetworks::Core::Packet packet(static_cast<std::uint16_t>(0x1001), std::string{});

        FakeSession session;
        const bool handled = handler.HandlePacket(session, packet);

        Assert::IsFalse(handled, L"non-admin 패킷은 HandlePacket 이 false 반환해야 함");
        Assert::IsTrue(session.sentMessages.empty(),
            L"non-admin 패킷에 대해서는 응답을 보내면 안 됨");
    }

    // AH-02: 0x8001 SummaryRequest → FakeSession 에 0x8002 응답 1개.
    TEST_METHOD(Handle_SummaryRequest_SendsResponse)
    {
        auto mocks = MakeMocks(3);

        LibNetworks::Stats::ServerStatsCollector collector(
            LibNetworks::Stats::ServerMode::IOCP,
            [mocks]() { return mocks; },
            []() -> std::uint64_t { return 0ULL; },
            nullptr);

        LibNetworks::Admin::AdminPacketHandler handler(collector);

        ::fastport::protocols::admin::AdminStatusSummaryRequest request;
        request.mutable_header()->set_request_id(1);
        request.mutable_header()->set_timestamp_ms(1000);

        const auto packet = MakeAdminPacket(
            LibNetworks::Admin::kPacketId_SummaryRequest, request);

        FakeSession session;
        const bool handled = handler.HandlePacket(session, packet);

        Assert::IsTrue(handled, L"admin 패킷은 consumed 처리");
        Assert::AreEqual(static_cast<size_t>(1), session.sentMessages.size());
        Assert::AreEqual<std::uint16_t>(
            LibNetworks::Admin::kPacketId_SummaryResponse,
            session.sentMessages.front().first);
    }

    // AH-03: Summary 응답 파싱하여 주요 필드가 올바르게 채워졌는지 검증.
    TEST_METHOD(Handle_SummaryResponse_FieldsPopulated)
    {
        auto mocks = MakeMocks(3);  // rx: 1000/2000/3000 = 합 6000, tx: 500/1000/1500 = 합 3000

        LibNetworks::Stats::ServerStatsCollector collector(
            LibNetworks::Stats::ServerMode::RIO,
            [mocks]() { return mocks; },
            []() -> std::uint64_t { return 11ULL; },
            nullptr);

        LibNetworks::Admin::AdminPacketHandler handler(collector);

        ::fastport::protocols::admin::AdminStatusSummaryRequest request;
        request.mutable_header()->set_request_id(99);
        request.mutable_header()->set_timestamp_ms(55555);

        const auto packet = MakeAdminPacket(
            LibNetworks::Admin::kPacketId_SummaryRequest, request);

        FakeSession session;
        Assert::IsTrue(handler.HandlePacket(session, packet));
        Assert::AreEqual(static_cast<size_t>(1), session.sentMessages.size());

        ::fastport::protocols::admin::AdminStatusSummaryResponse response;
        Assert::IsTrue(response.ParseFromString(session.sentMessages.front().second),
            L"응답 파싱 실패");

        // 헤더는 요청의 값을 그대로 echo.
        Assert::AreEqual<std::uint64_t>(99ULL, response.header().request_id());
        Assert::AreEqual<std::uint64_t>(55555ULL, response.header().timestamp_ms());

        // 집계 값.
        Assert::AreEqual<std::uint32_t>(3u,    response.active_session_count());
        Assert::AreEqual<std::uint64_t>(6000ULL, response.total_rx_bytes());
        Assert::AreEqual<std::uint64_t>(3000ULL, response.total_tx_bytes());
        Assert::AreEqual<std::uint64_t>(11ULL, response.idle_disconnect_count());

        // RIO → SERVER_MODE_RIO 변환.
        Assert::IsTrue(response.server_mode() == ::fastport::protocols::admin::SERVER_MODE_RIO);

        Assert::IsTrue(response.result() == ::fastport::protocols::commons::RESULT_CODE_OK);
    }

    // AH-04: SessionList 요청 offset=2, limit=3 → response.sessions_size == 3, total == 5.
    TEST_METHOD(Handle_SessionListRequest_Pagination)
    {
        auto mocks = MakeMocks(5);

        LibNetworks::Stats::ServerStatsCollector collector(
            LibNetworks::Stats::ServerMode::IOCP,
            [mocks]() { return mocks; },
            []() -> std::uint64_t { return 0ULL; },
            nullptr);

        LibNetworks::Admin::AdminPacketHandler handler(collector);

        ::fastport::protocols::admin::AdminSessionListRequest request;
        request.mutable_header()->set_request_id(3);
        request.set_offset(2);
        request.set_limit(3);

        const auto packet = MakeAdminPacket(
            LibNetworks::Admin::kPacketId_SessionListReq, request);

        FakeSession session;
        Assert::IsTrue(handler.HandlePacket(session, packet));

        Assert::AreEqual(static_cast<size_t>(1), session.sentMessages.size());
        Assert::AreEqual<std::uint16_t>(
            LibNetworks::Admin::kPacketId_SessionListRes,
            session.sentMessages.front().first);

        ::fastport::protocols::admin::AdminSessionListResponse response;
        Assert::IsTrue(response.ParseFromString(session.sentMessages.front().second));

        Assert::AreEqual<std::uint32_t>(5u, response.total());
        Assert::AreEqual<std::uint32_t>(2u, response.offset());
        Assert::AreEqual(3, response.sessions_size(),
            L"offset=2 limit=3 이면 3개 세션 엔트리 포함");
    }

    // AH-05: Malformed body → HandlePacket returns true (consumed), 응답 없음.
    // protobuf 파싱 실패를 유도하기 위해 invalid wire type(7) 을 포함하는 바이트 사용.
    TEST_METHOD(Handle_MalformedPacket_LogsError)
    {
        LibNetworks::Stats::ServerStatsCollector collector(
            LibNetworks::Stats::ServerMode::IOCP,
            []() { return std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>>{}; },
            []() -> std::uint64_t { return 0ULL; },
            nullptr);

        LibNetworks::Admin::AdminPacketHandler handler(collector);

        // 0x0F = tag(1) + wire type 7 (reserved, invalid) — 대부분의 protobuf 파서가 거부.
        std::string badBody("\x0F\xFF\xFF\xFF\xFF", 5);
        LibNetworks::Core::Packet packet(
            LibNetworks::Admin::kPacketId_SummaryRequest, std::move(badBody));

        FakeSession session;
        const bool handled = handler.HandlePacket(session, packet);

        Assert::IsTrue(handled,
            L"admin 대역은 parse 실패여도 consumed 로 반환 (일반 dispatch 방지)");
        Assert::IsTrue(session.sentMessages.empty(),
            L"parse 실패 시 응답을 보내지 않아야 함");
    }
};

} // namespace LibNetworksTests
