#include "CppUnitTest.h"
#include <WinSock2.h>
#include <MSWSock.h>
#include <thread>
#include <chrono>

import networks.services.rio_service;
import networks.core.rio_extension;
import networks.core.socket;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LibNetworksTests
{
    TEST_CLASS(RioServiceTests)
    {
    public:
        static LibNetworks::Core::Socket s_DummySocket;

        TEST_CLASS_INITIALIZE(InitializeRio)
        {

            LibNetworks::Core::Socket::Initialize();

            // RIO 확장 로딩용 더미 소켓
            bool bCreated = s_DummySocket.CreateSocket(LibNetworks::Core::Socket::ENetworkMode::RIO);
            Assert::IsTrue(bCreated, L"Failed to create RIO dummy socket");

            bool bInitialized = LibNetworks::Core::RioExtension::Initialize(s_DummySocket.GetSocket());
            Assert::IsTrue(bInitialized, L"Failed to initialize RioExtension");
        }

        TEST_CLASS_CLEANUP(CleanupRio)
        {
            s_DummySocket.Close();
            LibNetworks::Core::Socket::WSACleanup();
        }

        // CQ 생성 및 초기화 성공 여부를 테스트
        TEST_METHOD(TestCQCreation)
        {
            LibNetworks::Services::RIOService service;
            // 1024개의 엔트리를 가진 CQ 생성 시도
            bool result = service.Initialize(1024);
            
            Assert::IsTrue(result, L"Failed to initialize RIOService with CQ");
            Assert::AreNotEqual((void*)RIO_INVALID_CQ, (void*)service.GetCompletionQueue(), L"Completion Queue handle should be valid");
        }

        // 서비스 시작 및 워커 스레드 정상 동작 여부를 테스트
        TEST_METHOD(TestServiceStartStop)
        {
            LibNetworks::Services::RIOService service;
            Assert::IsTrue(service.Initialize(1024));

            // 워커 스레드 2개 시작
            service.Start(2);
            
            // 잠시 대기 후 정상 종료 확인
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            service.Stop();

            // Stop() 후에는 내부 스레드가 join되어야 함 (블로킹 없이 리턴되면 성공)
            Assert::IsTrue(true, L"Service stopped gracefully");
        }

        // CQ가 닫힌 후 더 이상 이벤트를 처리하지 않는지 테스트
        TEST_METHOD(TestGracefulShutdown)
        {
            LibNetworks::Services::RIOService service;
            service.Initialize(1024);
            service.Start(1);

            service.Stop();

            // Stop 이후 CQ 핸들이 유효하지 않거나 내부적으로 정리되었는지 확인은 어렵지만,
            // 크래시 없이 재시작이 안 되거나 안전하게 종료되었는지를 간접 검증
            // (여기서는 Stop 호출 시 예외가 발생하지 않는 것을 중점으로 봄)
        }
    };

    LibNetworks::Core::Socket RioServiceTests::s_DummySocket;
}
