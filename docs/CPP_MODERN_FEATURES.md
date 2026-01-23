# C++ 최신 기능 활용 가이드

FastPort 프로젝트에서 활용 가능한 C++20/23 최신 기능과 최적화 포인트입니다.

---

## 📋 현재 상태

| 항목 | 현재 | 목표 |
|------|------|------|
| **C++ 표준** | C++20 | C++23 |
| **모듈** | ✅ 사용 중 | - |
| **span** | ❌ 미사용 | ✅ 적용 예정 |
| **jthread** | ❌ 미사용 | ✅ 적용 예정 |
| **concepts** | ❌ 미사용 | ✅ 적용 예정 |
| **expected** | ❌ 미사용 | ✅ 적용 예정 (C++23) |

---

# C++20 기능

## 1. `std::span` - 버퍼 전달 최적화

포인터 + 크기 쌍을 하나의 타입으로 안전하게 전달합니다.

### 현재 코드

```cpp
void SendBuffer(const unsigned char* pData, size_t dataLength);
bool Write(const void* pData, size_t size);
bool Peek(void* pOutData, size_t size);
bool Pop(void* pOutData, size_t size);
```

### C++20 적용

```cpp
#include <span>

void SendBuffer(std::span<const unsigned char> data);
bool Write(std::span<const std::byte> data);
bool Peek(std::span<std::byte> outData);
bool Pop(std::span<std::byte> outData);
```

### 장점

| 항목 | 설명 |
|------|------|
| **안전성** | 포인터/크기 불일치 실수 방지 |
| **편의성** | 범위 기반 for 사용 가능 |
| **성능** | Zero-overhead abstraction |
| **호환성** | `std::vector`, 배열, C 배열 모두 암시적 변환 |

### 사용 예시

```cpp
// 호출 측
std::vector<unsigned char> data = {...};
session.SendBuffer(data);  // 암시적 변환

// 배열도 가능
unsigned char arr[100];
session.SendBuffer(arr);

// subspan으로 부분 전달
session.SendBuffer(std::span(data).subspan(0, 50));
```

### 적용 대상 파일

- `LibNetworks/IOSession.ixx` - `SendBuffer()`
- `LibCommons/IBuffer.ixx` - `Write()`, `Peek()`, `Pop()`
- `LibCommons/CircleBufferQueue.ixx` - 버퍼 연산

---

## 2. `std::jthread` - 자동 관리 스레드

RAII 기반 스레드로 자동 join 및 stop_token 지원.

### 현재 코드

```cpp
// ThreadPool.ixx
std::vector<std::thread> m_Workers;

// 소멸자에서 수동 join 필요
~ThreadPool()
{
    m_bStop = true;
    m_Condition.notify_all();
    for (auto& worker : m_Workers)
    {
        if (worker.joinable())
            worker.join();
    }
}
```

### C++20 적용

```cpp
#include <thread>

std::vector<std::jthread> m_Workers;

// 소멸자에서 자동 join (명시적 코드 불필요)
~ThreadPool() = default;

// stop_token 활용
void WorkerFunction(std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        // 작업 처리
    }
}
```

### 장점

| 항목 | 설명 |
|------|------|
| **RAII** | 소멸 시 자동 join |
| **stop_token** | Cooperative cancellation 지원 |
| **간결함** | 수동 join/detach 코드 제거 |
| **안전성** | join 누락으로 인한 terminate 방지 |

### stop_token 활용 패턴

```cpp
void IOService::Start(size_t numThreads)
{
    for (size_t i = 0; i < numThreads; ++i)
    {
        m_Workers.emplace_back([this](std::stop_token token)
        {
            while (!token.stop_requested())
            {
                DWORD bytesTransferred = 0;
                ULONG_PTR completionKey = 0;
                OVERLAPPED* pOverlapped = nullptr;

                BOOL result = GetQueuedCompletionStatus(
                    m_hIOCP, &bytesTransferred, &completionKey, 
                    &pOverlapped, 100);  // 타임아웃으로 stop 체크

                if (token.stop_requested())
                    break;

                // 처리...
            }
        });
    }
}

void IOService::Stop()
{
    for (auto& worker : m_Workers)
    {
        worker.request_stop();  // Cooperative stop 요청
    }
    // jthread 소멸 시 자동 join
}
```

### 적용 대상 파일

- `LibCommons/ThreadPool.ixx`
- `LibNetworks/IOService.ixx`

---

## 3. Concepts - 템플릿 제약 명시

템플릿 매개변수에 대한 제약을 명시적으로 표현합니다.

### 현재 코드

```cpp
// Packet.ixx
template<class T>
bool ParseMessage(T& outMessage) const
{
    return outMessage.ParseFromString(m_Payload);
}
```

### C++20 적용

```cpp
#include <concepts>

// Concept 정의
template<typename T>
concept ProtobufMessage = requires(T t, const std::string& s) {
    { t.ParseFromString(s) } -> std::convertible_to<bool>;
    { t.SerializeToString(&s) } -> std::convertible_to<bool>;
};

// Concept 적용
template<ProtobufMessage T>
bool ParseMessage(T& outMessage) const
{
    return outMessage.ParseFromString(m_Payload);
}
```

### 장점

| 항목 | 설명 |
|------|------|
| **명확성** | 요구 사항이 코드로 문서화 |
| **에러 메시지** | 컴파일 에러가 명확해짐 |
| **오버로드** | Concept 기반 오버로드 가능 |
| **안전성** | 의도하지 않은 타입 사용 방지 |

### 추가 Concept 예시

```cpp
// 버퍼 타입 Concept
template<typename T>
concept BufferLike = requires(T t, const void* data, size_t size) {
    { t.Write(data, size) } -> std::convertible_to<bool>;
    { t.CanReadSize() } -> std::convertible_to<size_t>;
};

// 세션 팩토리 Concept
template<typename F, typename Session>
concept SessionFactory = requires(F f, std::shared_ptr<Socket> socket) {
    { f(socket) } -> std::convertible_to<std::shared_ptr<Session>>;
};

// 사용
template<SessionFactory<InboundSession> F>
void SetSessionFactory(F&& factory);
```

### 적용 대상 파일

- `LibNetworks/Packet.ixx` - `ParseMessage<T>`
- `LibNetworks/IOSocketListener.ixx` - 세션 팩토리 콜백
- `LibNetworks/IOSocketConnector.ixx` - 세션 팩토리 콜백

---

## 4. `std::format` - 타입 안전 문자열 포매팅

printf 스타일보다 안전하고 확장 가능한 포매팅.

### 현재 코드

```cpp
// spdlog 사용 (내부적으로 fmt 사용)
logger.LogInfo("Session", "Session Id: {}, Bytes: {}", sessionId, bytes);
```

### C++20 적용

```cpp
#include <format>

// 직접 문자열 생성 시
std::string msg = std::format("Session Id: {}, Bytes: {}", sessionId, bytes);

// 커스텀 타입 포매터
template<>
struct std::formatter<Packet> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }
    
    auto format(const Packet& p, std::format_context& ctx) const {
        return std::format_to(ctx.out(), 
            "Packet[id={}, size={}]", p.GetPacketId(), p.GetPacketSize());
    }
};

// 사용
Packet packet(...);
std::string s = std::format("Received: {}", packet);
```

### 장점

| 항목 | 설명 |
|------|------|
| **타입 안전** | 컴파일 타임 타입 체크 |
| **확장성** | 커스텀 타입 포매터 정의 가능 |
| **성능** | 최적화된 구현 |
| **표준** | 외부 라이브러리 의존 제거 가능 |

---

## 5. `constexpr` 확장

더 많은 연산을 컴파일 타임에 수행합니다.

### 현재 코드

```cpp
inline static constexpr size_t GetHeaderSize() { return sizeof(uint16_t); }
inline static constexpr size_t GetPacketIdSize() { return sizeof(uint16_t); }
```

### C++20 확장

```cpp
// 컴파일 타임 검증
static_assert(Packet::GetHeaderSize() + Packet::GetPacketIdSize() == 4);

// constexpr 버퍼 크기
static constexpr size_t DefaultRecvBufferSize = 16 * 1024;
static constexpr size_t MaxPacketSize = 65535;

// constexpr 함수 확장
constexpr bool IsValidPacketSize(uint16_t size) {
    return size >= GetHeaderSize() + GetPacketIdSize() 
        && size <= MaxPacketSize;
}

// constexpr std::vector (C++20)
constexpr auto CreateDefaultHeader() {
    std::vector<unsigned char> header(4);
    // ... 초기화
    return header;
}
```

---

## 6. `std::atomic<std::shared_ptr<T>>` - 원자적 스마트 포인터

### 현재 코드

```cpp
std::shared_ptr<Socket> m_pSocket;
// 스레드 안전성을 위해 별도 동기화 필요
```

### C++20 적용

```cpp
#include <atomic>
#include <memory>

std::atomic<std::shared_ptr<Socket>> m_pSocket;

// 원자적 연산
auto socket = m_pSocket.load();
m_pSocket.store(newSocket);

// CAS 연산
std::shared_ptr<Socket> expected = nullptr;
m_pSocket.compare_exchange_strong(expected, newSocket);
```

### 장점

| 항목 | 설명 |
|------|------|
| **스레드 안전** | 별도 락 없이 원자적 접근 |
| **간결함** | atomic_load/store 함수 대체 |
| **표준화** | C++20에서 공식 지원 |

---

## 7. Ranges - 범위 기반 알고리즘

### 사용 예시

```cpp
#include <ranges>
#include <algorithm>

// 기존
std::vector<Session> sessions;
auto it = std::find_if(sessions.begin(), sessions.end(),
    [id](const auto& s) { return s.GetId() == id; });

// C++20 Ranges
auto it = std::ranges::find_if(sessions,
    [id](const auto& s) { return s.GetId() == id; });

// 파이프라인
auto activeIds = sessions
    | std::views::filter([](const auto& s) { return s.IsActive(); })
    | std::views::transform([](const auto& s) { return s.GetId(); });
```

---

# C++23 기능

## 1. `std::expected` - 에러 처리 개선

성공 값 또는 에러를 하나의 타입으로 표현합니다.

### 현재 코드

```cpp
// bool 반환 + 로그로 에러 처리
bool PostRecv();
bool TryPostSendFromQueue();

// optional + enum으로 구분
std::optional<Packet> TryPop(...);
enum class PacketFrameResult { Ok, NeedMore, Invalid };
```

### C++23 적용

```cpp
#include <expected>

// 에러 타입 정의
enum class IOError {
    SocketClosed,
    BufferFull,
    WinsockError
};

enum class FrameError {
    NeedMore,
    InvalidHeader,
    SizeTooLarge
};

// expected 사용
std::expected<void, IOError> PostRecv();
std::expected<void, IOError> TryPostSendFromQueue();
std::expected<Packet, FrameError> TryPop(IBuffer& buffer);
```

### 사용 예시

```cpp
auto result = PostRecv();
if (!result) {
    switch (result.error()) {
    case IOError::SocketClosed:
        RequestDisconnect();
        break;
    case IOError::WinsockError:
        logger.LogError("IOSession", "WSARecv failed: {}", WSAGetLastError());
        break;
    }
    return;
}

// 또는 monadic 연산
PostRecv()
    .and_then([this]() { return ProcessData(); })
    .or_else([this](IOError e) { HandleError(e); });
```

### 장점

| 항목 | 설명 |
|------|------|
| **명시성** | 에러 가능성이 타입에 표현됨 |
| **안전성** | 에러 무시 방지 |
| **정보량** | 에러 종류를 값으로 전달 |
| **합성** | monadic 연산으로 체이닝 가능 |

### 적용 대상 파일

- `LibNetworks/IOSession.cpp` - `PostRecv()`, `TryPostSendFromQueue()`
- `LibNetworks/PacketFramer.ixx` - `TryPop()`
- `LibCommons/IBuffer.ixx` - `Write()`, `Pop()`

---

## 2. `std::print` - 간편한 출력

### C++23 적용

```cpp
#include <print>

// 기존
std::cout << "Session Id: " << sessionId << ", Bytes: " << bytes << std::endl;

// C++23
std::println("Session Id: {}, Bytes: {}", sessionId, bytes);
```

---

## 3. `std::mdspan` - 다차원 span

### 사용 예시

```cpp
#include <mdspan>

// 2D 버퍼 뷰
std::vector<unsigned char> buffer(width * height);
std::mdspan view(buffer.data(), width, height);
auto pixel = view[x, y];
```

---

## 4. `std::generator` - 코루틴 제너레이터

### 사용 예시

```cpp
#include <generator>

std::generator<Packet> ParsePackets(IBuffer& buffer) {
    while (true) {
        auto result = TryPop(buffer);
        if (!result) break;
        co_yield std::move(*result);
    }
}

// 사용
for (const auto& packet : ParsePackets(receiveBuffer)) {
    OnPacketReceived(packet);
}
```

---

# 📊 적용 우선순위

## 난이도 / 임팩트 매트릭스

```mermaid
quadrantChart
    title 적용 우선순위
    x-axis 낮은 난이도 --> 높은 난이도
    y-axis 낮은 임팩트 --> 높은 임팩트
    quadrant-1 우선 적용
    quadrant-2 장기 계획
    quadrant-3 선택적
    quadrant-4 빠른 적용
    
    std::span: [0.25, 0.8]
    std::jthread: [0.3, 0.6]
    Concepts: [0.5, 0.75]
    std::expected: [0.6, 0.85]
    constexpr: [0.2, 0.3]
    std::format: [0.35, 0.4]
    Ranges: [0.55, 0.5]
```

## 추천 적용 순서

| 순위 | 기능 | 난이도 | 임팩트 | 표준 |
|------|------|--------|--------|------|
| 1 | `std::span` | ⭐ | ⭐⭐⭐ | C++20 |
| 2 | `std::jthread` | ⭐ | ⭐⭐ | C++20 |
| 3 | Concepts | ⭐⭐ | ⭐⭐⭐ | C++20 |
| 4 | `std::expected` | ⭐⭐ | ⭐⭐⭐ | C++23 |
| 5 | `constexpr` 확장 | ⭐ | ⭐ | C++20 |
| 6 | `std::format` | ⭐ | ⭐ | C++20 |
| 7 | Ranges | ⭐⭐ | ⭐⭐ | C++20 |

---

# ✅ 체크리스트

## C++20 적용

- [ ] `std::span` - 버퍼 전달 인터페이스 변경
- [ ] `std::jthread` - ThreadPool, IOService 적용
- [ ] Concepts - 템플릿 함수 제약 추가
- [ ] `constexpr` - 컴파일 타임 검증 확대
- [ ] `std::format` - 커스텀 포매터 추가

## C++23 적용 (컴파일러 지원 시)

- [ ] `std::expected` - 에러 처리 리팩터링
- [ ] `std::print` - 디버그 출력 간소화
- [ ] `std::generator` - 패킷 파싱 코루틴화
