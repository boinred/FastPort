export module benchmark.runner;

import std;
import benchmark.stats;

namespace FastPortBenchmark
{
    using namespace std;

// 벤치마크 설정
export struct BenchmarkConfig
{
    std::string testName = "Default";
    std::string serverHost = "127.0.0.1";
    uint16_t serverPort = 6628;
    
    size_t iterations = 10000;          // 반복 횟수
    size_t warmupIterations = 100;      // 워밍업 횟수
    size_t payloadSize = 64;            // 페이로드 크기 (바이트)
    size_t payloadMinSize = 0;          // 랜덤 페이로드 최소 크기. 0이면 payloadSize 사용
    size_t payloadMaxSize = 0;          // 랜덤 페이로드 최대 크기. 0이면 payloadSize 사용
    size_t payloadPoolSize = 1024;      // 사전 생성 payload 개수
    size_t sessionCount = 1;            // 실제 연결 세션 수
    uint32_t ioThreadCount = 2;         // IOCP 워커 스레드 수
    
    uint32_t timeoutMs = 5000;          // 응답 타임아웃 (밀리초)
    bool verbose = false;               // 상세 출력
    bool useRio = false;                // RIO 사용 여부
};

// 벤치마크 진행 상태
export enum class BenchmarkState
{
    Idle,
    Connecting,
    Warmup,
    Running,
    Completed,
    Failed
};

// 벤치마크 진행 콜백
export struct BenchmarkCallbacks
{
    std::function<void(BenchmarkState state)> onStateChanged;
    std::function<void(size_t current, size_t total)> onProgress;
    std::function<void(const BenchmarkStats& stats)> onCompleted;
    std::function<void(const std::string& error)> onError;
};

// 벤치마크 실행기 인터페이스
export class IBenchmarkRunner
{
public:
    virtual ~IBenchmarkRunner() = default;

    virtual bool Start(const BenchmarkConfig& config, const BenchmarkCallbacks& callbacks) = 0;
    virtual void Stop() = 0;
    virtual BenchmarkState GetState() const = 0;
    virtual BenchmarkStats GetResults() const = 0;
};

// 동기화 대기 헬퍼
export class BenchmarkWaiter
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
