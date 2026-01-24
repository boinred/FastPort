#pragma once

#include <memory>
#include <thread>
#include <atomic>
#include <cstdint>

#include "BenchmarkStats.h"
#include "BenchmarkRunner.h"

import networks.services.io_service;

namespace FastPortBenchmark
{

// 패킷 ID 정의
constexpr uint16_t PACKET_ID_BENCHMARK_REQUEST = 0x1001;
constexpr uint16_t PACKET_ID_BENCHMARK_RESPONSE = 0x1002;

// 레이턴시 벤치마크 실행기
class LatencyBenchmarkRunner : public IBenchmarkRunner
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

    std::shared_ptr< LibNetworks::Services::IOService> m_IoService;
};


} // namespace FastPortBenchmark
