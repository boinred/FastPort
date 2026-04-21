#include "CppUnitTest.h"
#include <WinSock2.h>
#include <MSWSock.h>

import networks.core.rio_buffer_manager;
import networks.core.rio_extension;
import networks.core.socket;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LibNetworksTests
{
    TEST_CLASS(RioBufferManagerTests)
    {
    public:
        static LibNetworks::Core::Socket s_DummySocket;

        TEST_CLASS_INITIALIZE(InitializeRio)
        {
            // Winsock 초기화
            LibNetworks::Core::Socket::Initialize();

            // RIO 확장 함수 로딩을 위한 더미 소켓 생성
            bool bCreated = s_DummySocket.CreateSocket(LibNetworks::Core::Socket::ENetworkMode::RIO);
            Assert::IsTrue(bCreated, L"Failed to create RIO dummy socket");

            // RIO 확장 함수 초기화
            bool bInitialized = LibNetworks::Core::RioExtension::Initialize(s_DummySocket.GetSocket());
            Assert::IsTrue(bInitialized, L"Failed to initialize RioExtension");
        }

        TEST_CLASS_CLEANUP(CleanupRio)
        {
            s_DummySocket.Close();
            LibNetworks::Core::Socket::WSACleanup();
        }

        // 대용량 메모리 풀(64MB) 초기화 테스트
        TEST_METHOD(TestPoolInitialization)
        {
            LibNetworks::Core::RioBufferManager manager;
            uint32_t poolSize = 64 * 1024 * 1024; // 64MB

            bool result = manager.Initialize(poolSize);
            Assert::IsTrue(result, L"Failed to initialize RioBufferManager pool");
        }

        // 버퍼 슬라이스 할당 및 속성 검증 테스트
        TEST_METHOD(TestSliceAllocation)
        {
            LibNetworks::Core::RioBufferManager manager;
            uint32_t poolSize = 1024 * 1024; // 1MB
            Assert::IsTrue(manager.Initialize(poolSize));

            LibNetworks::Core::RioBufferSlice slice1;
            bool result1 = manager.AllocateSlice(1024, slice1); // 1KB
            Assert::IsTrue(result1, L"Failed to allocate slice 1");
            Assert::AreNotEqual((void*)0, (void*)slice1.BufferId, L"Slice 1 BufferId invalid");
            Assert::AreEqual((ULONG)0, slice1.Offset, L"Slice 1 Offset mismatch");
            Assert::AreEqual((ULONG)1024, slice1.Length, L"Slice 1 Length mismatch");

            LibNetworks::Core::RioBufferSlice slice2;
            bool result2 = manager.AllocateSlice(2048, slice2); // 2KB
            Assert::IsTrue(result2, L"Failed to allocate slice 2");
            Assert::IsTrue(slice1.BufferId == slice2.BufferId, L"BufferId should be same for same pool");
            Assert::AreEqual((ULONG)1024, slice2.Offset, L"Slice 2 Offset mismatch");
            Assert::IsNotNull(slice2.pData, L"Slice 2 pointer null");
        }

        // 풀 용량 초과 시 할당 실패 테스트
        TEST_METHOD(TestPoolExhaustion)
        {
            LibNetworks::Core::RioBufferManager manager;
            uint32_t poolSize = 4096; // 4KB small pool
            Assert::IsTrue(manager.Initialize(poolSize));

            LibNetworks::Core::RioBufferSlice slice;
            
            // 1. Allocate full size
            bool result1 = manager.AllocateSlice(4096, slice);
            Assert::IsTrue(result1, L"Failed to allocate full pool");

            // 2. Try to allocate more
            bool result2 = manager.AllocateSlice(1, slice);
            Assert::IsFalse(result2, L"Should fail when pool is exhausted");
        }

        // 슬라이스 반환 후 재사용 테스트
        TEST_METHOD(TestDeallocateAndReuse)
        {
            LibNetworks::Core::RioBufferManager manager;
            uint32_t poolSize = 4096;
            Assert::IsTrue(manager.Initialize(poolSize));

            // 1. 풀 전체를 할당
            LibNetworks::Core::RioBufferSlice slice;
            Assert::IsTrue(manager.AllocateSlice(4096, slice));
            Assert::AreEqual((uint32_t)1, manager.GetAllocatedCount());
            Assert::AreEqual((uint32_t)0, manager.GetFreeCount());

            // 2. 추가 할당 실패 (풀 소진)
            LibNetworks::Core::RioBufferSlice slice2;
            Assert::IsFalse(manager.AllocateSlice(4096, slice2));

            // 3. 반환
            manager.DeallocateSlice(slice);
            Assert::AreEqual((uint32_t)0, manager.GetAllocatedCount());
            Assert::AreEqual((uint32_t)1, manager.GetFreeCount());

            // 4. 재할당 성공 (free list에서 재사용)
            LibNetworks::Core::RioBufferSlice reusedSlice;
            Assert::IsTrue(manager.AllocateSlice(4096, reusedSlice));
            Assert::AreEqual((uint32_t)1, manager.GetAllocatedCount());
            Assert::AreEqual((uint32_t)0, manager.GetFreeCount());

            // 5. 재사용된 슬라이스는 원본과 동일한 오프셋/포인터
            Assert::AreEqual(slice.Offset, reusedSlice.Offset);
            Assert::AreEqual(slice.Length, reusedSlice.Length);
            Assert::IsTrue(slice.pData == reusedSlice.pData);
        }

        // 여러 슬라이스 반환 후 순서대로 재사용
        TEST_METHOD(TestMultipleDeallocateReuse)
        {
            LibNetworks::Core::RioBufferManager manager;
            Assert::IsTrue(manager.Initialize(8192));

            LibNetworks::Core::RioBufferSlice s1, s2;
            Assert::IsTrue(manager.AllocateSlice(4096, s1));
            Assert::IsTrue(manager.AllocateSlice(4096, s2));
            Assert::AreEqual((uint32_t)2, manager.GetAllocatedCount());

            // 반환 순서: s1, s2
            manager.DeallocateSlice(s1);
            manager.DeallocateSlice(s2);
            Assert::AreEqual((uint32_t)0, manager.GetAllocatedCount());
            Assert::AreEqual((uint32_t)2, manager.GetFreeCount());

            // 재할당: free list에서 동일 크기 매칭
            LibNetworks::Core::RioBufferSlice r1;
            Assert::IsTrue(manager.AllocateSlice(4096, r1));
            Assert::AreEqual((uint32_t)1, manager.GetAllocatedCount());
            Assert::AreEqual((uint32_t)1, manager.GetFreeCount());
        }

        // 메모리 정렬(Alignment) 검증 (현재 구현은 단순 순차 할당이므로 오프셋 확인)
        TEST_METHOD(TestAlignmentCheck)
        {
            LibNetworks::Core::RioBufferManager manager;
            Assert::IsTrue(manager.Initialize(8192));

            LibNetworks::Core::RioBufferSlice slice1, slice2;
            
            // Allocate odd size
            manager.AllocateSlice(123, slice1);
            
            // Allocate next
            manager.AllocateSlice(100, slice2);

            // Current implementation is simple pointer bump. 
            // If alignment is needed later, this test will need update.
            // For now, we verify basic sequential consistency.
            Assert::AreEqual(slice1.Offset + slice1.Length, slice2.Offset, L"Sequential allocation offset mismatch");
        }
    };

    LibNetworks::Core::Socket RioBufferManagerTests::s_DummySocket;
}
