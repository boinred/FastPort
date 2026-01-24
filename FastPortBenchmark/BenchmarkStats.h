#pragma once

#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdint>

namespace FastPortBenchmark
{

// 고정밀 시간 측정
class HighResolutionTimer
{
public:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::nanoseconds;

    static TimePoint Now() { return Clock::now(); }

    static uint64_t NowNs()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<Duration>(Now().time_since_epoch()).count());
    }

    static double ToMicroseconds(double ns) { return ns / 1000.0; }
    static double ToMilliseconds(double ns) { return ns / 1000000.0; }
    static double ToSeconds(double ns) { return ns / 1000000000.0; }

    // uint64_t 오버로드
    static double ToMicroseconds(uint64_t ns) { return static_cast<double>(ns) / 1000.0; }
    static double ToMilliseconds(uint64_t ns) { return static_cast<double>(ns) / 1000000.0; }
    static double ToSeconds(uint64_t ns) { return static_cast<double>(ns) / 1000000000.0; }
};

// 벤치마크 결과 통계
struct BenchmarkStats
{
    std::string testName;
    size_t iterations = 0;
    size_t payloadSize = 0;

    // Latency (나노초)
    double avgLatencyNs = 0.0;
    double minLatencyNs = 0.0;
    double maxLatencyNs = 0.0;
    double medianLatencyNs = 0.0;
    double p50LatencyNs = 0.0;
    double p90LatencyNs = 0.0;
    double p95LatencyNs = 0.0;
    double p99LatencyNs = 0.0;
    double stdDevNs = 0.0;

    // Throughput
    double packetsPerSecond = 0.0;
    double megabytesPerSecond = 0.0;
    uint64_t totalBytes = 0;
    uint64_t totalElapsedNs = 0;

    std::string ToString() const
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "======================================\n";
        oss << " Benchmark: " << testName << "\n";
        oss << "======================================\n";
        oss << " Iterations    : " << iterations << "\n";
        oss << " Payload Size  : " << payloadSize << " bytes\n";
        oss << "--------------------------------------\n";
        oss << " Latency (RTT):\n";
        oss << "   Average     : " << HighResolutionTimer::ToMicroseconds(avgLatencyNs) << " us\n";
        oss << "   Min         : " << HighResolutionTimer::ToMicroseconds(minLatencyNs) << " us\n";
        oss << "   Max         : " << HighResolutionTimer::ToMicroseconds(maxLatencyNs) << " us\n";
        oss << "   Median      : " << HighResolutionTimer::ToMicroseconds(medianLatencyNs) << " us\n";
        oss << "   P50         : " << HighResolutionTimer::ToMicroseconds(p50LatencyNs) << " us\n";
        oss << "   P90         : " << HighResolutionTimer::ToMicroseconds(p90LatencyNs) << " us\n";
        oss << "   P95         : " << HighResolutionTimer::ToMicroseconds(p95LatencyNs) << " us\n";
        oss << "   P99         : " << HighResolutionTimer::ToMicroseconds(p99LatencyNs) << " us\n";
        oss << "   Std Dev     : " << HighResolutionTimer::ToMicroseconds(stdDevNs) << " us\n";
        oss << "--------------------------------------\n";
        oss << " Throughput:\n";
        oss << "   Packets/sec : " << packetsPerSecond << "\n";
        oss << "   MB/sec      : " << megabytesPerSecond << "\n";
        oss << "   Total Bytes : " << totalBytes << "\n";
        oss << "   Elapsed     : " << HighResolutionTimer::ToMilliseconds(totalElapsedNs) << " ms\n";
        oss << "======================================\n";
        return oss.str();
    }

    std::string ToCsv() const
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6);
        oss << testName << ","
            << iterations << ","
            << payloadSize << ","
            << avgLatencyNs << ","
            << minLatencyNs << ","
            << maxLatencyNs << ","
            << p50LatencyNs << ","
            << p90LatencyNs << ","
            << p95LatencyNs << ","
            << p99LatencyNs << ","
            << stdDevNs << ","
            << packetsPerSecond << ","
            << megabytesPerSecond;
        return oss.str();
    }

    static std::string CsvHeader()
    {
        return "test_name,iterations,payload_size,avg_latency_ns,min_latency_ns,max_latency_ns,"
               "p50_latency_ns,p90_latency_ns,p95_latency_ns,p99_latency_ns,stddev_ns,"
               "packets_per_sec,mb_per_sec";
    }
};

// 레이턴시 샘플 수집 및 통계 계산
class LatencyCollector
{
public:
    void Reserve(size_t count)
    {
        m_Samples.reserve(count);
    }

    void AddSample(uint64_t latencyNs)
    {
        m_Samples.push_back(latencyNs);
    }

    void Clear()
    {
        m_Samples.clear();
    }

    size_t Count() const { return m_Samples.size(); }

    BenchmarkStats Calculate(const std::string& testName, size_t payloadSize) const
    {
        BenchmarkStats stats;
        stats.testName = testName;
        stats.payloadSize = payloadSize;
        stats.iterations = m_Samples.size();

        if (m_Samples.empty())
        {
            return stats;
        }

        // 정렬된 복사본
        std::vector<uint64_t> sorted = m_Samples;
        std::sort(sorted.begin(), sorted.end());

        // 기본 통계
        stats.minLatencyNs = static_cast<double>(sorted.front());
        stats.maxLatencyNs = static_cast<double>(sorted.back());

        // 합계 계산 (오버플로 방지를 위해 double로 누적)
        double sum = 0.0;
        for (const auto sample : sorted)
        {
            sum += static_cast<double>(sample);
        }
        stats.avgLatencyNs = sum / static_cast<double>(sorted.size());

        // 백분위수
        stats.medianLatencyNs = Percentile(sorted, 50.0);
        stats.p50LatencyNs = stats.medianLatencyNs;
        stats.p90LatencyNs = Percentile(sorted, 90.0);
        stats.p95LatencyNs = Percentile(sorted, 95.0);
        stats.p99LatencyNs = Percentile(sorted, 99.0);

        // 표준 편차
        double sqSum = 0.0;
        for (const auto sample : sorted)
        {
            double diff = static_cast<double>(sample) - stats.avgLatencyNs;
            sqSum += diff * diff;
        }
        stats.stdDevNs = std::sqrt(sqSum / static_cast<double>(sorted.size()));

        // Throughput 계산
        stats.totalElapsedNs = static_cast<uint64_t>(sum);
        stats.totalBytes = static_cast<uint64_t>(stats.iterations) * static_cast<uint64_t>(payloadSize);

        double elapsedSec = HighResolutionTimer::ToSeconds(stats.totalElapsedNs);
        if (elapsedSec > 0.0)
        {
            stats.packetsPerSecond = static_cast<double>(stats.iterations) / elapsedSec;
            stats.megabytesPerSecond = static_cast<double>(stats.totalBytes) / (1024.0 * 1024.0) / elapsedSec;
        }

        return stats;
    }

private:
    static double Percentile(const std::vector<uint64_t>& sorted, double percent)
    {
        if (sorted.empty()) return 0.0;
        if (sorted.size() == 1) return static_cast<double>(sorted[0]);

        double index = (percent / 100.0) * static_cast<double>(sorted.size() - 1);
        size_t lower = static_cast<size_t>(std::floor(index));
        size_t upper = static_cast<size_t>(std::ceil(index));

        if (lower == upper)
        {
            return static_cast<double>(sorted[lower]);
        }

        double fraction = index - static_cast<double>(lower);
        return static_cast<double>(sorted[lower]) * (1.0 - fraction) 
             + static_cast<double>(sorted[upper]) * fraction;
    }

    std::vector<uint64_t> m_Samples;
};

} // namespace FastPortBenchmark
