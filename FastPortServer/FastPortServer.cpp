// FastPortServer.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
//

#include <iostream>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>
#include <cxxopts.hpp>

import std;
import commons.logger; 
import commons.event_listener; 
import commons.service_mode;
import networks.core.socket;

import iocp_service_mode;
import rio_service_mode;

int main(int argc, const char* argv[])
{
    std::string location = std::filesystem::current_path().string();

    // 1. 명령줄 인자 파싱
    bool bUseRIOMode = false;
    std::vector<const char*> filteredArgv;
    try {
        cxxopts::Options options("FastPortServer", "High-performance network server");
        options.allow_unrecognised_options(); // ServiceMode의 인자(install 등)를 위해 허용
        options.add_options()
            ("r,rio", "Use RIO (Registered I/O) mode", cxxopts::value<bool>()->default_value("false"))
            ("h,help", "Print usage");

        auto result = options.parse(argc, const_cast<char**>(argv));
        if (result.count("help")) {
            std::cout << options.help() << std::endl;
            return 0;
        }
        bUseRIOMode = result["rio"].as<bool>();

        // ServiceMode에 전달할 인자에서 --rio 관련 제거
        filteredArgv.reserve(argc);
        for (int i = 0; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg != "--rio" && arg != "-r" && arg != "-rio") {
                filteredArgv.push_back(argv[i]);
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error parsing options: " << e.what() << std::endl;
        return 1;
    }

    // 2. 로거 및 기본 환경 설정
    bool bServiceMode = true;
#if _DEBUG
    bServiceMode = false;
#endif

    auto now = std::chrono::floor<std::chrono::minutes>(std::chrono::current_zone()->to_local(std::chrono::system_clock::now()));
    std::string fileName = std::format("log_{:%Y_%m_%d_%H_%M}.txt", now);

    auto& logger = LibCommons::Logger::GetInstance();
    logger.Create(location + "/" + "loggers", fileName, 1024 * 1024 * 10, 3, bServiceMode);

    LibNetworks::Core::Socket::Initialize();
    LibCommons::EventListener::GetInstance().Init(std::thread::hardware_concurrency());

    // 3. 모드에 따른 서비스 생성 및 실행
    std::shared_ptr<LibCommons::ServiceMode> pService;
    if (bUseRIOMode)
    {
        pService = std::make_shared<RIOServiceMode>();
    }
    else
    {
        pService = std::make_shared<IOCPServiceMode>();
    }

    logger.LogInfo("Main", "FastPort Starting in {} mode...", (bUseRIOMode ? "RIO" : "IOCP"));

    pService->Execute(static_cast<DWORD>(filteredArgv.size()), filteredArgv.data());

#if _DEBUG 
    pService->Wait();
#endif
    logger.LogInfo("Main", "FastPort Closed.");
    return 0;
}

// 프로그램 실행: <Ctrl+F5> 또는 [디버그] > [디버깅하지 않고 시작] 메뉴
// 프로그램 디버그: <F5> 키 또는 [디버그] > [디버깅 시작] 메뉴

// 시작을 위한 팁: 
//   1. [솔루션 탐색기] 창을 사용하여 파일을 추가/관리합니다.
//   2. [팀 탐색기] 창을 사용하여 소스 제어에 연결합니다.
//   3. [출력] 창을 사용하여 빌드 출력 및 기타 메시지를 확인합니다.
//   4. [오류 목록] 창을 사용하여 오류를 봅니다.
//   5. [프로젝트] > [새 항목 추가]로 이동하여 새 코드 파일을 만들거나, [프로젝트] > [기존 항목 추가]로 이동하여 기존 코드 파일을 프로젝트에 추가합니다.
//   6. 나중에 이 프로젝트를 다시 열려면 [파일] > [열기] > [프로젝트]로 이동하고 .sln 파일을 선택합니다.
