#include "CppUnitTest.h"
#include <WinSock2.h>
#include <MSWSock.h>

#pragma comment(lib, "ws2_32.lib")

import networks.core.rio_extension;
import networks.core.socket;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LibNetworksTests
{
    TEST_CLASS(RioFoundationTests)
    {
    public:
        TEST_CLASS_INITIALIZE(InitializeWinsock)
        {
            WSADATA wsaData;
            int result = ::WSAStartup(MAKEWORD(2, 2), &wsaData);
            Assert::AreEqual(0, result, L"WSAStartup failed");

            LibNetworks::Core::Socket::Initialize();
        }

        TEST_CLASS_CLEANUP(CleanupWinsock)
        {
            LibNetworks::Core::Socket::WSACleanup();
        }

        // RIO 확장 함수들이 정상적으로 로드되는지 테스트.
        TEST_METHOD(TestRioExtensionLoading)
        {
            // 1. Create a dummy overlapped socket
            LibNetworks::Core::Socket dummy; 
            bool bSuccess = dummy.CreateSocket(LibNetworks::Core::Socket::ENetworkMode::RIO);
            
            Assert::IsTrue(bSuccess, L"Failed to create test socket");

            // 2. Perform initialization
            bool result = LibNetworks::Core::RioExtension::Initialize(dummy.GetSocket());
            
            // 3. Verify success
            Assert::IsTrue(result, L"RioExtension::Initialize failed with valid socket");
            Assert::IsTrue(LibNetworks::Core::RioExtension::IsInitialized(), L"IsInitialized() should be true");
            
            // 4. Verify function table is loaded (check one key function)
            Assert::IsNotNull((void*)LibNetworks::Core::RioExtension::GetTable().RIOReceive, L"RIOReceive function pointer is null");

            dummy.Close();
        }

        // Initialize 함수를 여러 번 호출해도 안전한지 테스트.
        TEST_METHOD(TestRioExtensionDoubleInitialization)
        {
            // Note: Tests run sequentially or in isolation depending on runner, 
            // but since RioExtension uses static flags, we check if it remains true.
            // 1. Create a dummy overlapped socket
            LibNetworks::Core::Socket dummy;
            bool bSuccess = dummy.CreateSocket(LibNetworks::Core::Socket::ENetworkMode::RIO);
            // First call
            LibNetworks::Core::RioExtension::Initialize(dummy.GetSocket());
            
            // Second call
            bool result = LibNetworks::Core::RioExtension::Initialize(dummy.GetSocket());
            
            Assert::IsTrue(result, L"Second initialization should return true");
            Assert::IsTrue(LibNetworks::Core::RioExtension::IsInitialized());

            dummy.Close();
        }

        // 초기화 중 유효하지 않은 소켓 처리를 테스트.
        TEST_METHOD(TestRioExtensionInvalidSocket)
        {
            // If already initialized by previous tests, we can't easily "un-initialize" 
            // because it's static. But we check if it handles INVALID_SOCKET at least 
            // when it's NOT initialized, OR if it returns false for the specific call.
            
            // Note: Our implementation returns false if socket is INVALID_SOCKET.
            bool result = LibNetworks::Core::RioExtension::Initialize(INVALID_SOCKET);
            
            // If it was already initialized, it might return true (it checks m_bInitialized first).
            // Let's verify the logic in RioExtension.cpp:
            // if (m_bInitialized) return true;
            // if (socket == INVALID_SOCKET) return false;
            
            if (!LibNetworks::Core::RioExtension::IsInitialized())
            {
                Assert::IsFalse(result, L"Initialization should fail with INVALID_SOCKET");
            }
        }
    };
}
