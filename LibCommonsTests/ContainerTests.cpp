#include "CppUnitTest.h"

import commons.container;
import std;

// Design Ref: session-idle-timeout §8.2 — Container::ForEach / Snapshot 유닛 테스트.
// C-01 ~ C-05 (ForEach 기본/반복/Snapshot 복사/동시성/콜백 제약 확인).

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LibCommonsTests
{

TEST_CLASS(ContainerTests)
{
public:

    // C-01: 빈 Container 에 ForEach 호출 → 콜백 0회.
    TEST_METHOD(ForEach_EmptyContainer)
    {
        LibCommons::Container<int, std::string> container;
        int callCount = 0;
        container.ForEach([&callCount](auto const& /*k*/, auto const& /*v*/) {
            ++callCount;
        });
        Assert::AreEqual(0, callCount, L"Empty container must not invoke callback");
    }

    // C-02: 3 엔트리 Add 후 ForEach → 3회 호출 + 모든 key/value 관찰.
    TEST_METHOD(ForEach_Iterates_AllEntries)
    {
        LibCommons::Container<int, std::string> container;
        container.Add(1, "one");
        container.Add(2, "two");
        container.Add(3, "three");

        int callCount = 0;
        std::map<int, std::string> observed;
        container.ForEach([&](int k, std::string const& v) {
            ++callCount;
            observed[k] = v;
        });

        Assert::AreEqual(3, callCount, L"Callback should be invoked for each entry");
        Assert::AreEqual(std::string("one"),   observed[1]);
        Assert::AreEqual(std::string("two"),   observed[2]);
        Assert::AreEqual(std::string("three"), observed[3]);
    }

    // C-03: Snapshot 은 복사본. 이후 원본 변경해도 snapshot 불변.
    TEST_METHOD(Snapshot_ReturnsIndependentCopy)
    {
        LibCommons::Container<int, std::string> container;
        container.Add(1, "a");
        container.Add(2, "b");

        auto snapshot = container.Snapshot();
        Assert::AreEqual(static_cast<size_t>(2), snapshot.size(), L"Snapshot size mismatch");

        // 원본에서 1개 제거
        Assert::IsTrue(container.Remove(1));
        Assert::AreEqual(static_cast<size_t>(1), container.Size(), L"Container should have 1 entry");

        // snapshot 은 여전히 2개
        Assert::AreEqual(static_cast<size_t>(2), snapshot.size(), L"Snapshot should remain independent");

        // 반환된 값으로 모든 키 확인
        std::map<int, std::string> fromSnapshot;
        for (auto const& [k, v] : snapshot) fromSnapshot[k] = v;
        Assert::AreEqual(std::string("a"), fromSnapshot[1]);
        Assert::AreEqual(std::string("b"), fromSnapshot[2]);
    }

    // C-04: 두 스레드가 동시에 ForEach 만 수행 → read-lock 이므로 블록 없이 완료.
    TEST_METHOD(ForEach_ConcurrentReaders_NoDeadlock)
    {
        LibCommons::Container<int, int> container;
        constexpr int N = 100;
        for (int i = 0; i < N; ++i) container.Add(i, i * 10);

        std::atomic<int> t1Count { 0 };
        std::atomic<int> t2Count { 0 };
        std::atomic<bool> start { false };

        std::thread t1([&]() {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < 50; ++i) {
                container.ForEach([&t1Count](int /*k*/, int /*v*/) {
                    t1Count.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
        std::thread t2([&]() {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < 50; ++i) {
                container.ForEach([&t2Count](int /*k*/, int /*v*/) {
                    t2Count.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });

        start.store(true);
        t1.join();
        t2.join();

        // 각 스레드가 50회 × 100 엔트리 = 5000회씩 호출되어야 함.
        Assert::AreEqual(5000, t1Count.load(), L"Thread 1 should have iterated 5000 times total");
        Assert::AreEqual(5000, t2Count.load(), L"Thread 2 should have iterated 5000 times total");
    }

    // C-05: Snapshot 은 shared_ptr 값도 제대로 복사 (세션 idle checker 주 용례).
    TEST_METHOD(Snapshot_WithSharedPtrValues)
    {
        LibCommons::Container<uint64_t, std::shared_ptr<int>> container;
        container.Add(1, std::make_shared<int>(100));
        container.Add(2, std::make_shared<int>(200));

        auto snapshot = container.Snapshot();
        Assert::AreEqual(static_cast<size_t>(2), snapshot.size());

        // shared_ptr use_count 는 컨테이너 + 스냅샷 = 2 이상이어야 함.
        for (auto const& [k, pVal] : snapshot) {
            Assert::IsNotNull(pVal.get());
            Assert::IsTrue(pVal.use_count() >= 2, L"shared_ptr should be shared between container and snapshot");
        }

        // 컨테이너에서 제거해도 snapshot 의 shared_ptr 은 살아있음.
        container.Remove(1);
        for (auto const& [k, pVal] : snapshot) {
            if (k == 1) {
                Assert::IsNotNull(pVal.get(), L"Snapshot entry should survive container removal");
                Assert::AreEqual(100, *pVal);
            }
        }
    }
};

} // namespace LibCommonsTests
