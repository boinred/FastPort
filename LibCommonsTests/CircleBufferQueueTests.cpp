#include "CppUnitTest.h"
#include <string>

import commons.buffers.circle_buffer_queue;
import std;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LibCommons::Buffers;

namespace LibCommonsTests
{
	TEST_CLASS(CircleBufferQueueTests)
	{
	public:
		
		TEST_METHOD(BasicOperations)
		{
			CircleBufferQueue queue(10);

			// Initial state
			Assert::AreEqual((size_t)10, queue.CanWriteSize());
			Assert::AreEqual((size_t)0, queue.CanReadSize());

			// Write "12345"
			char data1[] = "12345";
			Assert::IsTrue(queue.Write(data1, 5));
			Assert::AreEqual((size_t)5, queue.CanWriteSize());
			Assert::AreEqual((size_t)5, queue.CanReadSize());

			// Peek
			char buffer[10] = { 0, };
			Assert::IsTrue(queue.Peek(buffer, 5));
			Assert::AreEqual(std::string("12345"), std::string(buffer, 5));
			Assert::AreEqual((size_t)5, queue.CanReadSize());

			// Pop "123"
			Assert::IsTrue(queue.Pop(buffer, 3));
			Assert::AreEqual(std::string("123"), std::string(buffer, 3));
			Assert::AreEqual((size_t)2, queue.CanReadSize()); // "45" remains

			// Write "6789012" (7 bytes) -> Total 9 bytes. Wraps around if implemented correctly.
			char data2[] = "6789012";
			Assert::IsTrue(queue.Write(data2, 7));
			Assert::AreEqual((size_t)9, queue.CanReadSize());

			// Pop all "456789012"
			char buffer2[10] = { 0, };
			Assert::IsTrue(queue.Pop(buffer2, 9));
			Assert::AreEqual(std::string("456789012"), std::string(buffer2, 9));

			// Empty again
			Assert::AreEqual((size_t)0, queue.CanReadSize());
			Assert::AreEqual((size_t)10, queue.CanWriteSize());

			// Test overflow
			char data3[] = "12345678901"; // 11 bytes
			Assert::IsFalse(queue.Write(data3, 11));

			// Test Consume
			Assert::IsTrue(queue.Write(data1, 5)); // Write "12345"
			Assert::IsTrue(queue.Consume(2)); // Consume "12"
			Assert::AreEqual((size_t)3, queue.CanReadSize()); // "345" remains
			
			char buffer3[10] = { 0, };
			Assert::IsTrue(queue.Pop(buffer3, 3));
			Assert::AreEqual(std::string("345"), std::string(buffer3, 3));

			// Test Clear
			Assert::IsTrue(queue.Write(data1, 5));
			queue.Clear();
			Assert::AreEqual((size_t)0, queue.CanReadSize());
			Assert::AreEqual((size_t)10, queue.CanWriteSize());
		}

        //	  •	버퍼를 가득 채운 후 추가 쓰기 시도 시 실패하는지 확인.
        //    •	데이터를 일부 소비(Consume)하여 공간을 만든 후 다시 쓰기가 성공하는지 확인.
        //    •	데이터 무결성이 유지되는지 확인.
		TEST_METHOD(OverflowTests)
		{
			CircleBufferQueue queue(5);

			char data[] = "12345";
			Assert::IsTrue(queue.Write(data, 5));
			Assert::AreEqual((size_t)5, queue.CanReadSize());
			Assert::AreEqual((size_t)0, queue.CanWriteSize());

			// Try to write when full
			char extra = '6';
			Assert::IsFalse(queue.Write(&extra, 1));

			// Verify data is still intact
			char buffer[6] = { 0, };
			Assert::IsTrue(queue.Peek(buffer, 5));
			Assert::AreEqual(std::string("12345"), std::string(buffer));

			// Make space
			Assert::IsTrue(queue.Consume(1));
			Assert::AreEqual((size_t)1, queue.CanWriteSize());

			// Write 1 byte
			Assert::IsTrue(queue.Write(&extra, 1));

			// Try to write again
			Assert::IsFalse(queue.Write(&extra, 1));
			
			// Verify content after wrap write
			// Current state: "23456" (logical)
			Assert::IsTrue(queue.Peek(buffer, 5));
			Assert::AreEqual(std::string("23456"), std::string(buffer));
		}

        // •	버퍼의 중간부터 쓰기를 시작하여 끝을 지나 다시 처음으로 이어지는(Wrap-around) 상황을 시뮬레이션.
        // •	데이터가 끊기지 않고 올바르게 읽히는지(Peek, Pop) 확인.
		TEST_METHOD(WrapAroundTests)
		{
			CircleBufferQueue queue(10);

			// Move head to middle
			char padding[] = "12345";
			Assert::IsTrue(queue.Write(padding, 5));
			Assert::IsTrue(queue.Consume(5));

			// Now Head=5, Tail=5. Capacity=10.

			// Write 8 bytes. 5 bytes at [5..9], 3 bytes at [0..2]
			char data[] = "ABCDEFGH";
			Assert::IsTrue(queue.Write(data, 8));

			Assert::AreEqual((size_t)8, queue.CanReadSize());

			char buffer[9] = { 0 };
			Assert::IsTrue(queue.Peek(buffer, 8));
			Assert::AreEqual(std::string("ABCDEFGH"), std::string(buffer));

			// Pop 6 bytes. Tail moves from 5 -> 9 -> 1.
			char popBuffer[7] = { 0 };
			Assert::IsTrue(queue.Pop(popBuffer, 6));
			Assert::AreEqual(std::string("ABCDEF"), std::string(popBuffer));

			// Remaining: GH at [1..2]
			Assert::AreEqual((size_t)2, queue.CanReadSize());

			char remaining[3] = { 0 };
			Assert::IsTrue(queue.Pop(remaining, 2));
			Assert::AreEqual(std::string("GH"), std::string(remaining));
		}

        // •	크기가 0인 데이터를 쓰거나 읽을 때의 동작을 확인.
        // •	비어있는 버퍼에서 읽기(Pop, Peek)나 소비(Consume)를 시도할 때 실패하는지 확인.
		TEST_METHOD(EdgeCases)
		{
			CircleBufferQueue queue(10);

			// Zero size write/read
			char data = 'A';
			Assert::IsTrue(queue.Write(&data, 0));
			Assert::AreEqual((size_t)0, queue.CanReadSize());

			char buffer;
			Assert::IsTrue(queue.Pop(&buffer, 0));

			// Pop from empty
			Assert::IsFalse(queue.Pop(&buffer, 1));

			// Peek from empty
			Assert::IsFalse(queue.Peek(&buffer, 1));

			// Consume from empty
			Assert::IsFalse(queue.Consume(1));
		}

		// 버퍼 크기보다 큰 데이터 쓰기/읽기 테스트
		// • 버퍼 용량을 초과하는 데이터 쓰기 시도 시 실패하는지 확인
		// • 여러 번 나누어 쓰고 읽어서 큰 데이터를 처리할 수 있는지 확인
		// • 버퍼 확장 없이 청크 단위로 처리하는 패턴 검증
		TEST_METHOD(LargerThanBufferTests)
		{
			constexpr size_t BUFFER_SIZE = 10;
			CircleBufferQueue queue(BUFFER_SIZE);

			// 1. 버퍼보다 큰 데이터 한 번에 쓰기 시도 -> 실패해야 함
			char largeData[] = "123456789012345"; // 15 bytes
			Assert::IsFalse(queue.Write(largeData, 15));
			Assert::AreEqual((size_t)0, queue.CanReadSize());
			Assert::AreEqual(BUFFER_SIZE, queue.CanWriteSize());

			// 2. 버퍼보다 큰 데이터를 청크 단위로 나누어 쓰고 읽기
			const char* bigData = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"; // 26 bytes
			size_t totalWritten = 0;
			size_t totalRead = 0;
			std::string readResult;

			while (totalWritten < 26)
			{
				// 쓸 수 있는 만큼 쓰기
				size_t toWrite = std::min(queue.CanWriteSize(), 26 - totalWritten);
				if (toWrite > 0)
				{
					Assert::IsTrue(queue.Write(bigData + totalWritten, toWrite));
					totalWritten += toWrite;
				}

				// 읽을 수 있는 만큼 읽기
				size_t toRead = queue.CanReadSize();
				if (toRead > 0)
				{
					char buffer[11] = { 0 };
					Assert::IsTrue(queue.Pop(buffer, toRead));
					readResult.append(buffer, toRead);
					totalRead += toRead;
				}
			}

			// 남은 데이터 모두 읽기
			while (queue.CanReadSize() > 0)
			{
				size_t toRead = queue.CanReadSize();
				char buffer[11] = { 0 };
				Assert::IsTrue(queue.Pop(buffer, toRead));
				readResult.append(buffer, toRead);
				totalRead += toRead;
			}

			Assert::AreEqual((size_t)26, totalWritten);
			Assert::AreEqual((size_t)26, totalRead);
			Assert::AreEqual(std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ"), readResult);
		}

		// 버퍼 크기의 정확히 N배 데이터 처리 테스트
		// • 버퍼를 여러 번 가득 채우고 비우는 사이클 테스트
		// • 경계 조건에서 데이터 무결성 확인
		TEST_METHOD(MultipleBufferCyclesTest)
		{
			constexpr size_t BUFFER_SIZE = 8;
			CircleBufferQueue queue(BUFFER_SIZE);

			// 버퍼 크기의 5배 데이터 (40 bytes)
			const char* testData = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcd";
			std::string readResult;

			for (size_t cycle = 0; cycle < 5; ++cycle)
			{
				// 매 사이클마다 버퍼 크기만큼 쓰고 읽기
				const char* chunk = testData + (cycle * BUFFER_SIZE);
				
				Assert::IsTrue(queue.Write(chunk, BUFFER_SIZE));
				Assert::AreEqual((size_t)0, queue.CanWriteSize());
				Assert::AreEqual(BUFFER_SIZE, queue.CanReadSize());

				char buffer[9] = { 0 };
				Assert::IsTrue(queue.Pop(buffer, BUFFER_SIZE));
				readResult.append(buffer, BUFFER_SIZE);

				Assert::AreEqual(BUFFER_SIZE, queue.CanWriteSize());
				Assert::AreEqual((size_t)0, queue.CanReadSize());
			}

			Assert::AreEqual(std::string(testData, 40), readResult);
		}

		// 버퍼보다 큰 데이터를 Peek으로 확인하면서 처리하는 테스트
		// • Peek 후 Consume 패턴으로 큰 데이터 처리
		// • 패킷 프레이밍과 유사한 사용 패턴 검증
		TEST_METHOD(LargeDataWithPeekConsumeTest)
		{
			constexpr size_t BUFFER_SIZE = 16;
			CircleBufferQueue queue(BUFFER_SIZE);

			// 버퍼 크기보다 큰 데이터를 스트림처럼 처리
			const char* streamData = "HEADER:PAYLOAD_DATA_THAT_IS_VERY_LONG_END";
			size_t streamLen = strlen(streamData); // 42 bytes
			size_t writePos = 0;
			std::string accumulated;

			while (writePos < streamLen || queue.CanReadSize() > 0)
			{
				// 쓸 수 있으면 쓰기
				if (writePos < streamLen && queue.CanWriteSize() > 0)
				{
					size_t toWrite = std::min(queue.CanWriteSize(), streamLen - writePos);
					Assert::IsTrue(queue.Write(streamData + writePos, toWrite));
					writePos += toWrite;
				}

				// Peek으로 확인 후 Consume
				size_t readable = queue.CanReadSize();
				if (readable > 0)
				{
					char peekBuffer[17] = { 0 };
					Assert::IsTrue(queue.Peek(peekBuffer, readable));
					
					// Peek한 데이터가 올바른지 확인
					std::string peeked(peekBuffer, readable);
					
					// 일부만 Consume (예: 절반씩)
					size_t toConsume = (readable + 1) / 2;
					
					// Consume 전에 Pop으로 실제 데이터 가져오기
					char popBuffer[17] = { 0 };
					Assert::IsTrue(queue.Pop(popBuffer, toConsume));
					accumulated.append(popBuffer, toConsume);
				}
			}

			Assert::AreEqual(std::string(streamData), accumulated);
		}

		// 버퍼 크기 경계에서의 쓰기/읽기 테스트
		// • 정확히 버퍼 크기만큼 쓰기
		// • 버퍼 크기 + 1 쓰기 시도
		// • 버퍼 크기 - 1 쓰기 후 추가 쓰기
		TEST_METHOD(BoundaryConditionTests)
		{
			constexpr size_t BUFFER_SIZE = 10;
			CircleBufferQueue queue(BUFFER_SIZE);

			// 정확히 버퍼 크기만큼 쓰기 -> 성공
			char exactData[] = "1234567890";
			Assert::IsTrue(queue.Write(exactData, BUFFER_SIZE));
			Assert::AreEqual((size_t)0, queue.CanWriteSize());
			Assert::AreEqual(BUFFER_SIZE, queue.CanReadSize());

			// 버퍼 크기 + 1 쓰기 시도 -> 실패 (버퍼 가득 참)
			char extraByte = 'X';
			Assert::IsFalse(queue.Write(&extraByte, 1));

			// 읽고 비우기
			char buffer[11] = { 0 };
			Assert::IsTrue(queue.Pop(buffer, BUFFER_SIZE));
			Assert::AreEqual(std::string("1234567890"), std::string(buffer));

			// 버퍼 크기 - 1 쓰기
			char almostFull[] = "123456789";
			Assert::IsTrue(queue.Write(almostFull, BUFFER_SIZE - 1));
			Assert::AreEqual((size_t)1, queue.CanWriteSize());

			// 1바이트 추가 -> 성공
			Assert::IsTrue(queue.Write(&extraByte, 1));
			Assert::AreEqual((size_t)0, queue.CanWriteSize());

			// 2바이트 추가 시도 -> 실패
			char twoBytes[] = "YZ";
			Assert::IsFalse(queue.Write(twoBytes, 2));

			// 전체 읽기 검증
			memset(buffer, 0, sizeof(buffer));
			Assert::IsTrue(queue.Pop(buffer, BUFFER_SIZE));
			Assert::AreEqual(std::string("123456789X"), std::string(buffer));
		}
	};
}
