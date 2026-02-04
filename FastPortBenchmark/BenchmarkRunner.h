#pragma once

#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>

#include "BenchmarkStats.h"

namespace FastPortBenchmark
{

// 벤치마크 설정
struct BenchmarkConfig
{
    std::string testName = "Default";
    std::string serverHost = "127.0.0.1";
    uint16_t serverPort = 9000;
    
    size_t iterations = 10000;          // 반복 횟수
    size_t warmupIterations = 100;      // 워밍업 횟수
    size_t payloadSize = 64;            // 페이로드 크기 (바이트)
    
    uint32_t timeoutMs = 5000;          // 응답 타임아웃 (밀리초)
    bool verbose = false;               // 상세 출력
};

// 벤치마크 진행 상태
enum class BenchmarkState
{
    Idle,
    Connecting,
    Warmup,
    Running,
    Completed,
    Failed
};

// 벤치마크 진행 콜백
struct BenchmarkCallbacks
{
    std::function<void(BenchmarkState state)> onStateChanged;
    std::function<void(size_t current, size_t total)> onProgress;
    std::function<void(const BenchmarkStats& stats)> onCompleted;
    std::function<void(const std::string& error)> onError;
};

// 벤치마크 실행기 인터페이스
class IBenchmarkRunner
{
public:
    virtual ~IBenchmarkRunner() = default;

    virtual bool Start(const BenchmarkConfig& config, const BenchmarkCallbacks& callbacks) = 0;
    virtual void Stop() = 0;
    virtual BenchmarkState GetState() const = 0;
    virtual BenchmarkStats GetResults() const = 0;
};

// 동기화 대기 헬퍼
class BenchmarkWaiter
{
public:
    void Signal()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Signaled = true;
        m_Cv.notify_all();
    }

    bool Wait(uint32_t timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        return m_Cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return m_Signaled; });
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Signaled = false;
    }

private:
    std::mutex m_Mutex;
    std::condition_variable m_Cv;
    bool m_Signaled = false;
};

} // namespace FastPortBenchmark
