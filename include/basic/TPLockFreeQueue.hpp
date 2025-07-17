#ifndef HSLL_TBLOCKFREEQUEUE
#define HSLL_TBLOCKFREEQUEUE

#include"concurrentqueue/concurrentqueue.h"
#include"concurrentqueue/blockingconcurrentqueue.h"
#include <algorithm>

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

#if defined(_WIN32)
#include <malloc.h>
#define ALIGNED_MALLOC(size, align) _aligned_malloc(size, align)
#define ALIGNED_FREE(ptr) _aligned_free(ptr)
#else
#define ALIGNED_MALLOC(size, align) aligned_alloc(align, (size + align - 1) & ~(align - 1))
#define ALIGNED_FREE(ptr) free(ptr)
#endif

namespace HSLL
{
	template<typename TYPE>
	class alignas(64) TPBLFQueue
	{
		bool flag;
		std::atomic<bool> isStopped;
		moodycamel::BlockingConcurrentQueue<TYPE>* queue;

	public:

		TPBLFQueue() :flag(false), queue(nullptr) {}

		bool init(unsigned int capacity)
		{
			assert(!flag);

			if (!(queue = new (std::nothrow)moodycamel::BlockingConcurrentQueue<TYPE>(capacity)))
				return false;

			isStopped = false;
			flag = true;
			return true;
		}

		void stopWait()
		{
			assert(flag);
			return isStopped.store(true, std::memory_order_release);
		}

		bool is_Stopped()
		{
			assert(flag);
			return isStopped.load(std::memory_order_relaxed);
		}

		bool is_Stopped_Real()
		{
			assert(flag);
			return isStopped.load(std::memory_order_acquire);
		}

		unsigned int get_size()
		{
			assert(flag);
			return queue->size_approx();
		}

		template <class T>
		bool enqueue(T&& item)
		{
			assert(flag);
			return queue->try_enqueue(std::forward<T>(item));
		}

		unsigned int enqueue_bulk(TYPE* elements, unsigned int count)
		{
			assert(flag);
			return  queue->try_enqueue_bulk(elements, count) ? count : 0;
		}

		unsigned int enqueue_bulk(TYPE* part1, unsigned int count1, TYPE* part2, unsigned int count2)
		{
			assert(flag);
			assert(part1 && count1);
			unsigned int num = count1;
			if (queue->try_enqueue_bulk(part1, num))
			{
				if (num == count1 && part2 && count2)
				{
					if (queue->try_enqueue_bulk(part2, count2))
						num += count2;
				}
				return num;
			}
			else
			{
				return 0;
			}
		}

		bool dequeue(TYPE& element)
		{
			assert(flag);
			return queue->try_dequeue(element);
		}

		bool wait_dequeue(TYPE& element, std::int64_t timeout_usecs)
		{
			assert(flag);
			return queue->wait_dequeue_timed(element, timeout_usecs);
		}

		unsigned int dequeue_bulk(TYPE* elements, unsigned int count)
		{
			assert(flag);
			return queue->try_dequeue_bulk(elements, count);
		}

		unsigned int wait_dequeue_bulk(TYPE* elements, unsigned int count, std::int64_t timeout_usecs)
		{
			assert(flag);
			return queue->wait_dequeue_bulk_timed(elements, count, timeout_usecs);
		}

		void release()
		{
			assert(flag);
			delete queue;
			queue = nullptr;
			flag = false;
		}

		~TPBLFQueue()
		{
			if (flag)
				release();
		}

		TPBLFQueue(const TPBLFQueue&) = delete;
		TPBLFQueue& operator=(const TPBLFQueue&) = delete;
	};
}

#endif // HSLL_TBLOCKFREEQUEUEK