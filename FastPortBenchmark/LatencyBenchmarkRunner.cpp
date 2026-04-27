module;

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <spdlog/spdlog.h>

#include "Protocols/Benchmark.pb.h"

module benchmark.latency_runner;

import std;
import benchmark.session;
import networks.core.io_socket_connector;
import networks.core.socket;
import networks.core.packet;
import networks.services.io_service;
import networks.services.rio_service;
import networks.services.inetwork_service;
import networks.sessions.inetwork_session;
import commons.buffers.circle_buffer_queue;
import commons.logger;
import networks.core.rio_buffer_manager;

namespace FastPortBenchmark
{
using namespace std;

static bool SendAndWaitResponse(const shared_ptr<IBenchmarkSession>& pSession, const uint32_t seq, BenchmarkWaiter& waiter, const uint32_t timeoutMs)
{
    waiter.Reset();

    fastport::protocols::benchmark::BenchmarkRequest request;
    request.set_client_timestamp_ns(HighResolutionTimer::NowNs());
    request.set_sequence(seq);

    pSession->SendMessage(PACKET_ID_BENCHMARK_REQUEST, request);

    return waiter.Wait(timeoutMs);
}

LatencyBenchmarkRunner::LatencyBenchmarkRunner()
    : m_State(BenchmarkState::Idle)
{
}

LatencyBenchmarkRunner::~LatencyBenchmarkRunner()
{
    if (m_Service) m_Service->Stop();
    Stop();
}

bool LatencyBenchmarkRunner::Start(const BenchmarkConfig& config, const BenchmarkCallbacks& callbacks)
{
    if (m_State != BenchmarkState::Idle) return false;

    m_Config = config;
    m_Callbacks = callbacks;
    m_StopRequested.store(false);

    m_LatencyCollector.Clear();
    m_LatencyCollector.Reserve(config.iterations);

    m_RunnerThread = thread([this]() { RunBenchmark(); });
    return true;
}

void LatencyBenchmarkRunner::Stop()
{
    m_StopRequested.store(true);
    if (m_RunnerThread.joinable()) m_RunnerThread.join();
    SetState(BenchmarkState::Idle);
}

BenchmarkState LatencyBenchmarkRunner::GetState() const { return m_State.load(); }
BenchmarkStats LatencyBenchmarkRunner::GetResults() const { return m_Results; }

void LatencyBenchmarkRunner::SetState(BenchmarkState state)
{
    const auto previous = m_State.load(std::memory_order_acquire);
    if (state != BenchmarkState::Idle &&
        (previous == state || previous == BenchmarkState::Completed || previous == BenchmarkState::Failed))
    {
        return;
    }

    m_State.store(state);
    if (m_Callbacks.onStateChanged) m_Callbacks.onStateChanged(state);
}

void LatencyBenchmarkRunner::RunBenchmark()
{
    try
    {
        const bool fixedSingleSession =
            m_Config.sessionCount <= 1 &&
            (m_Config.payloadMinSize == 0 || m_Config.payloadMinSize == m_Config.payloadSize) &&
            (m_Config.payloadMaxSize == 0 || m_Config.payloadMaxSize == m_Config.payloadSize);

        if (fixedSingleSession)
        {
            RunSequentialBenchmark();
        }
        else
        {
            RunMultiSessionBenchmark();
        }
    }
    catch (const std::exception& ex)
    {
        SetState(BenchmarkState::Failed);
        if (m_Callbacks.onError) m_Callbacks.onError(ex.what());
    }
}

std::vector<std::string> LatencyBenchmarkRunner::BuildPayloadPool() const
{
    const size_t minSize = m_Config.payloadMinSize == 0 ? m_Config.payloadSize : m_Config.payloadMinSize;
    const size_t maxSize = m_Config.payloadMaxSize == 0 ? minSize : m_Config.payloadMaxSize;
    const size_t low = (std::min)(minSize, maxSize);
    const size_t high = (std::max)(minSize, maxSize);
    const size_t poolSize = (std::max<size_t>)(1, m_Config.payloadPoolSize);

    std::vector<std::string> payloads;
    payloads.reserve(poolSize);

    std::mt19937_64 rng{ 0x4650535045524655ULL };
    std::uniform_int_distribution<size_t> sizeDist(low, high);

    for (size_t i = 0; i < poolSize; ++i)
    {
        std::string payload(sizeDist(rng), '\0');
        for (size_t j = 0; j < payload.size(); ++j)
        {
            payload[j] = static_cast<char>('A' + ((i + j) % 26));
        }
        payloads.push_back(std::move(payload));
    }

    return payloads;
}

void LatencyBenchmarkRunner::RunSequentialBenchmark()
{
    try
    {
        if (m_Config.useRio)
        {
            auto rioService = make_shared<LibNetworks::Services::RIOService>();
            // RIO Service 초기화 (버퍼 크기 등 설정)
            // 예: MaxSessions=100, MaxRecv=16KB, MaxSend=16KB
            if (!rioService->Initialize(100))
            {
                throw runtime_error("Failed to initialize RIO Service");
            }
            m_Service = rioService;
        }
        else
        {
            m_Service = make_shared<LibNetworks::Services::IOService>();
        }

        m_Service->Start(m_Config.ioThreadCount);

        SetState(BenchmarkState::Connecting);
        const uint64_t connectStartNs = HighResolutionTimer::NowNs();

        shared_ptr<IBenchmarkSession> pSession;
        mutex sessionMutex;
        condition_variable sessionCv;
        bool connected = false;

        LibNetworks::Core::IOSocketConnector::Create(
            m_Service,
            [&](const shared_ptr<LibNetworks::Core::Socket>& pSocket)
            -> shared_ptr<LibNetworks::Sessions::INetworkSession>
            {
                shared_ptr<IBenchmarkSession> pBenchmarkSession;

                if (m_Config.useRio)
                {
                    auto rioBufferManager = std::make_shared<LibNetworks::Core::RioBufferManager>();

                    auto rioService = static_pointer_cast<LibNetworks::Services::RIOService>(m_Service);
                    auto& bufferManager = rioBufferManager;

                    rioBufferManager->Initialize(64 * 1024 * 16); // 16MB Pool

                    LibNetworks::Core::RioBufferSlice recvSlice{};
                    LibNetworks::Core::RioBufferSlice sendSlice{};

                    bufferManager->AllocateSlice(16 * 1024, recvSlice);
                    bufferManager->AllocateSlice(16 * 1024, sendSlice);


                    auto pRioSession = make_shared<BenchmarkSessionRIO>(pSocket, recvSlice, sendSlice, rioService->GetCompletionQueue());
                    if (!pRioSession->Initialize())
                    {
                        LibCommons::Logger::GetInstance().LogError("LatencyBenchmarkRunner", "Failed to initialize RIO Benchmark Session");

                        return nullptr;
                    }

                    pBenchmarkSession = pRioSession;
                }
                else
                {
                    pBenchmarkSession = make_shared<BenchmarkSessionIOCP>(
                        pSocket,
                        make_unique<LibCommons::Buffers::CircleBufferQueue>(64 * 1024),
                        make_unique<LibCommons::Buffers::CircleBufferQueue>(64 * 1024)
                    );
                }

                pBenchmarkSession->SetConnectHandler([&]()
                    {
                        lock_guard<mutex> lock(sessionMutex);
                        connected = true;
                        sessionCv.notify_all();
                    });

                pBenchmarkSession->SetDisconnectHandler([&]()
                    {
                        if (m_State != BenchmarkState::Completed)
                        {
                            SetState(BenchmarkState::Failed);
                            if (m_Callbacks.onError) m_Callbacks.onError("Connection lost");
                        }
                    });

                pSession = pBenchmarkSession;
                // INetworkSession으로 형변환하여 반환 (dynamic_pointer_cast 필요할 수 있음)
                // BenchmarkSessionIOCP/RIO는 모두 INetworkSession을 상속받음.
                // 하지만 IBenchmarkSession은 아님. 다중 상속.
                // std::shared_ptr은 똑똑해서 알아서 처리함.
                if (m_Config.useRio)
                    return static_pointer_cast<LibNetworks::Sessions::INetworkSession>(static_pointer_cast<BenchmarkSessionRIO>(pBenchmarkSession));
                else
                    return static_pointer_cast<LibNetworks::Sessions::INetworkSession>(static_pointer_cast<BenchmarkSessionIOCP>(pBenchmarkSession));
            },
            m_Config.serverHost.c_str(),
            m_Config.serverPort
        );

        {
            unique_lock<mutex> lock(sessionMutex);
            if (!sessionCv.wait_for(lock, chrono::milliseconds(m_Config.timeoutMs),
                [&] { return connected; }))
            {
                SetState(BenchmarkState::Failed);
                if (m_Callbacks.onError) m_Callbacks.onError("Connection timeout");
                return;
            }
        }
        const uint64_t connectElapsedNs = HighResolutionTimer::NowNs() - connectStartNs;

        atomic<uint32_t> lastReceivedSeq{ 0 };
        atomic<uint64_t> lastRecvTimestamp{ 0 };
        BenchmarkWaiter responseWaiter;
        size_t warmupResponses = 0;
        uint64_t warmupElapsedNs = 0;

        pSession->SetPacketHandler([&](const LibNetworks::Core::Packet& packet)
            {
                if (packet.GetPacketId() == PACKET_ID_BENCHMARK_RESPONSE)
                {
                    fastport::protocols::benchmark::BenchmarkResponse response;
                    if (packet.ParseMessage(response))
                    {
                        lastReceivedSeq.store(response.sequence());
                        lastRecvTimestamp.store(response.client_timestamp_ns());
                        responseWaiter.Signal();
                    }
                }
            });

        if (m_Config.warmupIterations > 0)
        {
            SetState(BenchmarkState::Warmup);
            const uint64_t warmupStartNs = HighResolutionTimer::NowNs();
            for (size_t i = 0; i < m_Config.warmupIterations && !m_StopRequested.load(); ++i)
            {
                if (SendAndWaitResponse(pSession, static_cast<uint32_t>(i), responseWaiter, m_Config.timeoutMs))
                {
                    ++warmupResponses;
                }
            }
            warmupElapsedNs = HighResolutionTimer::NowNs() - warmupStartNs;
        }

        SetState(BenchmarkState::Running);
        auto payloads = BuildPayloadPool();
        const auto& payload = payloads.front();
        const uint64_t measuredStartNs = HighResolutionTimer::NowNs();

        for (size_t i = 0; i < m_Config.iterations && !m_StopRequested.load(); ++i)
        {
            responseWaiter.Reset();
            uint64_t sendTime = HighResolutionTimer::NowNs();

            fastport::protocols::benchmark::BenchmarkRequest request;
            request.mutable_header()->set_request_id(i);
            request.mutable_header()->set_timestamp_ms(
                chrono::duration_cast<chrono::milliseconds>(
                    chrono::system_clock::now().time_since_epoch()).count());
            request.set_client_timestamp_ns(sendTime);
            request.set_sequence(static_cast<uint32_t>(i));
            request.set_payload(payload);

            pSession->SendMessage(PACKET_ID_BENCHMARK_REQUEST, request);

            if (!responseWaiter.Wait(m_Config.timeoutMs)) continue;

            uint64_t recvTime = HighResolutionTimer::NowNs();
            uint64_t rtt = recvTime - sendTime;
            m_LatencyCollector.AddSample(rtt, payload.size());

            if (m_Callbacks.onProgress && (i % 100 == 0 || i == m_Config.iterations - 1))
            {
                m_Callbacks.onProgress(i + 1, m_Config.iterations);
            }
        }

        m_Results = m_LatencyCollector.Calculate(m_Config.testName, m_Config.payloadSize);
        m_Results.debugProfileEnabled = true;
        m_Results.requestedSessions = 1;
        m_Results.connectedSessions = 1;
        m_Results.connectionLosses = 0;
        m_Results.warmupRequests = m_Config.warmupIterations;
        m_Results.warmupResponses = warmupResponses;
        m_Results.measuredRequests = m_Config.iterations;
        m_Results.measuredResponses = m_Results.iterations;
        m_Results.payloadMinBytes = payload.size();
        m_Results.payloadMaxBytes = payload.size();
        m_Results.payloadPoolSize = payloads.size();
        m_Results.connectElapsedNs = connectElapsedNs;
        m_Results.warmupElapsedNs = warmupElapsedNs;
        m_Results.measuredElapsedNs = HighResolutionTimer::NowNs() - measuredStartNs;
        SetState(BenchmarkState::Completed);
        if (m_Callbacks.onCompleted) m_Callbacks.onCompleted(m_Results);

        // Graceful teardown — iosession-lifetime-race §2.3 Rule D1 준수.
        // 지역 pSession 이 scope 을 벗어나기 전에 RequestDisconnect 로 outstanding I/O 를
        // drain 하고, OnDisconnected 까지 기다려 `counter==0 && fired==true` 불변식을
        // 확보한 후에만 세션을 소멸시킨다. 그렇지 않으면 ~IOSession 시점에 Recv 가
        // pending 상태로 남아 IOCP 워커가 해제된 포인터에 접근하며 AV 발생 (Exit code
        // 0xC0000005). m_Service->Stop() 순서만으로는 이 race 를 봉쇄할 수 없다.
        DisconnectAndWaitDrain(pSession);
    }
    catch (const std::exception& ex)
    {
        SetState(BenchmarkState::Failed);
        if (m_Callbacks.onError) m_Callbacks.onError(ex.what());
    }
}

void LatencyBenchmarkRunner::RunMultiSessionBenchmark()
{
    if (m_Config.useRio)
    {
        throw runtime_error("Multi-session random payload benchmark currently supports IOCP mode only");
    }

    m_Service = make_shared<LibNetworks::Services::IOService>();
    m_Service->Start(m_Config.ioThreadCount);

    const auto payloads = BuildPayloadPool();
    const size_t sessionCount = (std::max<size_t>)(1, m_Config.sessionCount);
    const size_t totalTarget = (std::max<size_t>)(1, m_Config.iterations);
    const size_t warmupTarget = m_Config.warmupIterations * sessionCount;

    SetState(BenchmarkState::Connecting);
    const uint64_t connectStartNs = HighResolutionTimer::NowNs();

    struct SessionContext
    {
        std::shared_ptr<IBenchmarkSession> Session;
        std::atomic<bool> Connected{ false };
        std::atomic<bool> Active{ true };
        std::atomic<uint32_t> Sequence{ 0 };
        std::atomic<uint64_t> SendTimeNs{ 0 };
        std::atomic<size_t> PayloadIndex{ 0 };
        size_t SessionIndex = 0;
    };

    std::mutex connectMutex;
    std::condition_variable connectCv;
    std::mutex doneMutex;
    std::condition_variable doneCv;
    std::atomic<bool> abortRequested{ false };
    std::atomic<size_t> disconnectedCount{ 0 };
    size_t connectedCount = 0;
    std::vector<std::shared_ptr<SessionContext>> contexts;
    std::vector<std::shared_ptr<LibNetworks::Core::IOSocketConnector>> connectors;
    contexts.reserve(sessionCount);
    connectors.reserve(sessionCount);

    for (size_t i = 0; i < sessionCount; ++i)
    {
        auto ctx = std::make_shared<SessionContext>();
        ctx->SessionIndex = i;
        contexts.push_back(ctx);

        auto connector = LibNetworks::Core::IOSocketConnector::Create(
            m_Service,
            [&, ctx](const shared_ptr<LibNetworks::Core::Socket>& pSocket)
            -> shared_ptr<LibNetworks::Sessions::INetworkSession>
            {
                auto pBenchmarkSession = make_shared<BenchmarkSessionIOCP>(
                    pSocket,
                    make_unique<LibCommons::Buffers::CircleBufferQueue>(256 * 1024),
                    make_unique<LibCommons::Buffers::CircleBufferQueue>(256 * 1024)
                );

                pBenchmarkSession->SetConnectHandler([&, ctx]()
                {
                    ctx->Connected.store(true, std::memory_order_release);
                    std::lock_guard<mutex> lock(connectMutex);
                    ++connectedCount;
                    connectCv.notify_all();
                });

                pBenchmarkSession->SetDisconnectHandler([&, ctx]()
                {
                    ctx->Active.store(false, std::memory_order_release);
                    if (m_State != BenchmarkState::Completed && !m_StopRequested.load())
                    {
                        abortRequested.store(true, std::memory_order_release);
                        const size_t disconnected = disconnectedCount.fetch_add(1, std::memory_order_acq_rel) + 1;
                        SetState(BenchmarkState::Failed);
                        if (disconnected == 1 && m_Callbacks.onError)
                        {
                            m_Callbacks.onError("Connection lost during multi-session benchmark");
                        }
                        connectCv.notify_all();
                        doneCv.notify_all();
                    }
                });

                ctx->Session = pBenchmarkSession;
                return static_pointer_cast<LibNetworks::Sessions::INetworkSession>(pBenchmarkSession);
            },
            m_Config.serverHost.c_str(),
            m_Config.serverPort
        );

        if (!connector)
        {
            throw runtime_error("Failed to create benchmark connector");
        }

        connectors.push_back(connector);
    }

    {
        unique_lock<mutex> lock(connectMutex);
        if (!connectCv.wait_for(lock, chrono::milliseconds(m_Config.timeoutMs),
            [&] { return connectedCount == sessionCount || abortRequested.load(std::memory_order_acquire); }))
        {
            SetState(BenchmarkState::Failed);
            if (m_Callbacks.onError) m_Callbacks.onError("Connection timeout while opening benchmark sessions");
            return;
        }

        if (abortRequested.load(std::memory_order_acquire))
        {
            return;
        }
    }
    const uint64_t connectElapsedNs = HighResolutionTimer::NowNs() - connectStartNs;

    std::atomic<size_t> nextWarmup{ 0 };
    std::atomic<size_t> completedWarmup{ 0 };
    std::atomic<size_t> nextMeasured{ 0 };
    std::atomic<size_t> completedMeasured{ 0 };
    std::atomic<bool> runningMeasured{ false };
    uint64_t warmupElapsedNs = 0;

    std::mutex sampleMutex;

    auto sendNext = [&](const std::shared_ptr<SessionContext>& ctx, bool measured)
    {
        if (!ctx || !ctx->Session || !ctx->Active.load(std::memory_order_acquire) || m_StopRequested.load())
        {
            return false;
        }

        size_t ticket = measured ? nextMeasured.fetch_add(1) : nextWarmup.fetch_add(1);
        size_t limit = measured ? totalTarget : warmupTarget;
        if (ticket >= limit)
        {
            return false;
        }

        const size_t payloadIndex = (ticket + ctx->SessionIndex * 131) % payloads.size();
        const auto& payload = payloads[payloadIndex];
        const uint64_t sendTime = HighResolutionTimer::NowNs();

        fastport::protocols::benchmark::BenchmarkRequest request;
        request.mutable_header()->set_request_id(ticket);
        request.mutable_header()->set_timestamp_ms(
            chrono::duration_cast<chrono::milliseconds>(
                chrono::system_clock::now().time_since_epoch()).count());
        request.set_client_timestamp_ns(sendTime);
        request.set_sequence(ctx->Sequence.fetch_add(1, std::memory_order_relaxed));
        request.set_payload(payload);

        ctx->PayloadIndex.store(payloadIndex, std::memory_order_release);
        ctx->SendTimeNs.store(sendTime, std::memory_order_release);
        ctx->Session->SendMessage(PACKET_ID_BENCHMARK_REQUEST, request);
        return true;
    };

    for (auto& ctx : contexts)
    {
        ctx->Session->SetPacketHandler([&, ctx](const LibNetworks::Core::Packet& packet)
        {
            if (packet.GetPacketId() != PACKET_ID_BENCHMARK_RESPONSE)
            {
                return;
            }

            fastport::protocols::benchmark::BenchmarkResponse response;
            if (!packet.ParseMessage(response))
            {
                return;
            }

            if (!runningMeasured.load(std::memory_order_acquire))
            {
                const size_t done = completedWarmup.fetch_add(1, std::memory_order_acq_rel) + 1;
                if (!sendNext(ctx, false) && done >= warmupTarget)
                {
                    std::lock_guard<mutex> lock(doneMutex);
                    doneCv.notify_all();
                }
                return;
            }

            const uint64_t recvTime = HighResolutionTimer::NowNs();
            const uint64_t sendTime = ctx->SendTimeNs.load(std::memory_order_acquire);
            const size_t payloadIndex = ctx->PayloadIndex.load(std::memory_order_acquire);
            {
                std::lock_guard<mutex> lock(sampleMutex);
                m_LatencyCollector.AddSample(recvTime - sendTime, payloads[payloadIndex].size());
            }

            const size_t done = completedMeasured.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (m_Callbacks.onProgress && (done % 1000 == 0 || done == totalTarget))
            {
                m_Callbacks.onProgress(done, totalTarget);
            }

            if (!sendNext(ctx, true) && done >= totalTarget)
            {
                std::lock_guard<mutex> lock(doneMutex);
                doneCv.notify_all();
            }
        });
    }

    if (warmupTarget > 0)
    {
        SetState(BenchmarkState::Warmup);
        const uint64_t warmupStartNs = HighResolutionTimer::NowNs();
        for (auto& ctx : contexts)
        {
            sendNext(ctx, false);
        }

        std::unique_lock<mutex> lock(doneMutex);
        const bool warmupDone = doneCv.wait_for(lock, chrono::milliseconds(m_Config.timeoutMs * (m_Config.warmupIterations + 1)),
            [&] { return completedWarmup.load(std::memory_order_acquire) >= warmupTarget || m_StopRequested.load() || abortRequested.load(std::memory_order_acquire); });
        if (!warmupDone || m_StopRequested.load() || abortRequested.load(std::memory_order_acquire))
        {
            SetState(BenchmarkState::Failed);
            if (m_Callbacks.onError)
            {
                m_Callbacks.onError(abortRequested.load(std::memory_order_acquire) ? "Warmup aborted after connection loss" : "Warmup timeout");
            }
            return;
        }
        warmupElapsedNs = HighResolutionTimer::NowNs() - warmupStartNs;
    }

    SetState(BenchmarkState::Running);
    runningMeasured.store(true, std::memory_order_release);
    const uint64_t measuredStartNs = HighResolutionTimer::NowNs();

    for (auto& ctx : contexts)
    {
        sendNext(ctx, true);
    }

    {
        std::unique_lock<mutex> lock(doneMutex);
        const bool measuredDone = doneCv.wait_for(lock, chrono::milliseconds(m_Config.timeoutMs * (totalTarget / sessionCount + 2)),
            [&] { return completedMeasured.load(std::memory_order_acquire) >= totalTarget || m_StopRequested.load() || abortRequested.load(std::memory_order_acquire); });
        if (!measuredDone || m_StopRequested.load() || abortRequested.load(std::memory_order_acquire))
        {
            SetState(BenchmarkState::Failed);
            if (m_Callbacks.onError)
            {
                m_Callbacks.onError(abortRequested.load(std::memory_order_acquire) ? "Measured run aborted after connection loss" : "Measured run timeout");
            }
            return;
        }
    }

    const uint64_t measuredElapsedNs = HighResolutionTimer::NowNs() - measuredStartNs;
    m_Results = m_LatencyCollector.Calculate(m_Config.testName, m_Config.payloadMaxSize == 0 ? m_Config.payloadSize : m_Config.payloadMaxSize);
    m_Results.totalElapsedNs = measuredElapsedNs;
    m_Results.debugProfileEnabled = true;
    m_Results.requestedSessions = sessionCount;
    m_Results.connectedSessions = connectedCount;
    m_Results.connectionLosses = disconnectedCount.load(std::memory_order_acquire);
    m_Results.warmupRequests = warmupTarget;
    m_Results.warmupResponses = completedWarmup.load(std::memory_order_acquire);
    m_Results.measuredRequests = totalTarget;
    m_Results.measuredResponses = completedMeasured.load(std::memory_order_acquire);
    m_Results.payloadMinBytes = m_Config.payloadMinSize == 0 ? m_Config.payloadSize : (std::min)(m_Config.payloadMinSize, m_Config.payloadMaxSize == 0 ? m_Config.payloadMinSize : m_Config.payloadMaxSize);
    m_Results.payloadMaxBytes = m_Config.payloadMaxSize == 0 ? m_Results.payloadMinBytes : (std::max)(m_Config.payloadMinSize == 0 ? m_Config.payloadSize : m_Config.payloadMinSize, m_Config.payloadMaxSize);
    m_Results.payloadPoolSize = payloads.size();
    m_Results.connectElapsedNs = connectElapsedNs;
    m_Results.warmupElapsedNs = warmupElapsedNs;
    m_Results.measuredElapsedNs = measuredElapsedNs;
    const double elapsedSec = HighResolutionTimer::ToSeconds(measuredElapsedNs);
    if (elapsedSec > 0.0)
    {
        m_Results.packetsPerSecond = static_cast<double>(m_Results.iterations) / elapsedSec;
        m_Results.megabytesPerSecond = static_cast<double>(m_Results.totalBytes) / (1024.0 * 1024.0) / elapsedSec;
    }
    SetState(BenchmarkState::Completed);
    if (m_Callbacks.onCompleted) m_Callbacks.onCompleted(m_Results);

    for (auto& ctx : contexts)
    {
        DisconnectAndWaitDrain(ctx->Session);
    }
}

// Graceful teardown helper — Disconnect() 호출 후 DisconnectHandler 가 트리거될
// 때까지 condition_variable 로 대기. 타임아웃은 최후 방어선 (5s) 으로, 정상 경로는
// 수십~수백 ms 내에 drain 된다.
void LatencyBenchmarkRunner::DisconnectAndWaitDrain(const shared_ptr<IBenchmarkSession>& pSession)
{
    if (!pSession) return;

    std::mutex waitMutex;
    std::condition_variable waitCv;
    bool drained = false;

    pSession->SetDisconnectHandler([&]()
    {
        std::lock_guard<std::mutex> lock(waitMutex);
        drained = true;
        waitCv.notify_all();
    });

    pSession->Disconnect();

    std::unique_lock<std::mutex> lock(waitMutex);
    if (!waitCv.wait_for(lock, std::chrono::seconds(5), [&] { return drained; }))
    {
        LibCommons::Logger::GetInstance().LogError("LatencyBenchmarkRunner",
            "Teardown drain timeout — session 이 OnDisconnected 발화 전에 teardown 종료. "
            "Outstanding I/O 가 남아있을 수 있어 직후 세션 소멸 시 UAF 위험.");
    }
}

} // namespace FastPortBenchmark
