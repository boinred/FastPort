#include "CppUnitTest.h"

import commons.timer_queue;
import commons.logger;
import std;

// Design Ref: §8 Test Plan — L1 Unit 테스트 (scope: core,schedule + cancel,lifecycle).
// Logger::Create() 초기화 필수: spdlog 가 async thread pool 을 필요로 하며, Create() 없이
// 로그 호출 시 "async log: thread pool doesn't exist anymore" 경고가 누적됨.
// TEST_MODULE_INITIALIZE 에서 1회 호출.

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LibCommonsTests
{

// DLL 로드 시 1회 실행 — Logger/spdlog 초기화.
TEST_MODULE_INITIALIZE(InitLogger)
{
    LibCommons::Logger::GetInstance().Create(
        /*directoryName=*/"_test_logs",
        /*fileName=*/"timer_queue_tests.log",
        /*maxFileSize=*/10 * 1024 * 1024,
        /*maxFileCount=*/3,
        /*bServiceMode=*/false);
}

TEST_MODULE_CLEANUP(ShutdownLogger)
{
    // 중요: TimerQueue 싱글톤을 Logger 보다 먼저 정리해야 한다.
    // 순서가 반대면 TimerQueue 의 남은 콜백이 이미 shutdown 된 spdlog 를 호출해 segfault.
    LibCommons::TimerQueue::GetInstance().Shutdown(/*waitForCallbacks=*/true);
    LibCommons::Logger::GetInstance().Shutdown();
}


// Design Ref: §8.2 L1-U07 (Command_Execute).
struct CountingCommand : public LibCommons::ITimerCommand
{
    std::atomic<int>* m_pCounter;
    std::string       m_Name;

    CountingCommand(std::atomic<int>* pCounter, std::string_view name)
        : m_pCounter(pCounter), m_Name(name)
    {
    }

    void Execute() override
    {
        m_pCounter->fetch_add(1, std::memory_order_relaxed);
    }

    std::string_view Name() const noexcept override
    {
        return m_Name;
    }
};

namespace
{
    // 전역 싱글톤을 테스트 전반에서 공용. 테스트 격리는 고유 id/카운터로 확보.
    LibCommons::TimerQueue& Tq()
    {
        return LibCommons::TimerQueue::GetInstance();
    }

    // Windows 타이머 해상도(~15ms)를 고려해 넉넉한 여유 시간.
    using namespace std::chrono_literals;
}


TEST_CLASS(TimerQueueTests)
{
public:

    // U-01: ScheduleOnce 가 유효 id 를 반환한다 (Plan SC: FR-01, FR-04).
    TEST_METHOD(ScheduleOnce_ReturnsValidId)
    {
        std::atomic<int> counter { 0 };

        auto id = Tq().ScheduleOnce(
            50ms,
            [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); },
            "U01-ReturnsValidId");

        Assert::AreNotEqual(LibCommons::kInvalidTimerId, id,
            L"ScheduleOnce should return a non-invalid TimerId");

        // 콜백이 실행되어 카운터가 1이 될 때까지 대기.
        std::this_thread::sleep_for(300ms);

        Assert::AreEqual(1, counter.load(std::memory_order_relaxed),
            L"One-shot callback should fire exactly once");
    }

    // U-02: ScheduleOnce 가 정확히 한 번만 발사 (Plan SC: FR-01).
    TEST_METHOD(ScheduleOnce_RunsExactlyOnce)
    {
        std::atomic<int> counter { 0 };

        Tq().ScheduleOnce(
            30ms,
            [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); },
            "U02-RunsOnce");

        std::this_thread::sleep_for(500ms);

        Assert::AreEqual(1, counter.load(std::memory_order_relaxed),
            L"One-shot timer must fire exactly once, not multiple times");
    }

    // U-03: SchedulePeriodic 가 반복 발사 (Plan SC: FR-02).
    //       CRITICAL: 싱글톤에 예약한 periodic 타이머는 테스트 종료 후에도 지속되어 dangling
    //       ref(스택 counter) 에 write 하며 스택 오염 유발. 반드시 Cancel 로 정리.
    TEST_METHOD(SchedulePeriodic_FiresMultipleTimes)
    {
        std::atomic<int> counter { 0 };

        auto id = Tq().SchedulePeriodic(
            50ms,
            [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); },
            "U03-Periodic");

        // 50ms 주기로 약 5번 이상 발사될 시간 대기.
        std::this_thread::sleep_for(400ms);

        const int fires = counter.load(std::memory_order_relaxed);

        // 반드시 Cancel 로 정리 (wait 포함).
        Tq().Cancel(id);

        // 최소 3회 이상, 최대 15회 미만 (Windows 타이머 해상도/jitter 범위 고려)
        Assert::IsTrue(fires >= 3,
            L"Periodic timer should fire at least 3 times in 400ms with 50ms interval");
        Assert::IsTrue(fires < 15,
            L"Periodic timer fire count unexpectedly high (jitter abnormality)");
    }

    // U-04: 여러 타이머가 독립적으로 동작 (Plan SC: FR-01).
    TEST_METHOD(MultipleTimers_Independent)
    {
        std::atomic<int> counterA { 0 };
        std::atomic<int> counterB { 0 };

        Tq().ScheduleOnce(30ms, [&counterA]() { counterA.fetch_add(1); }, "U04-A");
        Tq().ScheduleOnce(30ms, [&counterB]() { counterB.fetch_add(1); }, "U04-B");

        std::this_thread::sleep_for(300ms);

        Assert::AreEqual(1, counterA.load(), L"Timer A should fire once");
        Assert::AreEqual(1, counterB.load(), L"Timer B should fire once");
    }

    // U-05: Command 경로 (ITimerCommand::Execute 호출, Plan SC: FR-05, FR-06).
    TEST_METHOD(Command_Execute)
    {
        std::atomic<int> counter { 0 };
        auto cmd = std::make_unique<CountingCommand>(&counter, "U05-CommandCounter");

        auto id = Tq().ScheduleOnce(30ms, std::move(cmd));

        Assert::AreNotEqual(LibCommons::kInvalidTimerId, id,
            L"Command overload should return valid TimerId");

        std::this_thread::sleep_for(300ms);

        Assert::AreEqual(1, counter.load(), L"ITimerCommand::Execute should be called once");
    }

    // U-06: Command 가 nullptr 이면 kInvalidTimerId 반환 (Plan SC: FR-05).
    TEST_METHOD(Command_NullPtr_ReturnsInvalid)
    {
        std::unique_ptr<LibCommons::ITimerCommand> nullCmd;

        auto id = Tq().ScheduleOnce(10ms, std::move(nullCmd));

        Assert::AreEqual(LibCommons::kInvalidTimerId, id,
            L"Null command should yield kInvalidTimerId");
    }

    // U-07: Periodic Command 경로 (Plan SC: FR-02, FR-05 — 결합).
    //       U-03 과 동일 이유로 Cancel 필수.
    TEST_METHOD(Command_SchedulePeriodic)
    {
        std::atomic<int> counter { 0 };
        auto cmd = std::make_unique<CountingCommand>(&counter, "U07-PeriodicCommand");

        auto id = Tq().SchedulePeriodic(50ms, std::move(cmd));

        Assert::AreNotEqual(LibCommons::kInvalidTimerId, id);

        std::this_thread::sleep_for(350ms);

        const int fires = counter.load();

        Tq().Cancel(id);

        Assert::IsTrue(fires >= 3,
            L"Periodic command should fire at least 3 times");
    }

    // U-08: Cancel 미실행 타이머 취소 시 true 반환 + 콜백 미실행 (Plan SC: FR-03).
    TEST_METHOD(Cancel_PendingTimer_ReturnsTrue)
    {
        std::atomic<int> counter { 0 };
        auto id = Tq().ScheduleOnce(500ms,
            [&counter]() { counter.fetch_add(1); },
            "U08-CancelPending");
        Assert::AreNotEqual(LibCommons::kInvalidTimerId, id);

        // 발사 전에 취소.
        std::this_thread::sleep_for(50ms);
        bool cancelled = Tq().Cancel(id);

        Assert::IsTrue(cancelled, L"Cancel of pending timer should return true");

        // 원래 발사 예정 시점 지나도록 대기.
        std::this_thread::sleep_for(600ms);

        Assert::AreEqual(0, counter.load(), L"Cancelled timer must not fire");
    }

    // U-09: 이미 없는 id Cancel → false (Plan SC: FR-03).
    TEST_METHOD(Cancel_InvalidId_ReturnsFalse)
    {
        bool cancelled = Tq().Cancel(LibCommons::kInvalidTimerId);
        Assert::IsFalse(cancelled, L"Cancel with kInvalidTimerId must return false");

        // 아주 큰 id — 존재할 리 없음.
        bool cancelledPhantom = Tq().Cancel(9999999999ULL);
        Assert::IsFalse(cancelledPhantom, L"Cancel of non-existent id must return false");
    }

    // U-10: 두 번 Cancel → 두 번째는 false (Plan SC: FR-03).
    TEST_METHOD(Cancel_DoubleCancel_SecondReturnsFalse)
    {
        auto id = Tq().ScheduleOnce(1000ms, []() {}, "U10-DoubleCancel");

        bool first = Tq().Cancel(id);
        bool second = Tq().Cancel(id);

        Assert::IsTrue(first, L"First Cancel should succeed");
        Assert::IsFalse(second, L"Second Cancel should fail (already removed)");
    }

    // U-11: Periodic Cancel 후 추가 발사 없음 (Plan SC: FR-02, FR-03).
    TEST_METHOD(Cancel_Periodic_StopsFiring)
    {
        std::atomic<int> counter { 0 };
        auto id = Tq().SchedulePeriodic(40ms,
            [&counter]() { counter.fetch_add(1); },
            "U11-CancelPeriodic");

        std::this_thread::sleep_for(200ms);  // ~5회 발사
        const int beforeCancel = counter.load();

        Tq().Cancel(id);

        std::this_thread::sleep_for(200ms);  // 취소 후 대기
        const int afterCancel = counter.load();

        Assert::IsTrue(beforeCancel >= 3, L"Periodic should have fired multiple times before cancel");
        // 취소 직후 1건 정도는 진행 중일 수 있으나 장기적으로 추가 증가 없어야 함.
        Assert::IsTrue(afterCancel - beforeCancel <= 1,
            L"After cancel, periodic should stop firing (at most 1 in-flight)");
    }

    // U-12: ScopedTimer RAII — 소멸자가 실제 Cancel 수행 (Plan SC: FR-07).
    TEST_METHOD(ScopedTimer_DestructorCancelsTimer)
    {
        std::atomic<int> counter { 0 };
        auto id = Tq().ScheduleOnce(500ms, [&counter]() { counter.fetch_add(1); }, "U12-Scoped");
        Assert::AreNotEqual(LibCommons::kInvalidTimerId, id);

        {
            LibCommons::ScopedTimer scoped(Tq(), id);
            Assert::IsTrue(scoped.IsValid());
            Assert::AreEqual(id, scoped.Get());
        }  // 소멸자에서 Cancel 호출.

        // 원래 발사 예정 시점 지나도록 대기.
        std::this_thread::sleep_for(600ms);

        Assert::AreEqual(0, counter.load(),
            L"ScopedTimer destructor should cancel pending timer");
    }

    // U-13: ScopedTimer::Release 이후 소멸자가 Cancel 호출 안 함 → 타이머 그대로 발사 (Plan SC: FR-07).
    TEST_METHOD(ScopedTimer_ReleasePreservesTimer)
    {
        std::atomic<int> counter { 0 };
        auto id = Tq().ScheduleOnce(150ms, [&counter]() { counter.fetch_add(1); }, "U13-Release");

        {
            LibCommons::ScopedTimer scoped(Tq(), id);
            auto released = scoped.Release();
            Assert::AreEqual(id, released);
            Assert::IsFalse(scoped.IsValid());
        }  // 소멸자에서 Cancel 호출 안 함 (Release 이후).

        std::this_thread::sleep_for(400ms);

        Assert::AreEqual(1, counter.load(),
            L"After Release, timer should fire normally");
    }

    // U-14: ScopedTimer move — 이동 후 원본은 무효, 이동된 객체만 Cancel 책임 보유.
    TEST_METHOD(ScopedTimer_MoveTransfersOwnership)
    {
        auto id = Tq().ScheduleOnce(500ms, []() {}, "U14-Move");

        LibCommons::ScopedTimer original(Tq(), id);
        LibCommons::ScopedTimer moved(std::move(original));

        Assert::IsFalse(original.IsValid(), L"Moved-from ScopedTimer must be invalid");
        Assert::IsTrue(moved.IsValid());
        Assert::AreEqual(id, moved.Get());
    }

    // U-15: 격리된 TimerQueue 의 Shutdown 이 활성 타이머 모두 정리 (Plan SC: FR-08).
    //       싱글톤이 아닌 로컬 인스턴스 사용 (싱글톤은 다른 테스트와 공유되어 Shutdown 테스트 부적합).
    TEST_METHOD(Shutdown_CancelsAllActiveTimers)
    {
        LibCommons::TimerQueue localTq;

        std::atomic<int> counter { 0 };
        auto id1 = localTq.ScheduleOnce(500ms, [&counter]() { counter.fetch_add(1); }, "U15-A");
        auto id2 = localTq.SchedulePeriodic(100ms, [&counter]() { counter.fetch_add(1); }, "U15-B");

        Assert::AreNotEqual(LibCommons::kInvalidTimerId, id1);
        Assert::AreNotEqual(LibCommons::kInvalidTimerId, id2);

        std::this_thread::sleep_for(120ms);  // periodic 1회 정도 발사 시간
        const int beforeShutdown = counter.load();

        localTq.Shutdown(/*waitForCallbacks=*/true);

        std::this_thread::sleep_for(600ms);
        const int afterShutdown = counter.load();

        Assert::IsTrue(afterShutdown - beforeShutdown <= 1,
            L"Shutdown must stop all further callback fires");
    }

    // U-16: Shutdown 후 Schedule 거부 — kInvalidTimerId 반환 (Plan SC: FR-08).
    TEST_METHOD(Shutdown_BlocksNewSchedule)
    {
        LibCommons::TimerQueue localTq;
        localTq.Shutdown(true);

        std::atomic<int> counter { 0 };
        auto id = localTq.ScheduleOnce(30ms,
            [&counter]() { counter.fetch_add(1); },
            "U16-AfterShutdown");

        Assert::AreEqual(LibCommons::kInvalidTimerId, id,
            L"Schedule after Shutdown must return kInvalidTimerId");

        std::this_thread::sleep_for(100ms);
        Assert::AreEqual(0, counter.load(), L"Rejected schedule must not fire");
    }

    // U-17: Shutdown 두 번 호출 idempotent (Plan SC: FR-08).
    TEST_METHOD(Shutdown_Idempotent)
    {
        LibCommons::TimerQueue localTq;
        localTq.Shutdown(true);
        localTq.Shutdown(true);    // 두 번째는 no-op
        localTq.Shutdown(false);   // 세 번째도 no-op
        Assert::IsTrue(true, L"Multiple Shutdown calls should not crash");
    }

    // U-18: TimerQueue 소멸자가 Shutdown 호출 → 릭/UAF 없이 정리 (Plan SC: FR-07, FR-08).
    TEST_METHOD(Destructor_CleansUpActiveTimers)
    {
        std::atomic<int> counter { 0 };
        {
            LibCommons::TimerQueue localTq;
            localTq.SchedulePeriodic(50ms, [&counter]() { counter.fetch_add(1); }, "U18-Dtor");
            std::this_thread::sleep_for(120ms);
        }  // 소멸자 호출 → 내부 Shutdown(wait) → 모든 타이머 정리.

        const int beforeWait = counter.load();
        std::this_thread::sleep_for(300ms);
        const int afterWait = counter.load();

        Assert::AreEqual(beforeWait, afterWait,
            L"After TimerQueue destruction, no more callbacks should fire");
    }

    // U-19: 콜백 내 셀프 취소 — 데드락 없이 정상 동작 (Plan SC: FR-03, §4.4).
    TEST_METHOD(Cancel_SelfCancelInCallback_NoDeadlock)
    {
        LibCommons::TimerQueue localTq;
        std::atomic<int>     counter { 0 };
        std::atomic<LibCommons::TimerId> idHolder { LibCommons::kInvalidTimerId };

        auto id = localTq.SchedulePeriodic(30ms,
            [&counter, &idHolder, &localTq]() {
                counter.fetch_add(1);
                if (counter.load() == 2)
                {
                    // 콜백 내부에서 자기 자신 Cancel 호출 → 데드락 나면 이 테스트 timeout.
                    localTq.Cancel(idHolder.load());
                }
            },
            "U19-SelfCancel");

        idHolder.store(id);

        std::this_thread::sleep_for(300ms);

        const int fires = counter.load();
        Assert::IsTrue(fires >= 2, L"Periodic should fire at least 2 times before self-cancel");
        Assert::IsTrue(fires <= 4, L"After self-cancel, additional fires should be limited");
    }
};

} // namespace LibCommonsTests
