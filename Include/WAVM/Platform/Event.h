#pragma once

#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Time.h"
#include "WAVM/Platform/Defines.h"
#ifndef _WIN32
#if defined(__WAVIX__)
namespace WAVM {
	inline constexpr size_t wavm_pthread_mutex_words = 6;
	inline constexpr size_t wavm_pthread_cond_words = 12;
}
#else
#include <pthread.h>
namespace WAVM {
	inline constexpr size_t wavm_pthread_mutex_words
		= (sizeof(pthread_mutex_t) + sizeof(Uptr) - 1) / sizeof(Uptr);
	inline constexpr size_t wavm_pthread_cond_words
		= (sizeof(pthread_cond_t) + sizeof(Uptr) - 1) / sizeof(Uptr);
}
#endif
#endif

namespace WAVM { namespace Platform {
	// Platform-independent events.
	struct Event
	{
		WAVM_API Event();
		WAVM_API ~Event();

		// Don't allow copying or moving an Event.
		Event(const Event&) = delete;
		Event(Event&&) = delete;
		void operator=(const Event&) = delete;
		void operator=(Event&&) = delete;

		// Wait for the event to be signaled. Cancels the wait after waitDuration has elapsed.
		// Returns true if the event was signaled, false if the wait was cancelled.
		WAVM_API bool wait(Time waitDuration);
		WAVM_API void signal();

	private:
#ifdef _WIN32
		void* handle;
#else
		struct PthreadMutex
		{
			Uptr data[::WAVM::wavm_pthread_mutex_words];
		} pthreadMutex;
		struct PthreadCond
		{
			Uptr data[::WAVM::wavm_pthread_cond_words];
		} pthreadCond;
#endif
	};
}}
