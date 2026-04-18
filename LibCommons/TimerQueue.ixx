module;

#include <Windows.h>
#include <threadpoolapiset.h>

export module commons.timer_queue;

import std;
import commons.singleton;

// Design Ref: §4.1 — Public API module. Windows 타입은 GMF 안에서만 사용, export 심볼에는 노출하지 않는다.
// Design Ref: §2 Option B (Clean) — PImpl, 상태머신, ScopedTimer, 샤딩은 구현 파일에 은닉.

namespace LibCommons
{

export using TimerId  = std::uint64_t;
export using Duration = std::chrono::milliseconds;

export constexpr TimerId kInvalidTimerId = 0;

// Design Ref: §4.1 — Command 인터페이스. 메타데이터/정책(재시도 등) 필요 시 상속.
// Thread-safety: Execute()는 TP 풀 스레드에서 동시 호출될 수 있음 (Q3-a 겹침 허용). 호출자 책임.
export struct ITimerCommand
{
    virtual ~ITimerCommand() = default;

    virtual void Execute() = 0;

    virtual std::string_view Name() const noexcept = 0;
};

// Forward declaration — ScopedTimer 가 참조.
export class TimerQueue;

// Design Ref: §4.3 — RAII 핸들. 소멸 시 owning queue 에 Cancel 요청.
// Thread-safety: 단일 스레드 전용. 공유하려면 외부 동기화 필요.
export class ScopedTimer
{
public:
    ScopedTimer() noexcept = default;
    ScopedTimer(TimerQueue& queue, TimerId id) noexcept;
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    ScopedTimer(ScopedTimer&& other) noexcept;
    ScopedTimer& operator=(ScopedTimer&& other) noexcept;

    // 취소 책임 포기. 반환된 id 는 호출자가 직접 관리.
    TimerId Release() noexcept;

    TimerId Get() const noexcept { return m_Id; }
    bool IsValid() const noexcept { return m_Id != kInvalidTimerId; }

private:
    TimerQueue* m_pQueue = nullptr;
    TimerId     m_Id     = kInvalidTimerId;
};

// Design Ref: §4.1, §7.2 — public API.
// Plan SC: FR-01, FR-02, FR-03, FR-04, FR-05, FR-06, FR-07, FR-08, FR-09, FR-10.
// Singleton: 기존 LibCommons::SingleTon<> 패턴 상속. GetInstance() 는 상속받아 자동 제공.
// Thread-safety: 모든 public 메서드는 다중 스레드에서 동시 호출 안전.
//                단, 등록된 task 는 TP 풀 스레드에서 동시 실행될 수 있으므로 호출자 thread-safety 책임.
export class TimerQueue : public SingleTon<TimerQueue>
{
public:
    // Forward declaration only — definition hidden in TimerQueue.cpp (PImpl 패턴).
    // public 에 두는 이유는 out-of-line 멤버 정의(TimerQueue::Impl::...)에서 접근하기 위함이며,
    // 정의가 export 되지 않으므로 외부 소비자는 이 타입으로 어떤 작업도 할 수 없다.
    struct Impl;

private:
    friend class SingleTon<TimerQueue>;

    TimerQueue();

public:
    ~TimerQueue();

    TimerQueue(const TimerQueue&)            = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    // === 람다 경로 (일반 사용) ===
    // Design Ref: §4.1 — std::move_only_function 수용으로 move-only 캡처 허용 (unique_ptr 등).

    TimerId ScheduleOnce(Duration delay,
                         std::function<void()> task,
                         std::string_view name = "anonymous");

    TimerId SchedulePeriodic(Duration interval,
                             std::function<void()> task,
                             std::string_view name = "anonymous");

    // === 커맨드 경로 (메타데이터/정책 필요 시) ===
    // Design Ref: §4.1 — ITimerCommand 는 내부에서 람다로 감싸 TimerJob 으로 통일 저장.

    TimerId ScheduleOnce(Duration delay, std::unique_ptr<ITimerCommand> cmd);

    TimerId SchedulePeriodic(Duration interval, std::unique_ptr<ITimerCommand> cmd);

    // Design Ref: §2.2 Cancel flow — Fast/Wait path. true=실제 취소 or 이미 완료, false=id 없음/중복.
    bool Cancel(TimerId id);

    // Design Ref: §3.2 QueueState — Running → ShuttingDown → Dead 전이.
    // waitForCallbacks=true 면 진행 중 콜백 전부 완료까지 대기.
    void Shutdown(bool waitForCallbacks = true);

private:
    std::unique_ptr<Impl> m_pImpl;
};

} // namespace LibCommons
