/**
 * FastPortBenchmark
 *
 * IOCP/RIO Network Performance Benchmark Tool
 */

#include <stdint.h>
#include <format>

import std; 
import networks.core.socket;
import commons.logger;
import benchmark.stats;
import benchmark.runner;
import benchmark.latency_runner;

using namespace FastPortBenchmark;

// 타임스탬프 문자열 생성 (YYYY-MM-DD-HH-mm)
static std::string GetTimestampString()
{
    auto now = std::chrono::system_clock::now();
    // C++20 format 사용
    return std::format("{:%Y-%m-%d-%H-%M-%S}", std::chrono::zoned_time{ std::chrono::current_zone(), now });
}

// 파일명에 타임스탬프 추가
static std::string AddTimestampToFilename(const std::string& filename)
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
    uint16_t port = 9000;
    size_t iterations = 10000;
    size_t warmup = 100;
    size_t payloadSize = 64;
    std::string outputFile;
    bool verbose = false;
    bool help = false;
    bool useRio = false;

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
            else if (arg == "--mode" && i + 1 < argc)
            {
                std::string mode = argv[++i];
                if (mode == "rio") args.useRio = true;
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
FastPortBenchmark - Network Performance Benchmark

Usage: FastPortBenchmark.exe [options]

Options:
  --host <ip>         Server address (default: 127.0.0.1)
  --port <port>       Server port (default: 9000)
  --mode <mode>       Network mode: iocp (default) or rio
  --iterations <n>    Number of iterations (default: 10000)
  --warmup <n>        Warmup iterations (default: 100)
  --payload <bytes>   Payload size in bytes (default: 64)
  --output <file>     Output CSV file path (timestamp auto-added)
  --verbose           Verbose output
  --help, -h          Show this help

Examples:
  FastPortBenchmark.exe --mode rio --iterations 10000
  FastPortBenchmark.exe --host 192.168.1.100 --port 9001 --output results.csv
)";
    }
};


// 진행률 표시
static void PrintProgress(size_t current, size_t total)
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
static void SaveResultsToCsv(const std::string& baseFilename, const std::vector<BenchmarkStats>& results)
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
static void WaitForKeyInDebugMode()
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
        return 0;
    }

    std::string location = std::filesystem::current_path().string();

    std::cout << "Current Path : " << location << std::endl;

    auto now = std::chrono::system_clock::now();
    std::string fileName = std::format("log_{:%Y_%m_%d_%H_%M}.txt", std::chrono::zoned_time{ std::chrono::current_zone(), now });

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
    config.useRio = args.useRio;

    // 결과 저장용
    std::vector<BenchmarkStats> allResults;
    BenchmarkStats finalResult;
    bool completed = false;
    std::atomic<bool> finished{ false };

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