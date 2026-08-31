#pragma once

#include <mutex>
#include <condition_variable>
#include <deque>

/* One GL call from the emulator becomes one push() here, so this queue is on
 * the hottest path in the threaded renderer.  notify_one() used to fire on
 * every push, waiter or not.  On Linux an uncontended signal is resolved in
 * userspace and that is nearly free; on QNX pthread_cond_signal always issues
 * the SyncCondvarSignal kernel call, so it cost a syscall per GL call and
 * measured as the single largest consumer of CPU time in the process.
 *
 * Counting blocked consumers removes the signal whenever nobody is waiting,
 * which is the common case while the render thread is busy draining.  It
 * cannot lose a wakeup: a consumer only blocks after testing the predicate
 * while holding m_mutex, and the producer pushes and reads m_waiters under
 * that same mutex.  Either the consumer sees the new item and never blocks,
 * or it was already counted before the producer looked. */
template <typename T, typename  Container = std::deque<T>>
class BlockingQueue
{
private:
	std::mutex m_mutex;
	std::condition_variable m_condition;
	Container m_queue;
	unsigned m_waiters = 0;

	class WaiterScope
	{
	public:
		explicit WaiterScope(unsigned& _waiters) : m_waiters(_waiters) { ++m_waiters; }
		~WaiterScope() { --m_waiters; }
	private:
		unsigned& m_waiters;
	};

public:
	void push(T const& value)
	{
		bool wake;
		{
			std::unique_lock<std::mutex> lock(this->m_mutex);
			m_queue.push_front(value);
			wake = m_waiters != 0;
		}
		if (wake)
			this->m_condition.notify_one();
	}

	void pushBack(T const& value)
	{
		bool wake;
		{
			std::unique_lock<std::mutex> lock(this->m_mutex);
			m_queue.push_back(value);
			wake = m_waiters != 0;
		}
		if (wake)
			this->m_condition.notify_one();
	}

	T pop()
	{
		std::unique_lock<std::mutex> lock(this->m_mutex);
		{
			WaiterScope waiting(m_waiters);
			this->m_condition.wait(lock, [this]{ return !this->m_queue.empty(); });
		}
		T rc(std::move(this->m_queue.back()));
		this->m_queue.pop_back();
		return rc;
	}

	bool tryPop(T & v, std::chrono::milliseconds dur)
	{
		std::unique_lock<std::mutex> lock(this->m_mutex);
		{
			WaiterScope waiting(m_waiters);
			if (!this->m_condition.wait_for(lock, dur, [this]{ return !this->m_queue.empty(); }))
				return false;
		}
		v = std::move(this->m_queue.back());
		this->m_queue.pop_back();
		return true;
	}

	size_t size()
	{
		return m_queue.size();
	}
};
