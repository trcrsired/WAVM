#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include <vector>
#include "POSIXPrivate.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Platform/Diagnostics.h"

using namespace WAVM;
using namespace WAVM::Platform;

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#define UC_RESET_ALT_STACK 0x80000000
extern "C" int __sigreturn(ucontext_t*, int);

// libunwind's SPI for JITs: a process-global lookup callback that reports the unwind info
// sections covering an address that isn't in a normal Mach-O image. This is defined in
// libunwind's private libunwind_ext.h, which isn't in the public SDK, so declare it here.
struct unw_dynamic_unwind_sections
{
	Uptr dso_base;
	Uptr dwarf_section;
	Uptr dwarf_section_length;
	Uptr compact_unwind_section;
	Uptr compact_unwind_section_length;
};
extern "C" int __unw_add_find_dynamic_unwind_sections(
	int (*)(Uptr addr, unw_dynamic_unwind_sections*));
#endif

thread_local SignalContext* Platform::innermostSignalContext = nullptr;

struct ScopedSignalContext : SignalContext
{
	bool isLinked = false;

	void link()
	{
		outerContext = innermostSignalContext;
		innermostSignalContext = this;
		isLinked = true;
	}

	~ScopedSignalContext()
	{
		if(isLinked)
		{
			innermostSignalContext = outerContext;
			isLinked = false;
		}
	}
};

static void maskSignals(int how)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGFPE);
	sigaddset(&set, SIGSEGV);
	sigaddset(&set, SIGBUS);
	pthread_sigmask(how, &set, nullptr);
}

[[noreturn]] static void signalHandler(int signalNumber, siginfo_t* signalInfo, void*)
{
	maskSignals(SIG_BLOCK);

	Signal signal;

	// Derive the exception cause the from signal that was received.
	switch(signalNumber)
	{
	case SIGFPE:
		if(signalInfo->si_code != FPE_INTDIV && signalInfo->si_code != FPE_INTOVF)
		{
			Errors::fatalfWithCallStack("unknown SIGFPE code");
		}
		signal.type = Signal::Type::intDivideByZeroOrOverflow;
		break;
	case SIGSEGV:
	case SIGBUS: {
		// Determine whether the faulting address was an address reserved by the stack.
		U8* stackMinGuardAddr;
		U8* stackMinAddr;
		U8* stackMaxAddr;
		sigAltStack.getNonSignalStack(stackMinGuardAddr, stackMinAddr, stackMaxAddr);
		signal.type = signalInfo->si_addr >= stackMinGuardAddr && signalInfo->si_addr < stackMaxAddr
						  ? Signal::Type::stackOverflow
						  : Signal::Type::accessViolation;
		signal.accessViolation.address = reinterpret_cast<Uptr>(signalInfo->si_addr);
		break;
	}
	default: Errors::fatalfWithCallStack("unknown signal number: %i", signalNumber); break;
	};

	// Capture the execution context, omitting this function and the function that called it, so the
	// top of the callstack is the function that triggered the signal.
	CallStack callStack = captureCallStack(2);

	// Undo the -1 offset that captureCallStack applied to the trapping IP on the assumption that
	// the signal trampoline frame is returning from an ordinary call.
	if(callStack.frames.size()) { callStack.frames[0].ip += 1; }

	// Call the signal handlers, from innermost to outermost, until one returns true.
	for(SignalContext* signalContext = innermostSignalContext; signalContext;
		signalContext = signalContext->outerContext)
	{
		if(signalContext->filter(signalContext->filterArgument, signal, std::move(callStack)))
		{
			// siglongjmp won't unwind the stack, so manually call the CallStack destructor.
			callStack.~CallStack();

			// Jump back to the execution context that was saved in catchSignals.
			siglongjmp(signalContext->catchJump, 1);
		}
	}

	switch(signalNumber)
	{
	case SIGFPE: Errors::fatalfWithCallStack("unhandled SIGFPE");
	case SIGSEGV: Errors::fatalfWithCallStack("unhandled SIGSEGV");
	case SIGBUS: Errors::fatalfWithCallStack("unhandled SIGBUS");
	default: WAVM_UNREACHABLE();
	};
}

bool Platform::initGlobalSignalsOnce()
{
	// Set the signal handler for the signals we want to intercept.
	struct sigaction signalAction;
	signalAction.sa_sigaction = signalHandler;
	signalAction.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
	sigemptyset(&signalAction.sa_mask);
	WAVM_ERROR_UNLESS(!sigaction(SIGSEGV, &signalAction, nullptr));
	WAVM_ERROR_UNLESS(!sigaction(SIGBUS, &signalAction, nullptr));
	WAVM_ERROR_UNLESS(!sigaction(SIGFPE, &signalAction, nullptr));

	return true;
}

bool Platform::catchSignals(void (*thunk)(void*),
							bool (*filter)(void*, Signal, CallStack&&),
							void* argument)
{
	initThreadAndGlobalSignals();

	ScopedSignalContext signalContext;
	signalContext.filter = filter;
	signalContext.filterArgument = argument;

#ifdef __WAVIX__
	Errors::unimplemented("Wavix catchSignals");
#else
	// Use sigsetjmp to capture the execution state into the signal context. If a signal is raised,
	// the signal handler will jump back to here. Tell sigsetjmp not to save the signal mask, since
	// that's quite expensive (a syscall). Instead, just unblock the signals that our handler blocks
	// after handling those signals.
	bool isReturningFromSignalHandler = sigsetjmp(signalContext.catchJump, 0) != 0;
	if(!isReturningFromSignalHandler)
	{
		signalContext.link();

		// Call the thunk.
		thunk(argument);
	}
	else
	{
#if defined(__APPLE__)
		// On MacOS, it's necessary to call __sigreturn to restore the sigaltstack state after
		// exiting the signal handler.
		__sigreturn(nullptr, UC_RESET_ALT_STACK);
#endif

		// Unblock the signals that are blocked by the signal handler.
		maskSignals(SIG_UNBLOCK);
	}
#endif

	return isReturningFromSignalHandler;
}

// The LLVM project libunwind implementation that WAVM uses matches the Apple ABI, which expects
// __register_frame and __deregister_frame to be called for each FDE in the .eh_frame section.
#if WAVM_ENABLE_UNWIND || defined(__APPLE__)
static void visitFDEs(const U8* ehFrames, Uptr numBytes, void (*visitFDE)(const void*))
{
	const U8* next = ehFrames;
	const U8* end = ehFrames + numBytes;
	do {
		const U8* cfi = next;
		Uptr numCFIBytes = *((const U32*)next);
		next += 4;
		if(numBytes == 0xffffffff)
		{
			const U64 numCFIBytes64 = *((const U64*)next);
			WAVM_ERROR_UNLESS(numCFIBytes64 <= UINTPTR_MAX);
			numCFIBytes = Uptr(numCFIBytes64);
			next += 8;
		}
		const U32 cieOffset = *((const U32*)next);
		if(cieOffset != 0) { visitFDE(cfi); }

		next += numCFIBytes;
	} while(next < end);
}

#if defined(__APPLE__)
// Since macOS 15, Apple's libunwind reads the unwind_proc_info_t.extra field — the
// mach_header of the image containing the frame — when _Unwind_SetIP redirects execution
// to a landing pad, to determine whether the image uses arm64e pointer authentication.
// FDEs registered through __register_frame have no associated dso, so extra is zero and
// libunwind dereferences a null pointer. Reporting the JIT eh_frame through a dynamic
// unwind-sections finder makes libunwind use the section lookup path, which carries a
// valid dso_base.
struct JITEHFrames
{
	const U8* codeStart;
	Uptr codeNumBytes;
	const U8* ehFrames;
	Uptr ehFramesNumBytes;
};
static std::mutex jitEHFramesMutex;
static std::vector<JITEHFrames> jitEHFrames;

static int findJITEHFramesSection(Uptr addr, unw_dynamic_unwind_sections* info)
{
	std::lock_guard<std::mutex> lock(jitEHFramesMutex);
	for(const JITEHFrames& section : jitEHFrames)
	{
		if(addr >= (Uptr)section.codeStart && addr - (Uptr)section.codeStart < section.codeNumBytes)
		{
			// libunwind reads the dso's mach_header to determine the image's pointer
			// authentication mode; report the main executable's header so the JIT code is
			// treated with the process's actual pointer authentication scheme.
			info->dso_base = (Uptr)_dyld_get_image_header(0);
			info->dwarf_section = (Uptr)section.ehFrames;
			info->dwarf_section_length = section.ehFramesNumBytes;
			info->compact_unwind_section = 0;
			info->compact_unwind_section_length = 0;
			return 1;
		}
	}
	return 0;
}
#endif

void Platform::registerEHFrames(const U8* imageBase,
								Uptr imageNumBytes,
								const U8* ehFrames,
								Uptr numBytes)
{
	visitFDEs(ehFrames, numBytes, __register_frame);
#if defined(__APPLE__)
	{
		std::lock_guard<std::mutex> lock(jitEHFramesMutex);
		if(!jitEHFrames.size()) { __unw_add_find_dynamic_unwind_sections(findJITEHFramesSection); }
		jitEHFrames.push_back({imageBase, imageNumBytes, ehFrames, numBytes});
	}
#endif
}

void Platform::deregisterEHFrames(const U8* imageBase,
								  Uptr imageNumBytes,
								  const U8* ehFrames,
								  Uptr numBytes)
{
	visitFDEs(ehFrames, numBytes, __deregister_frame);
#if defined(__APPLE__)
	{
		std::lock_guard<std::mutex> lock(jitEHFramesMutex);
		for(auto it = jitEHFrames.begin(); it != jitEHFrames.end(); ++it)
		{
			if(it->ehFrames == ehFrames)
			{
				jitEHFrames.erase(it);
				break;
			}
		}
	}
#endif
}
#else
void Platform::registerEHFrames(const U8* imageBase,
								Uptr imageNumBytes,
								const U8* ehFrames,
								Uptr numBytes)
{
	__register_frame(ehFrames);
}

void Platform::deregisterEHFrames(const U8* imageBase,
								  Uptr imageNumBytes,
								  const U8* ehFrames,
								  Uptr numBytes)
{
	__deregister_frame(ehFrames);
}
#endif
