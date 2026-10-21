#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Inline/I128.h"
#include "WAVM/Inline/Time.h"
#include "WAVM/Platform/Clock.h"
#include "WAVM/Platform/Defines.h"
#include "WAVM/Platform/Event.h"

using namespace WAVM;
using namespace WAVM::Platform;

Platform::Event::Event()
{
	static_assert(sizeof(pthreadMutex) == sizeof(pthread_mutex_t), "");
	static_assert(alignof(PthreadMutex) >= alignof(pthread_mutex_t), "");

	static_assert(sizeof(pthreadCond) == sizeof(pthread_cond_t), "");
	static_assert(alignof(PthreadCond) >= alignof(pthread_cond_t), "");

	pthread_condattr_t conditionVariableAttr;
	WAVM_ERROR_UNLESS(!pthread_condattr_init(&conditionVariableAttr));

// Set the condition variable to use the monotonic clock for wait timeouts.
#ifndef __APPLE__
	WAVM_ERROR_UNLESS(!pthread_condattr_setclock(&conditionVariableAttr, CLOCK_MONOTONIC));
#endif

	WAVM_ERROR_UNLESS(!pthread_cond_init((pthread_cond_t*)&pthreadCond, nullptr));
	WAVM_ERROR_UNLESS(!pthread_mutex_init((pthread_mutex_t*)&pthreadMutex, nullptr));

	WAVM_ERROR_UNLESS(!pthread_condattr_destroy(&conditionVariableAttr));
}

Platform::Event::~Event()
{
	pthread_cond_destroy((pthread_cond_t*)&pthreadCond);
	WAVM_ERROR_UNLESS(!pthread_mutex_destroy((pthread_mutex_t*)&pthreadMutex));
}

bool Platform::Event::wait(Time waitDuration)
{
	WAVM_ERROR_UNLESS(!pthread_mutex_lock((pthread_mutex_t*)&pthreadMutex));

	// Like the Windows auto-reset event, a signal is sticky: signal() records
	// it under this mutex, so one delivered before the wait blocks is not
	// lost. Callers (e.g. the atomics wait list) have no predicate of their
	// own to re-check after a wakeup, which is what pthread_cond_wait
	// normally requires.
	if(signaled)
	{
		signaled = false;
		WAVM_ERROR_UNLESS(!pthread_mutex_unlock((pthread_mutex_t*)&pthreadMutex));
		return true;
	}

	bool result;
	if(isInfinity(waitDuration))
	{
		int condResult;
		while(!signaled)
		{
			condResult
				= pthread_cond_wait((pthread_cond_t*)&pthreadCond, (pthread_mutex_t*)&pthreadMutex);
			if(condResult != 0) { WAVM_ERROR_UNLESS(!condResult); }
		}
		signaled = false;
		result = true;
	}
	else
	{
		// Use the non-POSIX relative time wait on Mac, and an absolute monotonic clock timeout on
		// other POSIX systems.
#ifndef __APPLE__
		const I128 untilTimeNS = getClockTime(Clock::monotonic).ns + waitDuration.ns;
#endif
		result = true;
		for(;;)
		{
			int condResult;
#ifdef __APPLE__
			timespec waitTimeSpec;
			waitTimeSpec.tv_sec = U64(waitDuration.ns / 1000000000);
			waitTimeSpec.tv_nsec = U64(waitDuration.ns % 1000000000);
			condResult = pthread_cond_timedwait_relative_np(
				(pthread_cond_t*)&pthreadCond, (pthread_mutex_t*)&pthreadMutex, &waitTimeSpec);
#else
			timespec untilTimeSpec;
			untilTimeSpec.tv_sec = U64(untilTimeNS / 1000000000);
			untilTimeSpec.tv_nsec = U64(untilTimeNS % 1000000000);
			condResult = pthread_cond_timedwait(
				(pthread_cond_t*)&pthreadCond, (pthread_mutex_t*)&pthreadMutex, &untilTimeSpec);
#endif
			if(condResult == ETIMEDOUT)
			{
				// A signaler that ran just before the timeout took the mutex
				// and set signaled; consume it like a signal that won.
				if(signaled) { result = true; }
				else
				{
					result = false;
				}
				break;
			}
			if(condResult != 0) { WAVM_ERROR_UNLESS(!condResult); }
			if(signaled)
			{
				result = true;
				break;
			}
		}
		if(result) { signaled = false; }
	}

	WAVM_ERROR_UNLESS(!pthread_mutex_unlock((pthread_mutex_t*)&pthreadMutex));
	return result;
}

void Platform::Event::signal()
{
	WAVM_ERROR_UNLESS(!pthread_mutex_lock((pthread_mutex_t*)&pthreadMutex));
	signaled = true;
	WAVM_ERROR_UNLESS(!pthread_cond_signal((pthread_cond_t*)&pthreadCond));
	WAVM_ERROR_UNLESS(!pthread_mutex_unlock((pthread_mutex_t*)&pthreadMutex));
}
