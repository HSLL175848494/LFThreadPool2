#ifndef HSLL_THREADPOOL
#define HSLL_THREADPOOL

#include <vector>
#include <thread>
#include "basic/TPTask.hpp"
#include "basic/TPRWLock.hpp"
#include "basic/TPLockFreeQueue.hpp"

#define HSLL_THREADPOOL_TIMEOUT_MILLISECONDS 5
#define HSLL_THREADPOOL_SHRINK_FACTOR 0.25
#define HSLL_THREADPOOL_EXPAND_FACTOR 0.75

#define HSLL_THREADPOOL_TIMEOUT_MICROSECONDS \
	HSLL_THREADPOOL_TIMEOUT_MILLISECONDS*1000

static_assert(HSLL_THREADPOOL_TIMEOUT_MICROSECONDS == HSLL_THREADPOOL_TIMEOUT_MILLISECONDS * 1000,
	"Modify the configuration to use HSLL_THREADPOOL_TIMEOUT_MILLISECONDS instead of HSLL_THREADPOOL_TIMEOUT_MICROSECONDS.");
static_assert(HSLL_THREADPOOL_TIMEOUT_MILLISECONDS > 0, "Invalid timeout value.");
static_assert(HSLL_THREADPOOL_SHRINK_FACTOR < HSLL_THREADPOOL_EXPAND_FACTOR&& HSLL_THREADPOOL_EXPAND_FACTOR < 1.0
	&& HSLL_THREADPOOL_SHRINK_FACTOR>0.0, "Invalid factors.");

namespace HSLL
{
	template <class T>
	class SingleStealer
	{
		template <class TYPE>
		friend class ThreadPool;
	private:

		unsigned int index;
		unsigned int queueLength;
		unsigned int* threadNum;
		unsigned int threshold;

		ReadWriteLock* rwLock;
		TPBLFQueue<T>* queues;
		TPBLFQueue<T>* ignore;

		SingleStealer(ReadWriteLock* rwLock, TPBLFQueue<T>* queues, TPBLFQueue<T>* ignore,
			unsigned int queueLength, unsigned int* threadNum)
		{
			this->index = 0;
			this->queueLength = queueLength;
			this->threadNum = threadNum;
			this->threshold = std::min((unsigned int)2, queueLength);
			this->rwLock = rwLock;
			this->queues = queues;
			this->ignore = ignore;
		}

		unsigned int steal(T& element)
		{
			ReadLockGuard lock(*rwLock);
			unsigned int num = *threadNum;
			for (int i = 0; i < num; ++i)
			{
				unsigned int now = (index + i) % num;
				TPBLFQueue<T>* queue = queues + now;
				if (queue->get_size() >= threshold && queue != ignore)
				{
					if (queue->dequeue(element))
					{
						index = now;
						return 1;
					}
				}
			}
			return 0;
		}
	};

	template <class T>
	class BulkStealer
	{
		template <class TYPE>
		friend class ThreadPool;

	private:

		unsigned int index;
		unsigned int batchSize;
		unsigned int queueLength;
		unsigned int* threadNum;
		unsigned int threshold;

		ReadWriteLock* rwLock;
		TPBLFQueue<T>* queues;
		TPBLFQueue<T>* ignore;

		BulkStealer(ReadWriteLock* rwLock, TPBLFQueue<T>* queues, TPBLFQueue<T>* ignore, unsigned int queueLength,
			unsigned int* threadNum, unsigned int batchSize)
		{
			this->index = 0;
			this->batchSize = batchSize;
			this->queueLength = queueLength;
			this->threadNum = threadNum;
			this->threshold = std::min(2 * batchSize, queueLength / 2);
			this->rwLock = rwLock;
			this->queues = queues;
			this->ignore = ignore;
		}

		unsigned int steal(T* elements)
		{
			ReadLockGuard lock(*rwLock);
			unsigned int num = *threadNum;
			for (int i = 0; i < num; ++i)
			{
				unsigned int now = (index + i) % num;
				TPBLFQueue<T>* queue = queues + now;
				if (queue->get_size() >= threshold && queue != ignore)
				{
					unsigned int count = queue->dequeue_bulk(elements, batchSize);
					if (count)
					{
						index = now;
						return count;
					}
				}
			}
			return 0;
		}
	};

	/**
	 * @brief Thread pool implementation with multiple queues for task distribution
	 * @tparam T Type of task objects to be processed, must implement execute() method
	 */
	template <class T = TaskStack<>>
	class ThreadPool
	{
		static_assert(is_generic_ts<T>::value, "TYPE must be a TaskStack type");

	private:

		unsigned int threadNum;			  ///< Number of worker threads/queues
		unsigned int minThreadNum;
		unsigned int maxThreadNum;
		unsigned int batchSize;
		unsigned int queueLength;		  ///< Capacity of each internal queue
		std::chrono::milliseconds adjustInterval;

		T* containers;
		ReadWriteLock rwLock;
		std::atomic<bool> exitFlag;
		std::atomic<bool> shutdownPolicy;			  ///< Thread pool shutdown policy: true for graceful shutdown	
		moodycamel::LightweightSemaphore exitSem;
		moodycamel::LightweightSemaphore* stopSem;
		moodycamel::LightweightSemaphore* startSem;

		std::thread monitor;
		TPBLFQueue<T>* queues;		  ///< Per-worker task queues
		std::vector<std::thread> workers; ///< Worker thread collection
		std::atomic<unsigned int> index;  ///< Atomic counter for round-robin task distribution to worker queues

	public:

		/**
		 * @brief Constructs an uninitialized thread pool
		 */
		ThreadPool() : queues(nullptr) {}

		/**
		* @brief Initializes thread pool resources
		* @param capacity Capacity of each internal queue (>2)
		* @param minThreadNum Minimum number of worker threads (!=0)
		* @param maxThreadNum Maximum number of worker threads (>=minThreadNum)
		* @param batchSize Maximum tasks to process per batch (min 1)
		* @param adjustInterval Time interval for checking the load and adjusting the number of active threads
		* @return true if initialization succeeded, false otherwise
		*/
		bool init(unsigned int capacity, unsigned int minThreadNum,
			unsigned int maxThreadNum, unsigned int batchSize = 1,
			std::chrono::milliseconds adjustInterval = std::chrono::milliseconds(3000)) noexcept
		{
			assert(!queues);

			if (batchSize == 0 || minThreadNum == 0 || capacity< 2 || minThreadNum > maxThreadNum)
				return false;

			if (!initResourse(capacity, maxThreadNum, batchSize))
				return false;

			this->index = 0;
			this->exitFlag = false;
			this->shutdownPolicy = true;
			this->minThreadNum = minThreadNum;
			this->maxThreadNum = maxThreadNum;
			this->threadNum = maxThreadNum;
			this->batchSize = std::min(batchSize, capacity / 2);
			this->queueLength = capacity;
			this->adjustInterval = adjustInterval;
			workers.reserve(maxThreadNum);

			for (unsigned i = 0; i < maxThreadNum; ++i)
				workers.emplace_back(&ThreadPool::worker, this, i);

			if (maxThreadNum > 1)
				monitor = std::thread(&ThreadPool::load_monitor, this);
		}

		/**
		 * @brief Non-blocking push for preconstructed task
		 * @tparam U Deduced task type (supports perfect forwarding)
		 * @param task Task object to enqueue
		 * @return true if task was enqueued, false if queue was full
		 */
		template <class U>
		bool enqueue(U&& task) noexcept
		{
			assert(queues);

			if (maxThreadNum == 1)
				return queues->enqueue(std::forward<U>(task));

			ReadLockGuard lock(rwLock);
			return select_queue().enqueue(std::forward<U>(task));
		}

		/**
		 * @brief Non-blocking bulk push for multiple tasks
		 * @param tasks Array of tasks to enqueue
		 * @param count Number of tasks in array
		 * @return Actual number of tasks enqueued
		 */
		unsigned int enqueue_bulk(T* tasks, unsigned int count) noexcept
		{
			assert(queues);

			if (maxThreadNum == 1)
				return queues->enqueue_bulk(tasks, count);

			ReadLockGuard lock(rwLock);
			return select_queue_for_bulk(std::max(1u, count / 2)).enqueue_bulk(tasks, count);
		}

		/**
		 * @brief Non-blocking bulk push for multiple tasks (dual-part version)
		 * @param part1 First array segment of tasks to enqueue
		 * @param count1 Number of tasks in first segment
		 * @param part2 Second array segment of tasks to enqueue
		 * @param count2 Number of tasks in second segment
		 * @return Actual number of tasks successfully enqueued (sum of both segments minus failures)
		 * @note Designed for ring buffers that benefit from batched two-part insertion.
		 */
		unsigned int enqueue_bulk(T* part1, unsigned int count1, T* part2, unsigned int count2) noexcept
		{
			assert(queues);

			if (maxThreadNum == 1)
				return queues->enqueue_bulk(part1, count1, part2, count2);

			ReadLockGuard lock(rwLock);
			return select_queue_for_bulk(std::max(1u, (count1 + count2) / 2)).enqueue_bulk(part1, count1, part2, count2);
		}

		//Get the maximum occupied space of the thread pool.
		unsigned long long get_max_usage()
		{
			assert(queues);

			return  maxThreadNum * queues->get_bsize();
		}

		/**
		 * @brief Waits until all task queues are empty (all tasks have been taken from queues)
		 * @note This does NOT guarantee all tasks have completed execution - it only ensures:
		 *       1. All tasks have been dequeued by worker threads
		 *       2. May return while some tasks are still being processed by workers
		 * @details Continuously checks all active queues until they're empty.
		 *          Uses yield() between checks to avoid busy waiting.
		 */
		void join()
		{
			assert(queues);

			while (true)
			{
				bool flag = true;

				for (int i = 0; i < maxThreadNum; ++i)
				{
					if (queues[i].get_size())
					{
						flag = false;
						break;
					}
				}

				if (flag)
					return;
				else
					std::this_thread::yield();
			}
		}

		/**
		 * @brief Waits until all task queues are empty (all tasks dequeued) or sleeps for specified intervals between checks.
		 * @note This does NOT guarantee all tasks have completed execution - it only ensures:
		 *       1. All tasks have been dequeued by worker threads
		 *       2. May return while some tasks are still being processed by workers
		 * @tparam Rep Arithmetic type representing tick count
		 * @tparam Period Type representing tick period
		 * @param interval Sleep duration between queue checks. Smaller values increase responsiveness
		 *                 but may use more CPU, larger values reduce CPU load but delay detection.
		 */
		template <class Rep, class Period>
		void join(const std::chrono::duration<Rep, Period>& interval)
		{
			assert(queues);

			while (true)
			{
				bool flag = true;
				for (int i = 0; i < maxThreadNum; ++i)
				{
					if (queues[i].get_size())
					{
						flag = false;
						break;
					}
				}

				if (flag)
					return;
				else
					std::this_thread::sleep_for(interval);
			}
		}

		/**
		 * @brief Stops all workers and releases resources
		 * @param shutdownPolicy true for graceful shutdown (waiting for tasks to complete), false for immediate shutdown
		 */
		void exit(bool shutdownPolicy = true) noexcept
		{
			assert(queues);

			if (maxThreadNum > 1)
			{
				exitSem.signal();
				monitor.join();
			}

			exitFlag = true;
			this->shutdownPolicy = shutdownPolicy;

			for (unsigned i = 0; i < workers.size(); ++i)
				startSem[i].signal();

			for (unsigned i = 0; i < workers.size(); ++i)
				queues[i].stopWait();

			for (auto& worker : workers)
				worker.join();

			rleaseResourse();
		}

		~ThreadPool() noexcept
		{
			if (queues)
				exit(false);
		}

		ThreadPool(const ThreadPool&) = delete;
		ThreadPool& operator=(const ThreadPool&) = delete;
		ThreadPool(ThreadPool&&) = delete;
		ThreadPool& operator=(ThreadPool&&) = delete;

	private:

		unsigned int next_index() noexcept
		{
			return index.fetch_add(1, std::memory_order_relaxed) % threadNum;
		}

		TPBLFQueue<T>& select_queue() noexcept
		{
			unsigned int index = next_index();

			if (queues[index].get_size() < queueLength)
				return queues[index];

			unsigned int half = threadNum / 2;
			return queues[(index + half) % threadNum];
		}

		TPBLFQueue<T>& select_queue_for_bulk(unsigned required) noexcept
		{
			unsigned int index = next_index();

			if (queues[index].get_size() + required <= queueLength)
				return queues[index];

			unsigned int half = threadNum / 2;
			return queues[(index + half) % threadNum];
		}

		void load_monitor() noexcept
		{
			while (true)
			{
				if (exitSem.wait(adjustInterval.count() * 1000))
					return;

				unsigned int allSize = queueLength * threadNum;
				unsigned int totalSize = 0;

				for (int i = 0; i < threadNum; ++i)
					totalSize += queues[i].get_size();

				if (totalSize < allSize * HSLL_THREADPOOL_SHRINK_FACTOR && threadNum > minThreadNum)
				{
					rwLock.lock_write();
					threadNum--;
					rwLock.unlock_write();
					queues[threadNum].stopWait();
					while (!stopSem[threadNum].wait());
					queues[threadNum].release();
				}
				else if (totalSize > allSize * HSLL_THREADPOOL_EXPAND_FACTOR && threadNum < maxThreadNum)
				{
					unsigned int newThreads = std::max(1u, (maxThreadNum - threadNum) / 2);
					unsigned int succeed = 0;
					for (int i = threadNum; i < threadNum + newThreads; ++i)
					{
						if (!queues[i].init(queueLength))
							break;

						startSem[i].signal();
						succeed++;
					}

					if (succeed > 0)
					{
						rwLock.lock_write();
						threadNum += succeed;
						rwLock.unlock_write();
					}
				}
			}
		}

		void worker(unsigned int index) noexcept
		{
			if (batchSize == 1)
			{
				if (maxThreadNum == 1)
					process_single1(queues + index, index);
				else
					process_single2(queues + index, index);
			}
			else
			{
				if (maxThreadNum == 1)
					process_bulk1(queues + index, index);
				else
					process_bulk2(queues + index, index);
			}
		}

		static inline void execute_tasks(T* tasks, unsigned int count)
		{
			for (unsigned int i = 0; i < count; ++i)
			{
				tasks[i].execute();
				tasks[i].~T();
			}
		}

		void process_single1(TPBLFQueue<T>* queue, unsigned int index) noexcept
		{
			T* task = containers + index * batchSize;

			while (true)
			{
				while (true)
				{
					while (queue->dequeue(*task))
					{
						task->execute();
						task->~T();
					}

					if (queue->wait_dequeue(*task, HSLL_THREADPOOL_TIMEOUT_MICROSECONDS))
					{
						task->execute();
						task->~T();
					}
					else
					{
						if (queue->is_Stopped_Real())
							break;
					}

					if (queue->is_Stopped())
						break;
				}

				bool  shutdownPolicy = this->shutdownPolicy.load();

				while (shutdownPolicy && queue->dequeue(*task))
				{
					task->execute();
					task->~T();
				}

				stopSem[index].signal();
				while (!startSem[index].wait());

				if (exitFlag)
					break;
			}
		}

		void process_single2(TPBLFQueue<T>* queue, unsigned int index) noexcept
		{
			T* task = containers + index * batchSize;
			SingleStealer<T> stealer(&rwLock, queues, queue, queueLength, &threadNum);

			while (true)
			{
				while (true)
				{
					while (queue->dequeue(*task))
					{
						task->execute();
						task->~T();
					}

					if (stealer.steal(*task))
					{
						task->execute();
						task->~T();
					}
					else
					{
						if (queue->wait_dequeue(*task, HSLL_THREADPOOL_TIMEOUT_MICROSECONDS))
						{
							task->execute();
							task->~T();
						}
						else
						{
							if (queue->is_Stopped_Real())
								break;
						}
					}

					if (queue->is_Stopped())
						break;
				}

				bool  shutdownPolicy = this->shutdownPolicy.load();

				while (shutdownPolicy && queue->dequeue(*task))
				{
					task->execute();
					task->~T();
				}

				stopSem[index].signal();
				while (!startSem[index].wait());

				if (exitFlag)
					break;
			}
		}

		void process_bulk1(TPBLFQueue<T>* queue, unsigned int index) noexcept
		{
			T* tasks = containers + index * batchSize;
			unsigned int size_threshold = batchSize;
			unsigned int round_threshold = batchSize / 2;

			while (true)
			{
				unsigned int count;

				while (true)
				{
					while (true)
					{
						unsigned int round = 1;
						unsigned int size = queue->get_size();

						while (size < size_threshold && round < round_threshold)
						{
							std::this_thread::yield();
							size = queue->get_size();
							round++;
							std::this_thread::yield();
						}

						if (size && (count = queue->dequeue_bulk(tasks, size_threshold)))
							execute_tasks(tasks, count);
						else
							break;
					}

					count = queue->wait_dequeue_bulk(tasks, size_threshold, HSLL_THREADPOOL_TIMEOUT_MICROSECONDS);

					if (count)
					{
						execute_tasks(tasks, count);
					}
					else
					{
						if (queue->is_Stopped_Real())
							break;
					}

					if (queue->is_Stopped())
						break;
				}

				bool  shutdownPolicy = this->shutdownPolicy.load();

				while (shutdownPolicy && (count = queue->dequeue_bulk(tasks, size_threshold)))
					execute_tasks(tasks, count);

				stopSem[index].signal();
				while (!startSem[index].wait());

				if (exitFlag)
					break;
			}
		}

		void process_bulk2(TPBLFQueue<T>* queue, unsigned int index) noexcept
		{
			T* tasks = containers + index * batchSize;
			unsigned int size_threshold = batchSize;
			unsigned int round_threshold = batchSize / 2;
			BulkStealer<T> stealer(&rwLock, queues, queue, queueLength, &threadNum, batchSize);

			while (true)
			{
				unsigned int count;

				while (true)
				{
					while (true)
					{
						unsigned int round = 1;
						unsigned int size = queue->get_size();

						while (size < size_threshold && round < round_threshold)
						{
							std::this_thread::yield();
							size = queue->get_size();
							round++;
							std::this_thread::yield();
						}

						if (size && (count = queue->dequeue_bulk(tasks, size_threshold)))
							execute_tasks(tasks, count);
						else
							break;
					}

					count = stealer.steal(tasks);
					if (count)
					{
						execute_tasks(tasks, count);
					}
					else
					{
						count = queue->wait_dequeue_bulk(tasks, size_threshold, HSLL_THREADPOOL_TIMEOUT_MICROSECONDS);

						if (count)
						{
							execute_tasks(tasks, count);
						}
						else
						{
							if (queue->is_Stopped_Real())
								break;
						}
					}

					if (queue->is_Stopped())
						break;
				}

				bool  shutdownPolicy = this->shutdownPolicy.load();

				while (shutdownPolicy && (count = queue->dequeue_bulk(tasks, size_threshold)))
					execute_tasks(tasks, count);

				stopSem[index].signal();
				while (!startSem[index].wait());

				if (exitFlag)
					break;
			}
		}

		bool initResourse(unsigned int capacity, unsigned int maxThreadNum, unsigned int batchSize)
		{
			unsigned int succeed = 0;

			if (!(startSem = new(std::nothrow) moodycamel::LightweightSemaphore[2 * maxThreadNum]))
				goto clean_1;

			stopSem = startSem + maxThreadNum;

			if (!(containers = (T*)ALIGNED_MALLOC(sizeof(T) * batchSize * maxThreadNum, alignof(T))))
				goto clean_2;

			if (!(queues = (TPBLFQueue<T>*)ALIGNED_MALLOC(maxThreadNum * sizeof(TPBLFQueue<T>), 64)))
				goto clean_3;

			for (unsigned i = 0; i < maxThreadNum; ++i)
			{
				new (&queues[i]) TPBLFQueue<T>();

				if (!queues[i].init(capacity))
					goto clean_4;

				succeed++;
			}

			return true;

		clean_4:

			for (unsigned i = 0; i < succeed; ++i)
				queues[i].~TPBLFQueue<T>();

		clean_3:

			ALIGNED_FREE(queues);
			queues = nullptr;

		clean_2:

			ALIGNED_FREE(containers);

		clean_1:

			delete[] startSem;

			return false;
		}

		void rleaseResourse()
		{
			for (unsigned i = 0; i < maxThreadNum; ++i)
				queues[i].~TPBLFQueue<T>();

			ALIGNED_FREE(queues);
			ALIGNED_FREE(containers);
			delete[] startSem;
			queues = nullptr;
			workers.clear();
			workers.shrink_to_fit();
		}
	};

	template <class T, unsigned int BATCH>
	class BatchSubmitter
	{
		static_assert(is_generic_ts<T>::value, "TYPE must be a TaskStack type");
		static_assert(BATCH > 0, "BATCH > 0");
		alignas(alignof(T)) unsigned char buf[BATCH * sizeof(T)];

		T* elements;
		unsigned int size;
		unsigned int index;
		ThreadPool<T>* pool;

		bool check_and_submit()
		{
			if (size == BATCH)
				return submit() == BATCH;

			return true;
		}

	public:
		/**
		* @brief Constructs a batch submitter associated with a thread pool
		* @param pool Pointer to the thread pool for batch task submission
		*/
		BatchSubmitter(ThreadPool<T>* pool) : size(0), index(0), elements((T*)buf), pool(pool) {
			assert(pool);
		}

		/**
		 * @brief Gets current number of buffered tasks
		 * @return Number of tasks currently held in the batch buffer
		 */
		unsigned int get_size() const noexcept
		{
			return size;
		}

		/**
		 * @brief Checks if batch buffer is empty
		 * @return true if no tasks are buffered, false otherwise
		 */
		bool empty() const noexcept
		{
			return size == 0;
		}

		/**
		 * @brief Checks if batch buffer is full
		 * @return true if batch buffer has reached maximum capacity (BATCH), false otherwise
		 */
		bool full() const noexcept
		{
			return size == BATCH;
		}

		/**
		 * @brief Constructs task in-place in batch buffer
		 * @tparam Args Types of arguments for task constructor
		 * @param args Arguments forwarded to task constructor
		 * @return true if task was added to buffer or submitted successfully,
		 *         false if submission failed due to full thread pool queues
		 * @details Automatically submits batch if buffer becomes full during emplace
		 */
		template <typename... Args>
		bool emplace(Args &&...args) noexcept
		{
			if (!check_and_submit())
				return false;

			new (elements + index) T(std::forward<Args>(args)...);
			index = (index + 1) % BATCH;
			size++;
			return true;
		}

		/**
		 * @brief Adds preconstructed task to batch buffer
		 * @tparam U Deduced task type
		 * @param task Task object to buffer
		 * @return true if task was added to buffer or submitted successfully,
		 *         false if submission failed due to full thread pool queues
		 * @details Automatically submits batch if buffer becomes full during add
		 */
		template <class U>
		bool add(U&& task) noexcept
		{
			if (!check_and_submit())
				return false;

			new (elements + index) T(std::forward<U>(task));
			index = (index + 1) % BATCH;
			size++;
			return true;
		}

		/**
		 * @brief Submits all buffered tasks to thread pool
		 * @return Number of tasks successfully submitted
		 * @details Moves buffered tasks to thread pool in bulk.
		 */
		unsigned int submit() noexcept
		{
			if (!size)
				return 0;

			unsigned int start = (index - size + BATCH) % BATCH;
			unsigned int len1 = (start + size <= BATCH) ? size : (BATCH - start);
			unsigned int len2 = size - len1;
			unsigned int submitted;

			if (!len2)
			{
				if (len1 == 1)
					submitted = pool->enqueue(std::move(*(elements + start))) ? 1 : 0;
				else
					submitted = pool->enqueue_bulk(elements + start, len1);
			}
			else
			{
				submitted = pool->enqueue_bulk(
					elements + start, len1,
					elements, len2
				);
			}

			if (submitted > 0)
			{
				if (submitted <= len1)
				{
					for (unsigned i = 0; i < submitted; ++i)
						elements[(start + i) % BATCH].~T();
				}
				else
				{
					for (unsigned i = 0; i < len1; ++i)
						elements[(start + i) % BATCH].~T();

					for (unsigned i = 0; i < submitted - len1; ++i)
						elements[i].~T();
				}

				size -= submitted;
			}

			return submitted;
		}

		~BatchSubmitter() noexcept
		{
			if (size > 0)
			{
				unsigned int start = (index - size + BATCH) % BATCH;
				unsigned int len1 = (start + size <= BATCH) ? size : (BATCH - start);
				unsigned int len2 = size - len1;

				for (unsigned int i = 0; i < len1; i++)
					elements[(start + i) % BATCH].~T();

				for (unsigned int i = 0; i < len2; i++)
					elements[i].~T();
			}
		}

		BatchSubmitter(const BatchSubmitter&) = delete;
		BatchSubmitter& operator=(const BatchSubmitter&) = delete;
		BatchSubmitter(BatchSubmitter&&) = delete;
		BatchSubmitter& operator=(BatchSubmitter&&) = delete;
	};
}

#endif