// Design Ref: §3.4 — MetricsCollector (atomic counters + p50/p95/p99)
// Plan SC: SC2 (실시간 차트 렌더링)
module;

#include <atomic>
#include <array>
#include <vector>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <cmath>

export module test_client.metrics_collector;

export class MetricsCollector
{
public:
    struct Snapshot
    {
        uint64_t TotalMessages = 0;
        uint32_t ActiveConnections = 0;
        double MsgPerSec = 0.0;
        double AvgLatencyMs = 0.0;
        double P50LatencyMs = 0.0;
        double P95LatencyMs = 0.0;
        double P99LatencyMs = 0.0;
    };

    // IO 스레드에서 호출 (atomic, lock-free)
    void RecordLatency(double rttMs)
    {
        size_t idx = m_SampleHead.fetch_add(1, std::memory_order_relaxed) % MAX_SAMPLES;
        m_LatencySamples[idx] = static_cast<float>(rttMs);
        m_SampleCount.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordMessage()
    {
        m_TotalMessages.fetch_add(1, std::memory_order_relaxed);
        m_IntervalMessages.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordConnection(bool connected)
    {
        if (connected)
            m_ActiveConnections.fetch_add(1, std::memory_order_relaxed);
        else if (m_ActiveConnections.load(std::memory_order_relaxed) > 0)
            m_ActiveConnections.fetch_sub(1, std::memory_order_relaxed);
    }

    // GUI 스레드에서 매 프레임 호출
    Snapshot GetSnapshot()
    {
        Snapshot snap;
        snap.TotalMessages = m_TotalMessages.load(std::memory_order_relaxed);
        snap.ActiveConnections = m_ActiveConnections.load(std::memory_order_relaxed);

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_LastUpdate).count();
        if (elapsed >= 1.0)
        {
            uint64_t intervalMsgs = m_IntervalMessages.exchange(0, std::memory_order_relaxed);
            m_CurrentMsgPerSec = static_cast<double>(intervalMsgs) / elapsed;
            UpdatePercentiles();

            // 롤링 히스토리 갱신
            {
                std::lock_guard lock(m_HistoryMutex);
                m_LatencyHistory.push_back(static_cast<float>(m_CurrentP50));
                m_ThroughputHistory.push_back(static_cast<float>(m_CurrentMsgPerSec));
                if (m_LatencyHistory.size() > 60) m_LatencyHistory.erase(m_LatencyHistory.begin());
                if (m_ThroughputHistory.size() > 60) m_ThroughputHistory.erase(m_ThroughputHistory.begin());
            }

            m_LastUpdate = now;
        }

        snap.MsgPerSec = m_CurrentMsgPerSec;
        snap.AvgLatencyMs = m_CurrentAvg;
        snap.P50LatencyMs = m_CurrentP50;
        snap.P95LatencyMs = m_CurrentP95;
        snap.P99LatencyMs = m_CurrentP99;
        return snap;
    }

    // 차트 데이터 접근 (GUI 스레드에서만)
    void GetLatencyHistory(std::vector<float>& out) const
    {
        std::lock_guard lock(m_HistoryMutex);
        out = m_LatencyHistory;
    }

    void GetThroughputHistory(std::vector<float>& out) const
    {
        std::lock_guard lock(m_HistoryMutex);
        out = m_ThroughputHistory;
    }

    void Reset()
    {
        m_TotalMessages = 0;
        m_IntervalMessages = 0;
        m_ActiveConnections = 0;
        m_SampleHead = 0;
        m_SampleCount = 0;
        m_CurrentMsgPerSec = 0;
        m_CurrentAvg = 0;
        m_CurrentP50 = 0;
        m_CurrentP95 = 0;
        m_CurrentP99 = 0;
        std::lock_guard lock(m_HistoryMutex);
        m_LatencyHistory.clear();
        m_ThroughputHistory.clear();
    }

private:
    void UpdatePercentiles()
    {
        size_t count = m_SampleCount.load(std::memory_order_relaxed);
        if (count == 0) return;

        size_t n = (count < MAX_SAMPLES) ? count : MAX_SAMPLES;
        std::vector<float> sorted(m_LatencySamples.begin(), m_LatencySamples.begin() + n);
        std::sort(sorted.begin(), sorted.end());

        double sum = 0;
        for (auto v : sorted) sum += v;
        m_CurrentAvg = sum / n;
        m_CurrentP50 = sorted[n * 50 / 100];
        m_CurrentP95 = sorted[n * 95 / 100];
        m_CurrentP99 = sorted[n * 99 / 100];

        // 샘플 카운터 리셋 (다음 인터벌용)
        m_SampleCount = 0;
        m_SampleHead = 0;
    }

    static constexpr size_t MAX_SAMPLES = 10000;
    std::array<float, MAX_SAMPLES> m_LatencySamples{};
    std::atomic<size_t> m_SampleHead = 0;
    std::atomic<size_t> m_SampleCount = 0;

    std::atomic<uint64_t> m_TotalMessages = 0;
    std::atomic<uint64_t> m_IntervalMessages = 0;
    std::atomic<uint32_t> m_ActiveConnections = 0;

    double m_CurrentMsgPerSec = 0;
    double m_CurrentAvg = 0;
    double m_CurrentP50 = 0;
    double m_CurrentP95 = 0;
    double m_CurrentP99 = 0;

    std::chrono::steady_clock::time_point m_LastUpdate = std::chrono::steady_clock::now();

    mutable std::mutex m_HistoryMutex;
    std::vector<float> m_LatencyHistory;
    std::vector<float> m_ThroughputHistory;
};
