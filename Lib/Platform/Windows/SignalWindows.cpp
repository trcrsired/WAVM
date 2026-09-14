#include <atomic>
#include <functional>
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Platform/Defines.h"
#include "WAVM/Platform/Signal.h"
#include "WindowsPrivate.h"

#include <malloc.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef min
#undef max

#if defined(_MSC_VER) && !defined(__clang__)
#define WAVMSIGNALENABLESEH
#endif

using namespace WAVM;
using namespace WAVM::Platform;

void Platform::registerEHFrames(const U8* imageBase,
								Uptr imageNumBytes,
								const U8* ehFrames,
								Uptr numBytes)
{
#ifdef _WIN64
	const U32 numFunctions = (U32)(numBytes / sizeof(RUNTIME_FUNCTION));

	// Register our manually fixed up copy of the function table.
	if(!RtlAddFunctionTable(
		   (RUNTIME_FUNCTION*)ehFrames, numFunctions, reinterpret_cast<ULONG_PTR>(imageBase)))
	{
		Errors::fatal("RtlAddFunctionTable failed");
	}
#else
	Errors::fatal("registerEHFrames isn't implemented on 32-bit Windows");
#endif
}
void Platform::deregisterEHFrames(const U8* imageBase,
								  Uptr imageNumBytes,
								  const U8* ehFrames,
								  Uptr numBytes)
{
#ifdef _WIN64
	RtlDeleteFunctionTable((RUNTIME_FUNCTION*)ehFrames);
#else
	Errors::fatal("deregisterEHFrames isn't implemented on 32-bit Windows");
#endif
}

static bool translateSEHToSignal(EXCEPTION_POINTERS* exceptionPointers, Signal& outSignal)
{
	// Decide how to handle this exception code.
	switch(exceptionPointers->ExceptionRecord->ExceptionCode)
	{
	case EXCEPTION_ACCESS_VIOLATION: {
		outSignal.type = Signal::Type::accessViolation;
		outSignal.accessViolation.address
			= exceptionPointers->ExceptionRecord->ExceptionInformation[1];
		return true;
	}
	case EXCEPTION_STACK_OVERFLOW: outSignal.type = Signal::Type::stackOverflow; return true;
	case STATUS_INTEGER_DIVIDE_BY_ZERO:
		outSignal.type = Signal::Type::intDivideByZeroOrOverflow;
		return true;
	case STATUS_INTEGER_OVERFLOW:
		outSignal.type = Signal::Type::intDivideByZeroOrOverflow;
		return true;
	default: return false;
	}
}

#ifdef WAVMSIGNALENABLESEH

// __try/__except doesn't support locals with destructors in the same function, so this is just
// the body of the sehSignalFilterFunction __try pulled out into a function.
static LONG CALLBACK sehSignalFilterFunctionNonReentrant(EXCEPTION_POINTERS* exceptionPointers,
														 bool (*filter)(void*, Signal, CallStack&&),
														 void* context)
{
	Signal signal;
	if(!translateSEHToSignal(exceptionPointers, signal)) { return EXCEPTION_CONTINUE_SEARCH; }
	else
	{
		// Unwind the stack frames from the context of the exception.
		CallStack callStack = unwindStack(*exceptionPointers->ContextRecord, 0);

		if((*filter)(context, signal, std::move(callStack))) { return EXCEPTION_EXECUTE_HANDLER; }
		else { return EXCEPTION_CONTINUE_SEARCH; }
	}
}

static LONG CALLBACK sehSignalFilterFunction(EXCEPTION_POINTERS* exceptionPointers,
											 bool (*filter)(void*, Signal, CallStack&&),
											 void* context)
{
#ifdef WAVMSIGNALENABLESEH
	__try
	{
#endif
		return sehSignalFilterFunctionNonReentrant(exceptionPointers, filter, context);
#ifdef WAVMSIGNALENABLESEH
	}
	__except(Errors::fatal("reentrant exception"), true)
	{
		WAVM_UNREACHABLE();
	}
#endif
}
#else

// On non-MSVC compilers targeting Windows (e.g. clang/mingw), __try/__except isn't available,
// so hardware exceptions are caught with a vectored exception handler instead. If a filter
// accepts the exception, the handler copies the CONTEXT that catchSignals captured into the
// exception's context record and returns EXCEPTION_CONTINUE_EXECUTION, which resumes
// catchSignals just after the RtlCaptureContext call — the same structure as the POSIX
// sigsetjmp/siglongjmp implementation.
struct SignalContext
{
	bool (*filter)(void*, Signal, CallStack&&);
	void* filterArgument;
	SignalContext* outerContext;
	CONTEXT catchContext;
	volatile U32* caughtFlag;
};

// The per-thread signal context chain is stored in a TLS slot rather than a C++ thread_local:
// TlsGetValue is a plain TEB read with no lazy allocation, so it is safe to call inside the
// exception handler where a C++ thread_local could malloc on first use.
static DWORD signalContextTlsIndex = TLS_OUT_OF_INDEXES;

static SignalContext* getInnermostSignalContext()
{
	return reinterpret_cast<SignalContext*>(TlsGetValue(signalContextTlsIndex));
}

static LONG CALLBACK vectoredSignalHandler(EXCEPTION_POINTERS* exceptionPointers)
{
	SignalContext* signalContext = getInnermostSignalContext();
	if(!signalContext) { return EXCEPTION_CONTINUE_SEARCH; }

	Signal signal;
	if(!translateSEHToSignal(exceptionPointers, signal)) { return EXCEPTION_CONTINUE_SEARCH; }

	// Unwind the stack frames from the context of the exception.
	CallStack callStack = unwindStack(*exceptionPointers->ContextRecord, 0);

	for(; signalContext; signalContext = signalContext->outerContext)
	{
		if((*signalContext->filter)(
			   signalContext->filterArgument, signal, std::move(callStack)))
		{
			// Restoring the saved context abandons this handler's frame without unwinding, so
			// destroy the CallStack manually first, as the POSIX signal handler does before
			// siglongjmp.
			callStack.~CallStack();

			// Mark that the signal was caught, then resume the faulting thread at the context
			// captured by catchSignals.
			*signalContext->caughtFlag = 1;
			*exceptionPointers->ContextRecord = signalContext->catchContext;
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

static bool initVectoredSignalHandler()
{
	WAVM_ERROR_UNLESS((signalContextTlsIndex = TlsAlloc()) != TLS_OUT_OF_INDEXES);
	WAVM_ERROR_UNLESS(AddVectoredExceptionHandler(1, vectoredSignalHandler) != nullptr);
	return true;
}
#endif

bool Platform::catchSignals(void (*thunk)(void*),
							bool (*filter)(void*, Signal, CallStack&&),
							void* context)
{
	initThread();
#ifdef WAVMSIGNALENABLESEH
	__try
	{
		(*thunk)(context);
		return false;
	}
	__except(sehSignalFilterFunction(GetExceptionInformation(), filter, context))
	{
		// After a stack overflow, the stack will be left in a damaged state. Let the CRT repair
		// it.
		WAVM_ERROR_UNLESS(_resetstkoflw());

		return true;
	}
#else
	static bool vectoredHandlerRegistered = initVectoredSignalHandler();
	(void)vectoredHandlerRegistered;

	SignalContext signalContext;
	signalContext.filter = filter;
	signalContext.filterArgument = context;
	signalContext.outerContext = getInnermostSignalContext();

	volatile U32 caught = 0;
	signalContext.caughtFlag = &caught;

	// Capture the current execution context. If a signal is caught, vectoredSignalHandler sets
	// caught and restores this context, resuming execution at the following if. Link the
	// context only after it has been captured, so a signal raised before the thunk runs cannot
	// resume to an uninitialized context.
	RtlCaptureContext(&signalContext.catchContext);
	if(caught)
	{
		TlsSetValue(signalContextTlsIndex, signalContext.outerContext);

		// After a stack overflow, the stack will be left in a damaged state. Let the CRT repair
		// it.
		WAVM_ERROR_UNLESS(_resetstkoflw());
		return true;
	}

	TlsSetValue(signalContextTlsIndex, &signalContext);
	(*thunk)(context);
	TlsSetValue(signalContextTlsIndex, signalContext.outerContext);
	return false;
#endif
}
