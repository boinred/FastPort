#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <WinSock2.h>
#include <WS2tcpip.h>

#include "BenchmarkStats.h"
#include "BenchmarkRunner.h"
#include "LatencyBenchmarkRunner.h"
#include "Protocols/Benchmark.pb.h"

import benchmark.session;
import networks.services.io_service;
import networks.core.io_socket_connector;
import networks.core.socket;
import networks.core.packet;
import commons.buffers.circle_buffer_queue;
import commons.logger;

namespace FastPortBenchmark
{

static void SendAndWaitResponse(
    const std::shared_ptr<BenchmarkSession>& pSession,
    const uint32_t seq,
    BenchmarkWaiter& waiter,
    const uint32_t timeoutMs)
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
    if (m_IoService) m_IoService->Stop();
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

    m_RunnerThread = std::thread([this]() { RunBenchmark(); });
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
        m_IoService = std::make_shared<LibNetworks::Services::IOService>();
        m_IoService->Start(2);

        SetState(BenchmarkState::Connecting);

        std::shared_ptr<BenchmarkSession> pSession;
        std::mutex sessionMutex;
        std::condition_variable sessionCv;
        bool connected = false;

        LibNetworks::Core::IOSocketConnector::Create(
            m_IoService,
            [&](const std::shared_ptr<LibNetworks::Core::Socket>& pSocket)
            -> std::shared_ptr<LibNetworks::Sessions::INetworkSession>
            {
                auto pBenchmarkSession = std::make_shared<BenchmarkSession>(
                    pSocket,
                    std::make_unique<LibCommons::Buffers::CircleBufferQueue>(64 * 1024),
                    std::make_unique<LibCommons::Buffers::CircleBufferQueue>(64 * 1024)
                );

                pBenchmarkSession->SetConnectHandler([&]()
                    {
                        std::lock_guard<std::mutex> lock(sessionMutex);
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
                return pBenchmarkSession;
            },
            m_Config.serverHost.c_str(),
            m_Config.serverPort
        );

        {
            std::unique_lock<std::mutex> lock(sessionMutex);
            if (!sessionCv.wait_for(lock, std::chrono::milliseconds(m_Config.timeoutMs),
                [&] { return connected; }))
            {
                SetState(BenchmarkState::Failed);
                if (m_Callbacks.onError) m_Callbacks.onError("Connection timeout");
                return;
            }
        }

        std::atomic<uint32_t> lastReceivedSeq{ 0 };
        std::atomic<uint64_t> lastRecvTimestamp{ 0 };
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
        std::string payload(m_Config.payloadSize, 'X');

        for (size_t i = 0; i < m_Config.iterations && !m_StopRequested.load(); ++i)
        {
            responseWaiter.Reset();
            uint64_t sendTime = HighResolutionTimer::NowNs();

            fastport::protocols::benchmark::BenchmarkRequest request;
            request.mutable_header()->set_request_id(i);
            request.mutable_header()->set_timestamp_ms(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
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
    }
    catch (const std::exception& ex)
    {
        SetState(BenchmarkState::Failed);
        if (m_Callbacks.onError) m_Callbacks.onError(ex.what());
    }
}

} // namespace FastPortBenchmark
