// FastPortServer.cpp — IOCP 전용 서버 엔트리 포인트.
// RIO 서버는 FastPortServerRIO 프로젝트로 분리됨.

#include <iostream>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>

import std;
import commons.logger;
import commons.event_listener;
import commons.service_mode;
import networks.core.socket;

import iocp_service_mode;

int main(int argc, const char* argv[])
{
    std::string location = std::filesystem::current_path().string();

    // 1. 로거 및 기본 환경 설정
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

    // 2. IOCP 서비스 생성 및 실행
    auto pService = std::make_shared<IOCPServiceMode>();

    logger.LogInfo("Main", "FastPortServer Starting (IOCP mode)...");

    pService->Execute(static_cast<DWORD>(argc), argv);

#if _DEBUG
    pService->Wait();
#endif
    logger.LogInfo("Main", "FastPortServer Closed.");
    return 0;
}
