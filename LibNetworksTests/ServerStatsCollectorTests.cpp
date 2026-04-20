// ServerStatsCollectorTests.cpp
// -----------------------------------------------------------------------------
// Design Ref: server-status §8.4 — ServerStatsCollector 집계 단위 테스트 (SC-01 ~ SC-08).
// Mock ISessionStats 로 세션 데이터 주입. 실제 세션/소켓 없이 집계 로직만 검증.
// StatsSampler 는 nullptr 로 주입 (CPU/Memory 경로는 별도 테스트에서 커버).
// -----------------------------------------------------------------------------
#include "CppUnitTest.h"

#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <cstdint>

import networks.stats.server_stats_collector;
import networks.stats.stats_sampler;
import networks.sessions.isession_stats;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std::chrono_literals;

namespace LibNetworksTests
{

// Mock ISessionStats — rx/tx 바이트를 고정값으로 반환.
// Collector 의 SC-08 (OffsetOverflow) 처리 상 총 세션 수 집계에만 기여.
struct MockSessionStats : public LibNetworks::Sessions::ISessionStats
{
    std::uint64_t rx = 0;
    std::uint64_t tx = 0;

    std::uint64_t GetTotalRxBytes() const noexcept override { return rx; }
    std::uint64_t GetTotalTxBytes() const noexcept override { return tx; }
};


namespace
{
// 헬퍼: N 개의 Mock 을 rx/tx = i*100 으로 초기화하여 반환.
std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>>
MakeMocks(std::uint32_t count, std::uint64_t rxBase = 100, std::uint64_t txBase = 50)
{
    std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        auto pMock = std::make_shared<MockSessionStats>();
        pMock->rx = rxBase * (static_cast<std::uint64_t>(i) + 1);
        pMock->tx = txBase * (static_cast<std::uint64_t>(i) + 1);
        out.push_back(pMock);
    }
    return out;
}
} // anonymous namespace


TEST_CLASS(ServerStatsCollectorTests)
{
public:

    // SC-01: 비어있는 provider → activeSessionCount=0, totalRx=totalTx=0.
    TEST_METHOD(Summary_EmptyProvider)
    {
        LibNetworks::Stats::ServerStatsCollector collector(
            LibNetworks::Stats::ServerMode::IOCP,
            []() { return std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>>{}; },
            []() -> std::uint64_t { return 0ULL; },
            /*pSampler=*/nullptr);

        const auto summary = collector.SnapshotSummary();

        Assert::AreEqual<std::uint32_t>(0u, summary.activeSessionCount);
        Assert::AreEqual<std::uint64_t>(0ULL, summary.totalRxBytes);
        Assert::AreEqual<std::uint64_t>(0ULL, summary.totalTxBytes);
        Assert::IsTrue(summary.serverMode == LibNetworks::Stats::ServerMode::IOCP);
    }

    // SC-02: 3 Mock (rx 100/200/300, tx 50/100/150) → 합 600 / 300.
    TEST_METHOD(Summary_ThreeSessions_Aggregated)
    {
        auto mocks = MakeMocks(3, /*rxBase=*/100, /*txBase=*/50);

        LibNetworks::Stats::ServerStatsCollector collector(
            LibNetworks::Stats::ServerMode::RIO,
            [mocks]() { return mocks; },
            []() -> std::uint64_t { return 0ULL; },
            nullptr);

        const auto summary = collector.SnapshotSummary();

        Assert::AreEqual<std::uint32_t>(3u, summary.activeSessionCount);
        Assert::AreEqual<std::uint64_t>(100ULL + 200ULL + 300ULL, summary.totalRxBytes);
        Assert::AreEqual<std::uint64_t>(50ULL + 100ULL + 150ULL,  summary.totalTxBytes);
        Assert::IsTrue(summary.serverMode == LibNetworks::Stats::ServerMode::RIO);
    }

    // SC-03: ctor 후 일정 sleep → uptimeMs 가 해당 시간 이상.
    TEST_METHOD(Summary_UptimeIncreasesOverTime)
    {
        LibNetworks::Stats::ServerStatsCollector collector(
            LibNetworks::Stats::ServerMode::IOCP,
            []() { return std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>>{}; },
            []() -> std::uint64_t { return 0ULL; },
            nullptr);

        std::this_thread::sleep_for(120ms);

        const auto summary = collector.SnapshotSummary();
        Assert::IsTrue(summary.uptimeMs >= 100,
            L"120ms sleep 후 uptimeMs 가 100 이상이어야 함");
    }

    // SC-04: IdleCountProvider 가 반환한 값이 idleDisconnectCount 에 그대로 반영.
    TEST_METHOD(Summary_IdleCountFromProvider)
    {
        LibNetworks::Stats::ServerStatsCollector collector(
            LibNetworks::Stats::ServerMode::IOCP,
            []() { return std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>>{}; },
            []() -> std::uint64_t { return 42ULL; },
            nullptr);

        const auto summary = collector.SnapshotSummary();
        Assert::AreEqual<std::uint64_t>(42ULL, summary.idleDisconnectCount);
    }

    // SC-05: Sampler = nullptr → CPU=0, Memory=0 (기본값 유지).
    TEST_METHOD(Summary_NoSampler_CpuMemoryZero)
    {
        LibNetworks::Stats::ServerStatsCollector collector(
            LibNetworks::Stats::ServerMode::IOCP,
            []() { return std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>>{}; },
            []() -> std::uint64_t { return 0ULL; },
            /*pSampler=*/nullptr);

        const auto summary = collector.SnapshotSummary();
        Assert::AreEqual(0.0, summary.processCpuPercent, 1e-9);
        Assert::AreEqual<std::uint64_t>(0ULL, summary.processMemoryBytes);
    }

    // SC-06: 5 sessions, offset=1, limit=2 → sessions=2개, total=5.
    TEST_METHOD(SessionList_OffsetLimit_Paging)
    {
        auto mocks = MakeMocks(5, 100, 50);

        LibNetworks::Stats::ServerStatsCollector collector(
            LibNetworks::Stats::ServerMode::IOCP,
            [mocks]() { return mocks; },
            []() -> std::uint64_t { return 0ULL; },
            nullptr);

        const auto list = collector.SnapshotSessions(/*offset=*/1, /*limit=*/2);

        Assert::AreEqual<std::uint32_t>(5u, list.total);
        Assert::AreEqual<std::uint32_t>(1u, list.offset);
        Assert::AreEqual(static_cast<size_t>(2), list.sessions.size());

        // offset=1 이므로 두 번째/세 번째 mock (rx=200, 300).
        Assert::AreEqual<std::uint64_t>(200ULL, list.sessions[0].rxBytes);
        Assert::AreEqual<std::uint64_t>(300ULL, list.sessions[1].rxBytes);
    }

    // SC-07: limit 이 kMaxLimit(1000) 을 초과하면 1000 으로 clamp.
    // 1200 개 mock 으로 실제 clamp 동작 검증.
    TEST_METHOD(SessionList_LimitClamp)
    {
        auto mocks = MakeMocks(1200, 100, 50);

        LibNetworks::Stats::ServerStatsCollector collector(
            LibNetworks::Stats::ServerMode::IOCP,
            [mocks]() { return mocks; },
            []() -> std::uint64_t { return 0ULL; },
            nullptr);

        const auto list = collector.SnapshotSessions(/*offset=*/0, /*limit=*/5000);

        Assert::AreEqual<std::uint32_t>(1200u, list.total);
        Assert::AreEqual(
            static_cast<size_t>(LibNetworks::Stats::ServerStatsCollector::kMaxLimit),
            list.sessions.size(),
            L"limit=5000 은 kMaxLimit(1000) 로 clamp 되어야 함");
    }

    // SC-08: offset 이 total 을 초과하면 sessions 는 비어있고 total 은 정확히 보고.
    TEST_METHOD(SessionList_OffsetOverflow_EmptyResult)
    {
        auto mocks = MakeMocks(5, 100, 50);

        LibNetworks::Stats::ServerStatsCollector collector(
            LibNetworks::Stats::ServerMode::IOCP,
            [mocks]() { return mocks; },
            []() -> std::uint64_t { return 0ULL; },
            nullptr);

        const auto list = collector.SnapshotSessions(/*offset=*/100, /*limit=*/10);

        Assert::AreEqual<std::uint32_t>(5u, list.total);
        Assert::AreEqual<std::uint32_t>(100u, list.offset);
        Assert::IsTrue(list.sessions.empty(),
            L"offset >= total 이면 sessions 는 비어있어야 함");
    }
};

} // namespace LibNetworksTests
