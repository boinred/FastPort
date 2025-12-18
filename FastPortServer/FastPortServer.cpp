// FastPortServer.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
//

#include <iostream>
#include <filesystem>
#include <spdlog/spdlog.h>

import commons.logger; 
import networks.core.socket;

import fastport_service_mode;

int main(int argc, const char* argv[])
{
    std::string location;

    bool bServiceMode = true;
#if _DEBUG
    location = std::filesystem::current_path().string();
    bServiceMode = false;
#endif // #if _DEBUG

    std::time_t t = std::time(nullptr);

    std::tm now{};
    localtime_s(&now, &t);

    std::string fileName = std::format("log_{:04}_{:02}_{:02}_{:02}_{:02}.txt", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min);

    auto& logger = LibCommons::Logger::GetInstance();
    // Create Logger 
    logger.Create(location + "/" + "loggers", fileName, 1024 * 1024 * 10, 3, bServiceMode);

    LibNetworks::Core::Socket::Initialize();

    std::shared_ptr<FastPortServiceMode>  pServiceMode = std::make_shared<FastPortServiceMode>();
    pServiceMode->Execute(argc, argv);

    logger.LogInfo("Main", "FastPort Started. V : {}", "Started.");
#if _DEBUG 
    pServiceMode->Wait();
#endif // #if _DEBUG
    logger.LogInfo("Main", "FastPort Started. V : {}", "Closed.");

    
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
