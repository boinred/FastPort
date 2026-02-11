#include "CppUnitTest.h"
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <thread>
#include <chrono>
#include <iostream>
#include <string>
#include <format>

import commons.rwlock;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LibCommonsTests
{
    TEST_CLASS(LockBenchmarkTests)
    {
    public:
        // 1. std::mutex 성능 테스트
        TEST_METHOD(Benchmark_StdMutex)
        {
            const int TC = 8;
            const int OC = 1000000;
            
            std::mutex mtx;
            volatile int sharedData = 0;
            std::vector<std::thread> threads;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < TC; ++i)
            {
                threads.emplace_back([&mtx, &sharedData, TC, OC]() {
                    for (int j = 0; j < OC / TC; ++j)
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        sharedData = sharedData + 1;
                    }
                });
            }

            for (auto& t : threads) t.join();

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> elapsed = end - start;

            std::string msg = "std::mutex Time: " + std::to_string(elapsed.count()) + " ms";
            Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(msg.c_str());
        }

        // 2. LibCommons::RWLock (Write only) 성능 테스트
        TEST_METHOD(Benchmark_RWLock_WriteOnly)
        {
            const int TC = 8;
            const int OC = 1000000;

            LibCommons::RWLock rwLock;
            volatile int sharedData = 0;
            std::vector<std::thread> threads;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < TC; ++i)
            {
                threads.emplace_back([&rwLock, &sharedData, TC, OC]() {
                    for (int j = 0; j < OC / TC; ++j)
                    {
                        rwLock.WriteLock();
                        sharedData = sharedData + 1;
                        rwLock.WriteUnLock();
                    }
                });
            }

            for (auto& t : threads) t.join();

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> elapsed = end - start;

            std::string msg = "RWLock (WriteOnly) Time: " + std::to_string(elapsed.count()) + " ms";
            Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(msg.c_str());
        }

        // 3. LibCommons::RWLock (Read Heavy: 90% Read, 10% Write)
        TEST_METHOD(Benchmark_RWLock_ReadHeavy)
        {
            const int TC = 8;
            const int OC = 1000000;

            LibCommons::RWLock rwLock;
            volatile int sharedData = 0;
            std::vector<std::thread> threads;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < TC; ++i)
            {
                threads.emplace_back([&rwLock, &sharedData, TC, OC]() {
                    for (int j = 0; j < OC / TC; ++j)
                    {
                        if (j % 10 == 0)
                        {
                            rwLock.WriteLock();
                            sharedData = sharedData + 1;
                            rwLock.WriteUnLock();
                        }
                        else
                        {
                            rwLock.ReadLock();
                            int val = sharedData;
                            (void)val;
                            rwLock.ReadUnLock();
                        }
                    }
                });
            }

            for (auto& t : threads) t.join();

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> elapsed = end - start;

            std::string msg = "RWLock (Read 90) Time: " + std::to_string(elapsed.count()) + " ms";
            Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(msg.c_str());
        }

        // 4. std::mutex (Read Heavy 시뮬레이션)
        TEST_METHOD(Benchmark_StdMutex_ReadHeavy_Simulation)
        {
            const int TC = 8;
            const int OC = 1000000;

            std::mutex mtx;
            volatile int sharedData = 0;
            std::vector<std::thread> threads;

            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < TC; ++i)
            {
                threads.emplace_back([&mtx, &sharedData, TC, OC]() {
                    for (int j = 0; j < OC / TC; ++j)
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        if (j % 10 == 0)
                        {
                            sharedData = sharedData + 1;
                        }
                        else
                        {
                            int val = sharedData;
                            (void)val;
                        }
                    }
                });
            }

            for (auto& t : threads) t.join();

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> elapsed = end - start;

            std::string msg = "std::mutex (Read 90 Sim) Time: " + std::to_string(elapsed.count()) + " ms";
            Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(msg.c_str());
        }
    };
}
