#include "CppUnitTest.h"

import networks.sessions.idle_checker;
import networks.sessions.iidle_aware;
import networks.sessions.inetwork_session;  // DisconnectReason
import commons.logger;
import commons.timer_queue;
import std;

// Design Ref: session-idle-timeout §8.3 — SessionIdleChecker 단위 테스트 (I-01 ~ I-08).
// Mock IIdleAware 로 IdleChecker 격리. TimerQueue 는 실제 인스턴스(싱글톤) 사용.

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std::chrono_literals;

namespace LibNetworksTests
{

// Mock IIdleAware — GetLastRecvTimeMs 와 RequestDisconnect 호출 수/사유 기록.
struct MockIdleAware : public LibNetworks::Sessions::IIdleAware
{
    std::atomic<std::int64_t>                              lastRecvMs      { 0 };
    std::atomic<int>                                       disconnectCount { 0 };
    std::atomic<LibNetworks::Sessions::DisconnectReason>   lastReason      { LibNetworks::Sessions::DisconnectReason::Normal };
    bool                                                   throwOnDisconnect { false };

    std::int64_t GetLastRecvTimeMs() const noexcept override
    {
        return lastRecvMs.load(std::memory_order_relaxed);
    }

    void RequestDisconnect(LibNetworks::Sessions::DisconnectReason reason) override
    {
        disconnectCount.fetch_add(1, std::memory_order_relaxed);
        lastReason.store(reason, std::memory_order_relaxed);
        if (throwOnDisconnect)
        {
            throw std::runtime_error("mock: disconnect throw");
        }
    }
};


namespace
{
// steady_clock 기준 epoch-ms (IdleChecker 내부 NowMs 와 동일 기준).
std::int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // anonymous namespace


TEST_MODULE_INITIALIZE(InitLogger)
{
    LibCommons::Logger::GetInstance().Create(
        "_test_logs",
        "session_idle_checker_tests.log",
        10 * 1024 * 1024,
        3,
        /*bServiceMode=*/false);
}

TEST_MODULE_CLEANUP(ShutdownLogger)
{
    // TimerQueue 먼저 (잔여 tick 이 로그 접근하지 않게), 그 다음 Logger.
    if (auto* pTimerQueue = LibCommons::TimerQueue::TryGetInstance())
    {
        pTimerQueue->Shutdown(/*waitForCallbacks=*/true);
    }

    if (auto* pLogger = LibCommons::Logger::TryGetInstance())
    {
        pLogger->Shutdown();
    }
}


TEST_CLASS(SessionIdleCheckerTests)
{
public:

    // I-01: enabled=false → tick 안 돌고 disconnect 0.
    TEST_METHOD(Start_WithEnabledFalse_NeverTicks)
    {
        auto mockA = std::make_shared<MockIdleAware>();
        auto mockB = std::make_shared<MockIdleAware>();
        mockA->lastRecvMs.store(NowMs() - 10'000);  // stale 하게 설정
        mockB->lastRecvMs.store(NowMs() - 10'000);

        LibNetworks::Sessions::IdleCheckerConfig cfg;
        cfg.thresholdMs    = 100ms;
        cfg.tickIntervalMs = 50ms;
        cfg.enabled        = false;

        LibNetworks::Sessions::SessionIdleChecker checker(cfg,
            [mockA, mockB]() -> std::vector<std::shared_ptr<LibNetworks::Sessions::IIdleAware>> {
                return { mockA, mockB };
            });

        checker.Start();
        std::this_thread::sleep_for(500ms);

        Assert::AreEqual(0, mockA->disconnectCount.load(), L"enabled=false 면 tick 이 돌면 안 됨");
        Assert::AreEqual(0, mockB->disconnectCount.load());

        checker.Stop();
    }

    // I-02: 정상 트래픽 (lastRecv 주기적 갱신) → 오탐 없음.
    TEST_METHOD(Start_WithFreshSession_NoDisconnect)
    {
        auto mock = std::make_shared<MockIdleAware>();
        mock->lastRecvMs.store(NowMs());

        std::atomic<bool> running { true };

        // 별도 스레드로 lastRecv 를 30ms 주기로 갱신 (threshold 200ms 보다 훨씬 짧게).
        std::thread trafficThread([&]() {
            while (running.load()) {
                mock->lastRecvMs.store(NowMs());
                std::this_thread::sleep_for(30ms);
            }
        });

        LibNetworks::Sessions::IdleCheckerConfig cfg;
        cfg.thresholdMs    = 200ms;
        cfg.tickIntervalMs = 50ms;

        LibNetworks::Sessions::SessionIdleChecker checker(cfg,
            [mock]() -> std::vector<std::shared_ptr<LibNetworks::Sessions::IIdleAware>> {
                return { mock };
            });

        checker.Start();
        std::this_thread::sleep_for(500ms);
        checker.Stop();

        running.store(false);
        trafficThread.join();

        Assert::AreEqual(0, mock->disconnectCount.load(),
            L"정상 트래픽 세션은 disconnect 되면 안 됨");
    }

    // I-03: stale 세션은 threshold + tick 이내 disconnect 호출.
    TEST_METHOD(Start_WithStaleSession_DisconnectsWithinBudget)
    {
        auto mock = std::make_shared<MockIdleAware>();
        mock->lastRecvMs.store(NowMs() - 1'000);  // 1초 전 — 확실히 stale

        LibNetworks::Sessions::IdleCheckerConfig cfg;
        cfg.thresholdMs    = 200ms;
        cfg.tickIntervalMs = 50ms;

        LibNetworks::Sessions::SessionIdleChecker checker(cfg,
            [mock]() -> std::vector<std::shared_ptr<LibNetworks::Sessions::IIdleAware>> {
                return { mock };
            });

        checker.Start();
        std::this_thread::sleep_for(500ms);  // threshold + tick 충분히 넘김
        checker.Stop();

        Assert::IsTrue(mock->disconnectCount.load() >= 1,
            L"stale 세션은 최소 1회 disconnect 되어야 함");
        Assert::IsTrue(mock->lastReason.load() == LibNetworks::Sessions::DisconnectReason::IdleTimeout,
            L"사유는 IdleTimeout 이어야 함");
    }

    // I-04: 여러 세션 중 stale 만 disconnect 대상.
    TEST_METHOD(MultipleSessions_OnlyStaleDisconnected)
    {
        auto stale  = std::make_shared<MockIdleAware>();
        auto fresh1 = std::make_shared<MockIdleAware>();
        auto fresh2 = std::make_shared<MockIdleAware>();

        const auto now = NowMs();
        stale->lastRecvMs.store(now - 1'000);
        fresh1->lastRecvMs.store(now);
        fresh2->lastRecvMs.store(now);

        std::atomic<bool> running { true };
        std::thread keeper([&]() {
            while (running.load()) {
                fresh1->lastRecvMs.store(NowMs());
                fresh2->lastRecvMs.store(NowMs());
                std::this_thread::sleep_for(30ms);
            }
        });

        LibNetworks::Sessions::IdleCheckerConfig cfg;
        cfg.thresholdMs    = 200ms;
        cfg.tickIntervalMs = 50ms;

        LibNetworks::Sessions::SessionIdleChecker checker(cfg,
            [stale, fresh1, fresh2]() {
                std::vector<std::shared_ptr<LibNetworks::Sessions::IIdleAware>> v;
                v.push_back(stale);
                v.push_back(fresh1);
                v.push_back(fresh2);
                return v;
            });

        checker.Start();
        std::this_thread::sleep_for(500ms);
        checker.Stop();

        running.store(false);
        keeper.join();

        Assert::IsTrue(stale->disconnectCount.load() >= 1, L"stale 는 disconnect 되어야");
        Assert::AreEqual(0, fresh1->disconnectCount.load(), L"fresh1 는 안 됨");
        Assert::AreEqual(0, fresh2->disconnectCount.load(), L"fresh2 는 안 됨");
    }

    // I-05: lastRecvMs=0 (수신 이력 없음) → skip, disconnect 안 함.
    TEST_METHOD(LastRecvZero_Skipped)
    {
        auto mock = std::make_shared<MockIdleAware>();
        mock->lastRecvMs.store(0);  // 명시적 0

        LibNetworks::Sessions::IdleCheckerConfig cfg;
        cfg.thresholdMs    = 10ms;   // 극단적으로 짧게
        cfg.tickIntervalMs = 20ms;

        LibNetworks::Sessions::SessionIdleChecker checker(cfg,
            [mock]() -> std::vector<std::shared_ptr<LibNetworks::Sessions::IIdleAware>> {
                return { mock };
            });

        checker.Start();
        std::this_thread::sleep_for(200ms);
        checker.Stop();

        Assert::AreEqual(0, mock->disconnectCount.load(),
            L"lastRecvMs=0 은 '수신 이력 없음' 의미 → skip 되어야 함");
    }

    // I-06: Stop 이후 추가 tick 없음.
    TEST_METHOD(Stop_PreventsFurtherTicks)
    {
        auto mock = std::make_shared<MockIdleAware>();
        mock->lastRecvMs.store(NowMs() - 10'000);  // stale

        LibNetworks::Sessions::IdleCheckerConfig cfg;
        cfg.thresholdMs    = 100ms;
        cfg.tickIntervalMs = 50ms;

        LibNetworks::Sessions::SessionIdleChecker checker(cfg,
            [mock]() -> std::vector<std::shared_ptr<LibNetworks::Sessions::IIdleAware>> {
                return { mock };
            });

        checker.Start();
        std::this_thread::sleep_for(300ms);
        const int beforeStop = mock->disconnectCount.load();
        checker.Stop();

        std::this_thread::sleep_for(300ms);
        const int afterStop = mock->disconnectCount.load();

        Assert::AreEqual(beforeStop, afterStop,
            L"Stop 이후 disconnect 카운트가 증가하면 안 됨");
    }

    // I-07: Provider 예외 → catch, checker 생존.
    TEST_METHOD(ProviderException_LoggedNotFatal)
    {
        std::atomic<int> providerCalls { 0 };

        LibNetworks::Sessions::IdleCheckerConfig cfg;
        cfg.thresholdMs    = 100ms;
        cfg.tickIntervalMs = 50ms;

        LibNetworks::Sessions::SessionIdleChecker checker(cfg,
            [&providerCalls]() -> std::vector<std::shared_ptr<LibNetworks::Sessions::IIdleAware>> {
                providerCalls.fetch_add(1);
                throw std::runtime_error("intentional provider error");
            });

        checker.Start();
        std::this_thread::sleep_for(300ms);
        checker.Stop();

        // 예외에도 불구하고 provider 가 여러 번 호출되어야 (tick 이 계속 돈 증거).
        Assert::IsTrue(providerCalls.load() >= 2,
            L"provider 가 예외 던져도 이후 tick 에 재호출되어야 함");
    }

    // I-08: RequestDisconnect 예외 → 다음 세션 계속 처리.
    TEST_METHOD(RequestDisconnectException_LoggedContinues)
    {
        auto throwingMock = std::make_shared<MockIdleAware>();
        auto normalMock   = std::make_shared<MockIdleAware>();

        const auto now = NowMs();
        throwingMock->lastRecvMs.store(now - 1'000);
        throwingMock->throwOnDisconnect = true;
        normalMock->lastRecvMs.store(now - 1'000);

        LibNetworks::Sessions::IdleCheckerConfig cfg;
        cfg.thresholdMs    = 100ms;
        cfg.tickIntervalMs = 50ms;

        LibNetworks::Sessions::SessionIdleChecker checker(cfg,
            [throwingMock, normalMock]() {
                std::vector<std::shared_ptr<LibNetworks::Sessions::IIdleAware>> v;
                v.push_back(throwingMock);
                v.push_back(normalMock);
                return v;
            });

        checker.Start();
        std::this_thread::sleep_for(300ms);
        checker.Stop();

        // throwingMock 도 disconnect 호출은 받음 (그 안에서 throw).
        Assert::IsTrue(throwingMock->disconnectCount.load() >= 1);
        // 정상 mock 도 처리됨 (throwing 에서 예외가 전체 loop 를 중단시키면 안 됨).
        Assert::IsTrue(normalMock->disconnectCount.load() >= 1,
            L"한 세션의 예외가 다른 세션 처리를 막으면 안 됨");
    }
};

} // namespace LibNetworksTests
