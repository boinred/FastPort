// StatsSampler.cpp
// -----------------------------------------------------------------------------
// Design Ref: server-status §4.1, §2.2 — CPU/Memory 주기 샘플 구현.
// GetProcessTimes: kernel + user time (100ns 단위)의 델타를 wall-clock 델타로 나눠 % 산출
// (논리 코어 전체 기준이므로 100% = 전체 코어 풀가동 기준으로 나타남. 정규화 없이
//  OS 표준 관례 따름. 필요 시 호출자가 `/ processorCount` 로 정규화).
// GetProcessMemoryInfo: WorkingSetSize (실제 상주 메모리 바이트).
// -----------------------------------------------------------------------------
module;

#include <Windows.h>
#include <Psapi.h>
#include <cstdint>
#include <spdlog/spdlog.h>

module networks.stats.stats_sampler;

import std;
import commons.logger;
import commons.timer_queue;


namespace LibNetworks::Stats
{

namespace
{
constexpr const char* kLogCategory = "StatsSampler";

inline void LogInfo(const std::string& msg)
{
    LibCommons::Logger::GetInstance().LogInfo(kLogCategory, msg);
}
inline void LogError(const std::string& msg)
{
    LibCommons::Logger::GetInstance().LogError(kLogCategory, msg);
}

// 100ns → uint64 변환 유틸.
inline std::uint64_t FileTimeToU64(const FILETIME& ft) noexcept
{
    ULARGE_INTEGER ul{};
    ul.LowPart  = ft.dwLowDateTime;
    ul.HighPart = ft.dwHighDateTime;
    return static_cast<std::uint64_t>(ul.QuadPart);
}

} // anonymous namespace


StatsSampler::StatsSampler(SamplerConfig cfg)
    : m_Config(cfg)
{
}


StatsSampler::~StatsSampler()
{
    Stop();
}


void StatsSampler::Start()
{
    if (!m_Config.enabled)
    {
        LogInfo("Disabled, skip scheduling");
        return;
    }

    bool expected = false;
    if (!m_Running.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return;
    }

    // 첫 샘플은 즉시 수행하여 이후 CPU% 계산 기준점 확보.
    DoSample();

    auto& tq = LibCommons::TimerQueue::GetInstance();
    const auto id = tq.SchedulePeriodic(
        m_Config.tickIntervalMs,
        [this]() { this->OnTick(); },
        "StatsSampler");

    m_TimerId.store(static_cast<std::uint64_t>(id), std::memory_order_release);

    LogInfo(std::format("Started. TickIntervalMs : {}", m_Config.tickIntervalMs.count()));
}


void StatsSampler::Stop()
{
    bool wasRunning = m_Running.exchange(false, std::memory_order_acq_rel);
    if (!wasRunning)
    {
        return;
    }

    const auto id = static_cast<LibCommons::TimerId>(
        m_TimerId.exchange(0, std::memory_order_acq_rel));

    if (id != LibCommons::kInvalidTimerId)
    {
        LibCommons::TimerQueue::GetInstance().Cancel(id);
    }

    LogInfo("Stopped");
}


double StatsSampler::SnapshotCpuPercent() const noexcept
{
    return m_CpuPercentCache.load(std::memory_order_relaxed);
}


std::uint64_t StatsSampler::SnapshotMemoryBytes() const noexcept
{
    return m_MemoryBytesCache.load(std::memory_order_relaxed);
}


void StatsSampler::ForceSampleNow()
{
    DoSample();
}


void StatsSampler::OnTick()
{
    if (!m_Running.load(std::memory_order_acquire))
    {
        return;
    }

    try
    {
        DoSample();
    }
    catch (const std::exception& e)
    {
        LogError(std::format("DoSample threw: {}", e.what()));
    }
}


// Windows GetProcessTimes/GetProcessMemoryInfo 호출 + 캐시 갱신.
void StatsSampler::DoSample()
{
    HANDLE hProc = ::GetCurrentProcess();

    FILETIME creationTime{}, exitTime{}, kernelTime{}, userTime{};
    if (!::GetProcessTimes(hProc, &creationTime, &exitTime, &kernelTime, &userTime))
    {
        LogError(std::format("GetProcessTimes failed. GLE : {}", ::GetLastError()));
        return;
    }

    const std::uint64_t currProcKernel = FileTimeToU64(kernelTime);
    const std::uint64_t currProcUser   = FileTimeToU64(userTime);

    FILETIME sysNow{};
    ::GetSystemTimeAsFileTime(&sysNow);
    const std::uint64_t currWall = FileTimeToU64(sysNow);

    double cpuPct = 0.0;
    if (m_HasPrevSample)
    {
        const std::uint64_t procDelta = (currProcKernel + currProcUser)
                                      - (m_PrevProcKernelTicks + m_PrevProcUserTicks);
        const std::uint64_t wallDelta = currWall - m_PrevWallTicks;
        if (wallDelta > 0)
        {
            // 논리 코어 전체 기준(OS 관례): 100% == 1 core 풀가동.
            cpuPct = (static_cast<double>(procDelta) / static_cast<double>(wallDelta)) * 100.0;
            if (cpuPct < 0.0) cpuPct = 0.0;
        }
    }

    m_PrevProcKernelTicks = currProcKernel;
    m_PrevProcUserTicks   = currProcUser;
    m_PrevWallTicks       = currWall;
    m_HasPrevSample       = true;

    m_CpuPercentCache.store(cpuPct, std::memory_order_relaxed);

    // Memory: WorkingSetSize
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(hProc,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc)))
    {
        m_MemoryBytesCache.store(static_cast<std::uint64_t>(pmc.WorkingSetSize),
            std::memory_order_relaxed);
    }
}

} // namespace LibNetworks::Stats
