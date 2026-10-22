#pragma once

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define TESTRUN(cmd, msg)                                                                                              \
	int res = cmd;                                                                                                     \
	if (res != 0) {                                                                                                    \
		printf(msg, res);                                                                                              \
		exit(res);                                                                                                     \
	}

/// Mutex class.
class Mutex
{
  public:
	/// Constructor.
	Mutex() { TESTRUN(pthread_mutex_init(&mtx, NULL), "Mutex init failed with msg %d") }

	/// Destructor.
	virtual ~Mutex() { TESTRUN(pthread_mutex_destroy(&mtx), "Mutex destroy failed with msg %d") }

	/// Lock the mutex.
	void lock() { TESTRUN(pthread_mutex_lock(&mtx), "Mutex lock failed with msg %d") }

	/// Unlock the mutex.
	void unlock() { TESTRUN(pthread_mutex_unlock(&mtx), "Mutex unlock failed with msg %d") }

	/// Try to lock the mutex, return true if succes else false.
	bool tryLock()
	{
		// return true if lock acquired
		return pthread_mutex_trylock(&mtx) == 0;
	}

  protected:
	/// A pthread mutex.
	pthread_mutex_t mtx;
};

/// Stack of every Thread. The default stack of a thread follows `ulimit -s`, and is only 2 MB when the limit is
/// unlimited (common on clusters), while the solvers recurse deeply on large instances. Only the touched pages use
/// memory.
constexpr size_t THREAD_STACK_SIZE = 256UL << 20;

/// Alternate signal stack of each thread, used by the crash handler of painless.cpp: after a stack overflow the
/// thread's own stack has no room left to run a handler.
constexpr size_t SIGNAL_STACK_SIZE = 64UL << 10;

/// Installs an alternate signal stack for the calling thread. It is never freed: a thread may receive a signal until it
/// exits, and the threads are few and long-lived.
inline void
installSignalStack()
{
	stack_t stack;
	stack.ss_sp = malloc(SIGNAL_STACK_SIZE);
	stack.ss_size = SIGNAL_STACK_SIZE;
	stack.ss_flags = 0;
	if (stack.ss_sp)
		sigaltstack(&stack, NULL);
}

/// Thread class
class Thread
{
  public:
	/// Constructor: the thread gets a stack of THREAD_STACK_SIZE (the default one if it cannot be allocated) and an
	/// alternate signal stack.
	Thread(void* (*main)(void*), void* arg)
	{
		Start* start = new Start{ main, arg };
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE);
		if (pthread_create(&myTid, &attr, trampoline, start) != 0)
			pthread_create(&myTid, NULL, trampoline, start);
		pthread_attr_destroy(&attr);
	}

	/// Join the thread.
	void join() { pthread_join(myTid, NULL); }

	void setThreadAffinity(int coreId)
	{
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(coreId, &cpuset);

		pthread_setaffinity_np(this->myTid, sizeof(cpu_set_t), &cpuset);
	}

  protected:
	/// The id of the pthread.
	pthread_t myTid;

  private:
	/// Function and argument of the thread, passed to the trampoline.
	struct Start
	{
		void* (*main)(void*);
		void* arg;
	};

	/// Entry point of every thread: installs its alternate signal stack, then runs its function.
	static void* trampoline(void* data)
	{
		Start start = *(Start*)data;
		delete (Start*)data;
		installSignalStack();
		return start.main(start.arg);
	}
};
