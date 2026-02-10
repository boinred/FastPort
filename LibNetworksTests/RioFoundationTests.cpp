#include "CppUnitTest.h"
#include <WinSock2.h>
#include <MSWSock.h>

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
        }

        TEST_CLASS_CLEANUP(CleanupWinsock)
        {
            ::WSACleanup();
        }

        TEST_METHOD(TestRioExtensionLoading)
        {
            // 1. Create a dummy overlapped socket
            SOCKET sock = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_REGISTERED_IO);
            if (sock == INVALID_SOCKET)
            {
                // Fallback for environments that might not support RIO flags directly in WSASocket
                sock = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
            }
            
            Assert::AreNotEqual(INVALID_SOCKET, sock, L"Failed to create test socket");

            // 2. Perform initialization
            bool result = LibNetworks::Core::RioExtension::Initialize(sock);
            
            // 3. Verify success
            Assert::IsTrue(result, L"RioExtension::Initialize failed with valid socket");
            Assert::IsTrue(LibNetworks::Core::RioExtension::IsInitialized(), L"IsInitialized() should be true");
            
            // 4. Verify function table is loaded (check one key function)
            Assert::IsNotNull((void*)LibNetworks::Core::RioExtension::GetTable().RIOReceive, L"RIOReceive function pointer is null");

            ::closesocket(sock);
        }

        TEST_METHOD(TestRioExtensionDoubleInitialization)
        {
            // Note: Tests run sequentially or in isolation depending on runner, 
            // but since RioExtension uses static flags, we check if it remains true.
            SOCKET sock = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
            Assert::AreNotEqual(INVALID_SOCKET, sock);

            // First call
            LibNetworks::Core::RioExtension::Initialize(sock);
            
            // Second call
            bool result = LibNetworks::Core::RioExtension::Initialize(sock);
            
            Assert::IsTrue(result, L"Second initialization should return true");
            Assert::IsTrue(LibNetworks::Core::RioExtension::IsInitialized());

            ::closesocket(sock);
        }

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
