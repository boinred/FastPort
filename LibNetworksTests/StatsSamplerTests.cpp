// StatsSamplerTests.cpp
// -----------------------------------------------------------------------------
// Design Ref: server-status §8.5 — StatsSampler CPU/Memory 샘플 단위 테스트 (SS-01 ~ SS-04).
// GetProcessTimes / GetProcessMemoryInfo 를 호출하는 실 테스트. Windows 전용.
// TimerQueue 는 싱글톤 (SessionIdleCheckerTests 에서 공유) — 동일 TEST_MODULE 생명주기.
// -----------------------------------------------------------------------------
#include "CppUnitTest.h"

#include <thread>
#include <chrono>
#include <cstdint>

import networks.stats.stats_sampler;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std::chrono_literals;

namespace LibNetworksTests
{

TEST_CLASS(StatsSamplerTests)
{
public:

    // SS-01: 샘플러 생성 직후, Start/ForceSampleNow 미호출이면 CPU 캐시는 0.0.
    // (생성자에서는 OS API 를 건드리지 않음.)
    TEST_METHOD(Sampler_InitialSample_ZeroCpu)
    {
        LibNetworks::Stats::SamplerConfig cfg;
        cfg.enabled = true;
        LibNetworks::Stats::StatsSampler sampler(cfg);

        // Start/ForceSampleNow 전에는 캐시가 초기값(0.0).
        Assert::AreEqual(0.0, sampler.SnapshotCpuPercent(), 1e-9,
            L"Start 전에는 CPU 캐시가 0.0 이어야 함");
        Assert::AreEqual<std::uint64_t>(0ULL, sampler.SnapshotMemoryBytes(),
            L"Start 전에는 Memory 캐시가 0 이어야 함");
    }

    // SS-02: Start 는 내부적으로 첫 샘플을 즉시 수행하므로 SnapshotMemoryBytes > 0.
    TEST_METHOD(Sampler_AfterStart_MemoryPositive)
    {
        LibNetworks::Stats::SamplerConfig cfg;
        cfg.tickIntervalMs = 1000ms;
        cfg.enabled        = true;
        LibNetworks::Stats::StatsSampler sampler(cfg);

        sampler.Start();

        // Start 내부에서 DoSample 이 즉시 한 번 호출됨 → Memory 캐시 채워짐.
        const auto mem = sampler.SnapshotMemoryBytes();
        Assert::IsTrue(mem > 0,
            L"Start 후 Memory 캐시가 양수여야 함 (GetProcessMemoryInfo 결과)");

        sampler.Stop();
    }

    // SS-03: Stop 후 ForceSampleNow 는 여전히 유효 (명시적 강제 샘플 용도).
    // 하지만 Stop 으로 periodic tick 은 멈춤 — 카운터 간접 확인은 어려우므로
    // Stop 이후 ForceSampleNow 가 crash 없이 동작 + 캐시가 갱신되는지만 검증.
    TEST_METHOD(Sampler_Stop_ForceSampleStillWorks)
    {
        LibNetworks::Stats::SamplerConfig cfg;
        cfg.tickIntervalMs = 1000ms;
        cfg.enabled        = true;
        LibNetworks::Stats::StatsSampler sampler(cfg);

        sampler.Start();
        const auto memAfterStart = sampler.SnapshotMemoryBytes();
        Assert::IsTrue(memAfterStart > 0);

        sampler.Stop();

        // Stop 이후 ForceSampleNow 는 예외 없이 호출 가능 — production 은 사용하지 않지만
        // 테스트/디버깅 경로에서 명시적 호출은 허용되어야 함.
        sampler.ForceSampleNow();

        const auto memAfterForce = sampler.SnapshotMemoryBytes();
        Assert::IsTrue(memAfterForce > 0,
            L"ForceSampleNow 이후에도 Memory 캐시는 양수여야 함");
    }

    // SS-04: enabled=false 이면 Start 가 no-op. 이후 tick 은 발생하지 않으며 캐시는 초기값.
    TEST_METHOD(Sampler_Disabled_NeverTicks)
    {
        LibNetworks::Stats::SamplerConfig cfg;
        cfg.tickIntervalMs = 50ms;
        cfg.enabled        = false;
        LibNetworks::Stats::StatsSampler sampler(cfg);

        sampler.Start();
        std::this_thread::sleep_for(200ms);

        Assert::AreEqual(0.0, sampler.SnapshotCpuPercent(), 1e-9,
            L"enabled=false 이면 CPU 샘플이 발생하면 안 됨");
        Assert::AreEqual<std::uint64_t>(0ULL, sampler.SnapshotMemoryBytes(),
            L"enabled=false 이면 Memory 샘플이 발생하면 안 됨");

        sampler.Stop();
    }
};

} // namespace LibNetworksTests
