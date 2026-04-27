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

static void SendAndWaitResponse(const shared_ptr<IBenchmarkSession>& pSession, const uint32_t seq, BenchmarkWaiter& waiter, const uint32_t timeoutMs)
{
    waiter.Reset();

    fastport::protocols::benchmark::BenchmarkRequest request;
    request.set_client_timestamp_ns(HighResolutionTimer::NowNs());
    request.set_sequence(seq);

    pSession->SendMessage(PACKET_ID_BENCHMARK_REQUEST, request);

    waiter.Wait(timeoutMs);
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
    m_State.store(state);
    if (m_Callbacks.onStateChanged) m_Callbacks.onStateChanged(state);
}

void LatencyBenchmarkRunner::RunBenchmark()
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

        m_Service->Start(2);

        SetState(BenchmarkState::Connecting);

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

        atomic<uint32_t> lastReceivedSeq{ 0 };
        atomic<uint64_t> lastRecvTimestamp{ 0 };
        BenchmarkWaiter responseWaiter;

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
            for (size_t i = 0; i < m_Config.warmupIterations && !m_StopRequested.load(); ++i)
            {
                SendAndWaitResponse(pSession, static_cast<uint32_t>(i), responseWaiter, m_Config.timeoutMs);
            }
        }

        SetState(BenchmarkState::Running);
        string payload(m_Config.payloadSize, 'X');

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
            m_LatencyCollector.AddSample(rtt);

            if (m_Callbacks.onProgress && (i % 100 == 0 || i == m_Config.iterations - 1))
            {
                m_Callbacks.onProgress(i + 1, m_Config.iterations);
            }
        }

        m_Results = m_LatencyCollector.Calculate(m_Config.testName, m_Config.payloadSize);
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