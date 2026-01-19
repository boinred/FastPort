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
	};
}
