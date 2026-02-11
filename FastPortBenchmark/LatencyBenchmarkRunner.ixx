module;

#include <stdint.h>

export module benchmark.latency_runner;

import std;
import benchmark.stats;
import benchmark.runner;
import networks.services.inetwork_service;

namespace FastPortBenchmark
{

// 패킷 ID 정의
export constexpr uint16_t PACKET_ID_BENCHMARK_REQUEST = 0x1001;
export constexpr uint16_t PACKET_ID_BENCHMARK_RESPONSE = 0x1002;

// 레이턴시 벤치마크 실행기
export class LatencyBenchmarkRunner : public IBenchmarkRunner
{
public:
    LatencyBenchmarkRunner();
    ~LatencyBenchmarkRunner() override;

    // IBenchmarkRunner 구현
    bool Start(const BenchmarkConfig& config, const BenchmarkCallbacks& callbacks) override;
    void Stop() override;
    BenchmarkState GetState() const override;
    BenchmarkStats GetResults() const override;

private:
    void SetState(BenchmarkState state);
    void RunBenchmark();

private:
    BenchmarkConfig m_Config;
    BenchmarkCallbacks m_Callbacks;

    std::atomic<BenchmarkState> m_State;
    std::atomic<bool> m_StopRequested{ false };

    LatencyCollector m_LatencyCollector;
    BenchmarkStats m_Results;

    std::thread m_RunnerThread;

    std::shared_ptr<LibNetworks::Services::INetworkService> m_Service;
};


} // namespace FastPortBenchmark
