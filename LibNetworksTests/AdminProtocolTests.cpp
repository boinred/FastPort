// AdminProtocolTests.cpp
// -----------------------------------------------------------------------------
// Design Ref: server-status §8.2 — Admin proto 라운드트립 (AP-01 ~ AP-05).
// 목적: Protocols/Admin.proto 가 정의한 Request/Response 메시지를 직렬화 → 역직렬화
// 했을 때 모든 필드가 보존되는지 확인.
// 서버/클라이언트 의존 없음 — 순수 proto 레벨 테스트.
// -----------------------------------------------------------------------------
#include "CppUnitTest.h"

#include <string>
#include <cstdint>

#include <Protocols/Admin.pb.h>
#include <Protocols/Commons.pb.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LibNetworksTests
{

TEST_CLASS(AdminProtocolTests)
{
public:

    // AP-01: SummaryRequest 헤더 + auth_token 라운드트립.
    TEST_METHOD(SummaryRequest_Roundtrip)
    {
        ::fastport::protocols::admin::AdminStatusSummaryRequest src;
        auto* pHeader = src.mutable_header();
        pHeader->set_request_id(42);
        pHeader->set_timestamp_ms(1'700'000'000'123ULL);
        src.set_auth_token("dev-token");

        std::string wire;
        Assert::IsTrue(src.SerializeToString(&wire), L"Serialize failed");

        ::fastport::protocols::admin::AdminStatusSummaryRequest dst;
        Assert::IsTrue(dst.ParseFromString(wire), L"Parse failed");

        Assert::AreEqual<std::uint64_t>(42ULL, dst.header().request_id());
        Assert::AreEqual<std::uint64_t>(1'700'000'000'123ULL, dst.header().timestamp_ms());
        Assert::AreEqual(std::string("dev-token"), dst.auth_token());
    }

    // AP-02: SummaryResponse 의 모든 필드 보존.
    TEST_METHOD(SummaryResponse_AllFields)
    {
        ::fastport::protocols::admin::AdminStatusSummaryResponse src;
        auto* pHeader = src.mutable_header();
        pHeader->set_request_id(7);
        pHeader->set_timestamp_ms(1'700'000'000'000ULL);

        src.set_result(::fastport::protocols::commons::RESULT_CODE_OK);
        src.set_server_uptime_ms(123'456ULL);
        src.set_active_session_count(42u);
        src.set_total_rx_bytes(9'876'543ULL);
        src.set_total_tx_bytes(1'234'567ULL);
        src.set_idle_disconnect_count(3ULL);
        src.set_server_mode(::fastport::protocols::admin::SERVER_MODE_IOCP);
        src.set_process_memory_bytes(128ULL * 1024 * 1024);
        src.set_process_cpu_percent(4.25);
        src.set_server_timestamp_ms(1'700'000'000'999ULL);

        std::string wire;
        Assert::IsTrue(src.SerializeToString(&wire));

        ::fastport::protocols::admin::AdminStatusSummaryResponse dst;
        Assert::IsTrue(dst.ParseFromString(wire));

        Assert::AreEqual<std::uint64_t>(7ULL, dst.header().request_id());
        Assert::AreEqual<std::uint64_t>(1'700'000'000'000ULL, dst.header().timestamp_ms());
        Assert::IsTrue(dst.result() == ::fastport::protocols::commons::RESULT_CODE_OK);
        Assert::AreEqual<std::uint64_t>(123'456ULL, dst.server_uptime_ms());
        Assert::AreEqual<std::uint32_t>(42u, dst.active_session_count());
        Assert::AreEqual<std::uint64_t>(9'876'543ULL, dst.total_rx_bytes());
        Assert::AreEqual<std::uint64_t>(1'234'567ULL, dst.total_tx_bytes());
        Assert::AreEqual<std::uint64_t>(3ULL, dst.idle_disconnect_count());
        Assert::IsTrue(dst.server_mode() == ::fastport::protocols::admin::SERVER_MODE_IOCP);
        Assert::AreEqual<std::uint64_t>(128ULL * 1024 * 1024, dst.process_memory_bytes());
        Assert::AreEqual(4.25, dst.process_cpu_percent(), 1e-9);
        Assert::AreEqual<std::uint64_t>(1'700'000'000'999ULL, dst.server_timestamp_ms());
    }

    // AP-03: SessionListRequest 의 offset/limit 라운드트립.
    TEST_METHOD(SessionListRequest_OffsetLimit)
    {
        ::fastport::protocols::admin::AdminSessionListRequest src;
        src.mutable_header()->set_request_id(99);
        src.mutable_header()->set_timestamp_ms(1'700'000'000'555ULL);
        src.set_offset(250u);
        src.set_limit(128u);
        src.set_auth_token("");

        std::string wire;
        Assert::IsTrue(src.SerializeToString(&wire));

        ::fastport::protocols::admin::AdminSessionListRequest dst;
        Assert::IsTrue(dst.ParseFromString(wire));

        Assert::AreEqual<std::uint64_t>(99ULL, dst.header().request_id());
        Assert::AreEqual<std::uint32_t>(250u, dst.offset());
        Assert::AreEqual<std::uint32_t>(128u, dst.limit());
    }

    // AP-04: SessionListResponse 의 repeated sessions 필드 보존.
    TEST_METHOD(SessionListResponse_WithSessions)
    {
        ::fastport::protocols::admin::AdminSessionListResponse src;
        src.mutable_header()->set_request_id(5);
        src.set_result(::fastport::protocols::commons::RESULT_CODE_OK);
        src.set_total(100u);
        src.set_offset(10u);

        // 3개 세션 엔트리 추가.
        const std::uint64_t kIds[]  = { 1001ULL, 1002ULL, 1003ULL };
        const std::int64_t  kRecv[] = { 1234, 5678, -1 };
        const std::uint64_t kRx[]   = { 100ULL, 200ULL, 300ULL };
        const std::uint64_t kTx[]   = { 50ULL, 150ULL, 250ULL };
        for (int i = 0; i < 3; ++i)
        {
            auto* pInfo = src.add_sessions();
            pInfo->set_session_id(kIds[i]);
            pInfo->set_last_recv_ms(kRecv[i]);
            pInfo->set_rx_bytes(kRx[i]);
            pInfo->set_tx_bytes(kTx[i]);
        }

        std::string wire;
        Assert::IsTrue(src.SerializeToString(&wire));

        ::fastport::protocols::admin::AdminSessionListResponse dst;
        Assert::IsTrue(dst.ParseFromString(wire));

        Assert::AreEqual<std::uint32_t>(100u, dst.total());
        Assert::AreEqual<std::uint32_t>(10u, dst.offset());
        Assert::AreEqual(3, dst.sessions_size());

        for (int i = 0; i < 3; ++i)
        {
            const auto& s = dst.sessions(i);
            Assert::AreEqual<std::uint64_t>(kIds[i],  s.session_id());
            Assert::AreEqual<std::int64_t> (kRecv[i], s.last_recv_ms());
            Assert::AreEqual<std::uint64_t>(kRx[i],   s.rx_bytes());
            Assert::AreEqual<std::uint64_t>(kTx[i],   s.tx_bytes());
        }
    }

    // AP-05: ServerMode enum 3종 모두 정상 보존.
    TEST_METHOD(ServerMode_Enum_IOCP_RIO_Unknown)
    {
        using ResponseT = ::fastport::protocols::admin::AdminStatusSummaryResponse;
        const ::fastport::protocols::admin::ServerMode modes[] = {
            ::fastport::protocols::admin::SERVER_MODE_UNKNOWN,
            ::fastport::protocols::admin::SERVER_MODE_IOCP,
            ::fastport::protocols::admin::SERVER_MODE_RIO,
        };

        for (const auto mode : modes)
        {
            ResponseT src;
            src.set_server_mode(mode);

            std::string wire;
            Assert::IsTrue(src.SerializeToString(&wire));

            ResponseT dst;
            Assert::IsTrue(dst.ParseFromString(wire));
            Assert::IsTrue(dst.server_mode() == mode,
                L"ServerMode enum 값이 라운드트립에서 변형됨");
        }
    }
};

} // namespace LibNetworksTests
