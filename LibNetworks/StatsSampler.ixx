// StatsSampler.ixx
// -----------------------------------------------------------------------------
// Design Ref: server-status §4.1 — OS 프로세스 메트릭(CPU/Memory) 주기 샘플.
// TimerQueue 로 1Hz (기본) 주기 tick. GetProcessTimes 델타 계산으로 CPU%, GetProcessMemoryInfo
// 로 WorkingSetSize. 요청마다 OS API 호출을 피하기 위해 결과를 atomic 캐시로 보관.
// -----------------------------------------------------------------------------
module;

#include <cstdint>

export module networks.stats.stats_sampler;

import std;

namespace LibNetworks::Stats
{

export struct SamplerConfig
{
    std::chrono::milliseconds tickIntervalMs { 1000 };
    bool                      enabled        { true };
};

export class StatsSampler
{
public:
    explicit StatsSampler(SamplerConfig cfg = {});
    ~StatsSampler();

    StatsSampler(const StatsSampler&)            = delete;
    StatsSampler& operator=(const StatsSampler&) = delete;

    // TimerQueue 에 Periodic tick 등록. enabled=false 면 no-op.
    void Start();

    // 진행 중 tick 완료 대기 후 정리. idempotent.
    void Stop();

    // 최근 캐시된 값 (OS API 호출 없이 반환).
    double        SnapshotCpuPercent() const noexcept;
    std::uint64_t SnapshotMemoryBytes() const noexcept;

    // 즉시 샘플 강제 (테스트용). tick 과 race 가능하므로 테스트에서만 사용.
    void ForceSampleNow();

    const SamplerConfig& GetConfig() const noexcept { return m_Config; }

private:
    void OnTick();
    void DoSample();

    SamplerConfig              m_Config;
    std::atomic<std::uint64_t> m_TimerId           { 0 };
    std::atomic<bool>          m_Running           { false };

    // CPU 계산용 이전 샘플(스레드 전용 — tick 콜백 단일 스레드).
    std::uint64_t              m_PrevProcKernelTicks { 0 };
    std::uint64_t              m_PrevProcUserTicks   { 0 };
    std::uint64_t              m_PrevWallTicks       { 0 };
    bool                       m_HasPrevSample     { false };

    // 최근 캐시 (요청 경로에서 lock-free read).
    std::atomic<double>        m_CpuPercentCache   { 0.0 };
    std::atomic<std::uint64_t> m_MemoryBytesCache  { 0 };
};

} // namespace LibNetworks::Stats
