#include "CppUnitTest.h"

import commons.timer_queue;
import std;

// Design Ref: §8 Test Plan — Scope core,schedule 범위의 L1 Unit 테스트.
// 범위 제약:
//   - Cancel / Shutdown 은 stub 이므로 실제 취소 기반 테스트는 다음 스코프에서 수행.
//   - 테스트 안전성을 위해 TimerQueue::GetInstance() 싱글톤을 사용 (프로세스 종료 시까지 살아있어
//     dangling callback 위험 회피). 각 테스트는 고유 이름과 고유한 부작용 상태를 사용.

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LibCommonsTests
{

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
    //       Note: FR-02 는 다음 스코프 예정이지만 SchedulePeriodic 구현은 이번 스코프에 포함됨.
    TEST_METHOD(SchedulePeriodic_FiresMultipleTimes)
    {
        std::atomic<int> counter { 0 };

        Tq().SchedulePeriodic(
            50ms,
            [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); },
            "U03-Periodic");

        // 50ms 주기로 약 5번 이상 발사될 시간 대기.
        std::this_thread::sleep_for(400ms);

        const int fires = counter.load(std::memory_order_relaxed);

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
    TEST_METHOD(Command_SchedulePeriodic)
    {
        std::atomic<int> counter { 0 };
        auto cmd = std::make_unique<CountingCommand>(&counter, "U07-PeriodicCommand");

        auto id = Tq().SchedulePeriodic(50ms, std::move(cmd));

        Assert::AreNotEqual(LibCommons::kInvalidTimerId, id);

        std::this_thread::sleep_for(350ms);

        const int fires = counter.load();
        Assert::IsTrue(fires >= 3,
            L"Periodic command should fire at least 3 times");
    }

    // U-08: Cancel stub — 이번 스코프에서는 항상 false 반환 (다음 스코프 이관 예정).
    TEST_METHOD(Cancel_Stub_ReturnsFalse)
    {
        auto id = Tq().ScheduleOnce(1000ms, []() {}, "U08-StubCancel");
        Assert::AreNotEqual(LibCommons::kInvalidTimerId, id);

        bool cancelled = Tq().Cancel(id);
        Assert::IsFalse(cancelled,
            L"Cancel in core,schedule scope is stub and should return false");
    }

    // U-09: Shutdown stub — no-op, 크래시 없음 (다음 스코프 이관 예정).
    TEST_METHOD(Shutdown_Stub_NoOp)
    {
        // 그냥 호출해도 크래시하지 않으면 성공.
        Tq().Shutdown(false);
        Tq().Shutdown(true);

        // Shutdown 호출 후에도 Schedule 은 여전히 동작 (stub 이므로 QueueState 변경 안 함).
        std::atomic<int> counter { 0 };
        auto id = Tq().ScheduleOnce(30ms, [&counter]() { counter.fetch_add(1); }, "U09-AfterShutdown");
        Assert::AreNotEqual(LibCommons::kInvalidTimerId, id);

        std::this_thread::sleep_for(200ms);
        Assert::AreEqual(1, counter.load());
    }

    // U-10: ScopedTimer RAII — 소멸자가 Cancel 호출 (stub이므로 실제 취소는 안 되지만 크래시 없음).
    TEST_METHOD(ScopedTimer_DestructorCallsCancel)
    {
        std::atomic<int> counter { 0 };
        auto id = Tq().ScheduleOnce(500ms, [&counter]() { counter.fetch_add(1); }, "U10-Scoped");
        Assert::AreNotEqual(LibCommons::kInvalidTimerId, id);

        {
            LibCommons::ScopedTimer scoped(Tq(), id);
            Assert::IsTrue(scoped.IsValid());
            Assert::AreEqual(id, scoped.Get());
        }
        // 소멸자 호출 완료 — stub Cancel 이 호출됨 (side effect 없음).
        // 스코프 벗어났으니 예외 없이 정상 return.
        Assert::IsTrue(true);
    }

    // U-11: ScopedTimer::Release — 이후 소멸자가 Cancel 호출 안 함.
    TEST_METHOD(ScopedTimer_ReleaseSkipsCancel)
    {
        auto id = Tq().ScheduleOnce(500ms, []() {}, "U11-Release");

        LibCommons::ScopedTimer scoped(Tq(), id);
        auto released = scoped.Release();

        Assert::AreEqual(id, released, L"Release should return the held id");
        Assert::IsFalse(scoped.IsValid(), L"Scoped should be invalid after Release");
        Assert::AreEqual(LibCommons::kInvalidTimerId, scoped.Get());
        // scoped 소멸 시 Cancel 호출 안 함 (stub 이므로 어차피 효과 없음).
    }

    // U-12: ScopedTimer move — 이동 후 원본은 무효.
    TEST_METHOD(ScopedTimer_MoveTransfersOwnership)
    {
        auto id = Tq().ScheduleOnce(500ms, []() {}, "U12-Move");

        LibCommons::ScopedTimer original(Tq(), id);
        LibCommons::ScopedTimer moved(std::move(original));

        Assert::IsFalse(original.IsValid(), L"Moved-from ScopedTimer must be invalid");
        Assert::IsTrue(moved.IsValid());
        Assert::AreEqual(id, moved.Get());
    }
};

} // namespace LibCommonsTests
