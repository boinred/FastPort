/**
 * FastPortBenchmark
 * 
 * IOCP 네트워크 성능 벤치마크 도구
 * 
 * 측정 항목:
 * - Latency (RTT): 요청-응답 왕복 시간
 * - Throughput: 초당 패킷/바이트 처리량
 * 
 * 사용법:
 *   FastPortBenchmark.exe [옵션]
 * 
 * 옵션:
 *   --host <ip>         서버 주소 (기본: 127.0.0.1)
 *   --port <port>       서버 포트 (기본: 9000)
 *   --iterations <n>    반복 횟수 (기본: 10000)
 *   --warmup <n>        워밍업 횟수 (기본: 100)
 *   --payload <bytes>   페이로드 크기 (기본: 64)
 *   --output <file>     CSV 결과 파일 (자동 타임스탬프 추가)
 *   --verbose           상세 출력
 */

#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <WinSock2.h>
#include <WS2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#include "BenchmarkStats.h"
#include "BenchmarkRunner.h"
#include "LatencyBenchmarkRunner.h"

import networks.core.socket;
import commons.logger;
import std; 

using namespace FastPortBenchmark;

// 타임스탬프 문자열 생성 (YYYY-MM-DD-HH-mm)
std::string GetTimestampString()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    
    std::tm tm{};
    localtime_s(&tm, &time);
    
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d-%H-%M-%S");
    return oss.str();
}

// 파일명에 타임스탬프 추가
std::string AddTimestampToFilename(const std::string& filename)
{
    std::string timestamp = GetTimestampString();
    
    // 확장자 분리
    size_t dotPos = filename.rfind('.');
    if (dotPos != std::string::npos)
    {
        std::string name = filename.substr(0, dotPos);
        std::string ext = filename.substr(dotPos);
        return name + "_" + timestamp + ext;
    }
    else
    {
        return filename + "_" + timestamp;
    }
}

// 명령줄 파서
struct CommandLineArgs
{
    std::string host = "127.0.0.1";
    uint16_t port = 6628;
    size_t iterations = 10000;
    size_t warmup = 100;
    size_t payloadSize = 64;
    std::string outputFile;
    bool verbose = false;
    bool help = false;

    static CommandLineArgs Parse(int argc, char* argv[])
    {
        CommandLineArgs args;

        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];

            if (arg == "--host" && i + 1 < argc)
            {
                args.host = argv[++i];
            }
            else if (arg == "--port" && i + 1 < argc)
            {
                args.port = static_cast<uint16_t>(std::stoi(argv[++i]));
            }
            else if (arg == "--iterations" && i + 1 < argc)
            {
                args.iterations = std::stoull(argv[++i]);
            }
            else if (arg == "--warmup" && i + 1 < argc)
            {
                args.warmup = std::stoull(argv[++i]);
            }
            else if (arg == "--payload" && i + 1 < argc)
            {
                args.payloadSize = std::stoull(argv[++i]);
            }
            else if (arg == "--output" && i + 1 < argc)
            {
                args.outputFile = argv[++i];
            }
            else if (arg == "--verbose")
            {
                args.verbose = true;
            }
            else if (arg == "--help" || arg == "-h")
            {
                args.help = true;
            }
        }

        return args;
    }

    static void PrintUsage()
    {
        std::cout << R"(
FastPortBenchmark - IOCP Network Performance Benchmark

Usage: FastPortBenchmark.exe [options]

Options:
  --host <ip>         Server address (default: 127.0.0.1)
  --port <port>       Server port (default: 9000)
  --iterations <n>    Number of iterations (default: 10000)
  --warmup <n>        Warmup iterations (default: 100)
  --payload <bytes>   Payload size in bytes (default: 64)
  --output <file>     Output CSV file path (timestamp auto-added)
  --verbose           Verbose output
  --help, -h          Show this help

Examples:
  FastPortBenchmark.exe --iterations 1000 --payload 256
  FastPortBenchmark.exe --host 192.168.1.100 --port 9001 --output results.csv

Note: Output filename will have timestamp appended (e.g., results_2024-01-15-14-30.csv)
)";
    }
};


// 진행률 표시
void PrintProgress(size_t current, size_t total)
{
    static size_t lastPercent = 0;
    size_t percent = (current * 100) / total;

    if (percent != lastPercent)
    {
        std::cout << "\rProgress: " << percent << "% (" << current << "/" << total << ")   " << std::flush;
        lastPercent = percent;
    }
}

// CSV 저장
void SaveResultsToCsv(const std::string& baseFilename, const std::vector<BenchmarkStats>& results)
{
    std::string filename = AddTimestampToFilename(baseFilename);
    
    std::ofstream file(filename);
    if (!file.is_open())
    {
        std::cerr << "Failed to open output file: " << filename << std::endl;
        return;
    }

    file << BenchmarkStats::CsvHeader() << "\n";
    for (const auto& stats : results)
    {
        file << stats.ToCsv() << "\n";
    }

    std::cout << "Results saved to: " << filename << std::endl;
}

// 디버그 모드에서 키 입력 대기
void WaitForKeyInDebugMode()
{
#ifdef _DEBUG
    std::cout << "\nPress any key to exit..." << std::endl;
    std::cin.get();
#endif
}

int main(int argc, char* argv[])
{
    // 명령줄 파싱
    auto args = CommandLineArgs::Parse(argc, argv);

    if (args.help)
    {
        CommandLineArgs::PrintUsage();
        //WaitForKeyInDebugMode();
        return 0;
    }

    std::string location = std::filesystem::current_path().string();

    std::cout << "Current Path : " << location << std::endl;

    std::time_t t = std::time(nullptr);

    std::tm now{};
    localtime_s(&now, &t);
    std::string fileName = std::format("log_{:04}_{:02}_{:02}_{:02}_{:02}.txt", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min);   

    // Create Logger 
    LibCommons::Logger::GetInstance().Create(location + "/" + "loggers_benchmark", fileName, 1024 * 1024 * 10, 3, false);

    // Winsock 초기화
    LibNetworks::Core::Socket::Initialize();
    

    std::cout << "======================================\n";
    std::cout << " FastPort Benchmark\n";
    std::cout << "======================================\n";
    std::cout << " Server     : " << args.host << ":" << args.port << "\n";
    std::cout << " Iterations : " << args.iterations << "\n";
    std::cout << " Warmup     : " << args.warmup << "\n";
    std::cout << " Payload    : " << args.payloadSize << " bytes\n";
    std::cout << "======================================\n\n";

    // 벤치마크 설정
    BenchmarkConfig config;
    config.testName = "LatencyTest";
    config.serverHost = args.host;
    config.serverPort = args.port;
    config.iterations = args.iterations;
    config.warmupIterations = args.warmup;
    config.payloadSize = args.payloadSize;
    config.verbose = args.verbose;

    // 결과 저장용
    std::vector<BenchmarkStats> allResults;
    BenchmarkStats finalResult;
    bool completed = false;
    std::atomic<bool> finished{false};

    // 콜백 설정
    BenchmarkCallbacks callbacks;

    callbacks.onStateChanged = [&](BenchmarkState state)
    {
        switch (state)
        {
        case BenchmarkState::Connecting:
            std::cout << "Connecting to server..." << std::endl;
            break;
        case BenchmarkState::Warmup:
            std::cout << "Warming up..." << std::endl;
            break;
        case BenchmarkState::Running:
            std::cout << "Running benchmark..." << std::endl;
            break;
        case BenchmarkState::Completed:
            std::cout << "\nBenchmark completed!" << std::endl;
            completed = true;
            finished.store(true);
            break;
        case BenchmarkState::Failed:
            std::cout << "\nBenchmark failed!" << std::endl;
            finished.store(true);
            break;
        default:
            break;
        }
    };

    callbacks.onProgress = [&](size_t current, size_t total)
    {
        PrintProgress(current, total);
    };

    callbacks.onCompleted = [&](const BenchmarkStats& stats)
    {
        finalResult = stats;
        allResults.push_back(stats);
        std::cout << "\n" << stats.ToString() << std::endl;
    };

    callbacks.onError = [&](const std::string& error)
    {
        std::cerr << "Error: " << error << std::endl;
        finished.store(true);
    };

    // 벤치마크 실행
    auto runner = std::make_unique<LatencyBenchmarkRunner>();
    if (!runner->Start(config, callbacks))
    {
        std::cerr << "Failed to start benchmark" << std::endl;
        //WaitForKeyInDebugMode();
        return 1;
    }

    // 완료 대기
    while (!finished.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    runner->Stop();

    // CSV 저장
    if (!args.outputFile.empty() && !allResults.empty())
    {
        SaveResultsToCsv(args.outputFile, allResults);
    }

    WaitForKeyInDebugMode();

    return completed ? 0 : 1;
}
