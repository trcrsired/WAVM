#include "WAVM/WASI/WASI.h"
#include "./WASIPrivate.h"
#include <atomic>
#include <cstdlib>
#include <memory>
#include <vector>
#include "WAVM/IR/Types.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Inline/IndexMap.h"
#include "WAVM/Logging/Logging.h"
#include "WAVM/Platform/Clock.h"
#include "WAVM/Platform/Defines.h"
#include "WAVM/Platform/Diagnostics.h"
#include "WAVM/Platform/File.h"
#include "WAVM/Platform/Intrinsic.h"
#include "WAVM/Platform/Random.h"
#include "WAVM/Platform/Socket.h"
#include "WAVM/Platform/Thread.h"
#include "WAVM/Runtime/Intrinsics.h"
#include "WAVM/Runtime/Linker.h"
#include "WAVM/Runtime/Runtime.h"
#include "WAVM/VFS/VFS.h"
#include "WAVM/WASI/WASI.h"
#include "WAVM/WASI/WASIABI64.h"

using namespace WAVM;
using namespace WAVM::IR;
using namespace WAVM::Runtime;
using namespace WAVM::WASI;

namespace WAVM { namespace WASI {
	WAVM_DEFINE_INTRINSIC_MODULE(wasi);
}}

// The WASI threads extension's thread-spawn syscall.
//
// The guest passes an opaque start_arg. The host creates a native thread that runs the module's
// exported wasi_thread_start(thread_id, start_arg) entry point on a fresh execution context. The
// context gives the new thread its own copy of the module's mutable globals -- notably the C ABI's
// __stack_pointer and __tls_base, which wasi_thread_start sets from start_arg -- while every
// thread shares the module's memory, tables, and instance. This is the same model WAVM already
// uses for Emscripten's _pthread_create.
struct SpawnedThread
{
	Process* process;
	Runtime::Context* context;
	Runtime::Function* startFunction;
	U32 threadId;
	U32 startArg;
};

static I64 spawnedThreadEntry(void* argument)
{
	std::unique_ptr<SpawnedThread> thread(static_cast<SpawnedThread*>(argument));

	try
	{
		UntaggedValue arguments[2];
		arguments[0].i32 = thread->threadId;
		arguments[1].i32 = thread->startArg;

		Runtime::catchRuntimeExceptions(
			[&] {
				// The spawned thread must use the same memtag mode as the main thread, so the
				// invoke thunk sets up the context's memtag base and random-buffer pointers the
				// same way the main thread's did.
				Runtime::invokeFunctionWithMemTag(
					thread->context,
					thread->startFunction,
					FunctionType(TypeTuple{}, TypeTuple{ValueType::i32, ValueType::i32}),
					arguments,
					nullptr,
					thread->process->memtagStatus);
			},
			[](Runtime::Exception* exception) {
				Errors::fatalf("Exception in spawned WASI thread: %s",
							   Runtime::describeException(exception).c_str());
			});
	}
	catch(ExitException const& exitException)
	{
		// A spawned thread called proc_exit, which terminates the whole process.
		exit(I32(exitException.exitCode));
	}

	return 0;
}

static U32 spawnThread(Process& process, U32 startArg)
{
	Runtime::Instance* instance = process.instance;
	if(!instance) { return __WASI_EPERM; }

	Runtime::Function* startFunction
		= asFunctionNullable(getInstanceExport(instance, "wasi_thread_start"));
	if(!startFunction
	   || getFunctionType(startFunction)
			  != FunctionType(TypeTuple{}, TypeTuple{ValueType::i32, ValueType::i32}))
	{
		Log::printf(Log::error,
					"The module does not export a wasi_thread_start(int, int) entry point, so it "
					"cannot spawn threads.\n");
		return __WASI_EPERM;
	}

	Runtime::Context* context = Runtime::createContext(process.compartment);
	if(!context) { return __WASI_EAGAIN; }

	// Assign each thread a unique, non-zero ID. The WASI threads ABI reserves 0 and requires that
	// a live thread's ID not be reused, which a monotonically increasing counter guarantees.
	static std::atomic<U32> nextThreadId{1};
	const U32 threadId = nextThreadId.fetch_add(1, std::memory_order_relaxed);
	if(threadId == 0) { return __WASI_EAGAIN; }

	SpawnedThread* thread = new SpawnedThread{&process, context, startFunction, threadId, startArg};

	Platform::Thread* platformThread = Platform::createThread(0, spawnedThreadEntry, thread);
	// The WASI threads ABI has no host-visible join: the guest tracks completion itself. Detaching
	// lets the platform reclaim the thread's resources once the entry point returns.
	Platform::detachThread(platformThread);

	return __WASI_ESUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
							   "thread-spawn",
							   __wasi_errno_return_t,
							   wasi_thread_spawn,
							   U32 startArg)
{
	TRACE_SYSCALL("thread-spawn", "(%u)", startArg);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	return TRACE_SYSCALL_RETURN(spawnThread(*process, startArg));
}

bool ProcessResolver::resolve(const std::string& moduleName,
							  const std::string& exportName,
							  ExternType type,
							  Object*& outObject)
{
	const auto& namedInstance = moduleNameToInstanceMap.get(moduleName);
	if(namedInstance)
	{
		outObject = getInstanceExport(*namedInstance, exportName);
		if(outObject)
		{
			if(isA(outObject, type)) { return true; }
			else
			{
				Log::printf(Log::debug,
							"Resolved import %s.%s to a %s, but was expecting %s\n",
							moduleName.c_str(),
							exportName.c_str(),
							asString(getExternType(outObject)).c_str(),
							asString(type).c_str());
				return false;
			}
		}
	}

	return false;
}

WASI::Process::~Process()
{
	for(const std::shared_ptr<WASI::FDE>& fde : fdMap)
	{
		VFS::Result result = fde->close();
		if(result != VFS::Result::success)
		{
			Log::printf(Log::Category::debug,
						"Error while closing file because of process exit: %s\n",
						VFS::describeResult(result));
		}
	}
}

std::shared_ptr<Process> WASI::createProcessWithFeatureSpec(Runtime::Compartment* compartment,
															std::vector<std::string>&& inArgs,
															std::vector<std::string>&& inEnvs,
															VFS::FileSystem* fileSystem,
															VFS::VFD* stdIn,
															VFS::VFD* stdOut,
															VFS::VFD* stdErr,
															FeatureSpec const& featureSpec)
{
	std::shared_ptr<Process> process = std::make_shared<Process>();
	process->args = std::move(inArgs);
	process->envs = std::move(inEnvs);
	process->fileSystem = fileSystem;

	process->compartment = compartment;
	setUserData(process->compartment, process.get(), nullptr);

	// Record the memtag mode the module was loaded with. spawnThread passes this to
	// invokeFunctionWithMemTag for the spawned thread's entry point so the invoke thunk sets up
	// the memtag context the same way the main thread's did.
	if(featureSpec.memtagMte || featureSpec.memtagMteSync)
	{
		process->memtagStatus = ::WAVM::LLVMJIT::memtagStatus::armmte;
	}
	else if(featureSpec.memtagMteIrg || featureSpec.memtagMteSyncIrg)
	{
		process->memtagStatus = ::WAVM::LLVMJIT::memtagStatus::armmteirg;
	}
	else if(featureSpec.memtagFull)
	{
		process->memtagStatus = ::WAVM::LLVMJIT::memtagStatus::full;
	}
	else if(featureSpec.memtag)
	{
		process->memtagStatus = ::WAVM::LLVMJIT::memtagStatus::basic;
	}

	Instance* wasi_snapshot_preview1
		= Intrinsics::instantiateModuleWithFeatureSpec(compartment,
													   {WAVM_INTRINSIC_MODULE_REF(wasi),
														WAVM_INTRINSIC_MODULE_REF(wasiArgsEnvs),
														WAVM_INTRINSIC_MODULE_REF(wasiClocks),
														WAVM_INTRINSIC_MODULE_REF(wasiFile)},
													   "wasi_snapshot_preview1",
													   featureSpec);

	process->resolver.moduleNameToInstanceMap.set("wasi_unstable", wasi_snapshot_preview1);
	process->resolver.moduleNameToInstanceMap.set("wasi_snapshot_preview1", wasi_snapshot_preview1);
	// The WASI threads proposal imports thread-spawn from the module named "wasi" (as opposed to
	// the older wasi_unstable / wasi_snapshot_preview1 names).
	process->resolver.moduleNameToInstanceMap.set("wasi", wasi_snapshot_preview1);

	__wasi_rights_t stdioRights = __WASI_RIGHT_FD_READ | __WASI_RIGHT_FD_FDSTAT_SET_FLAGS
								  | __WASI_RIGHT_FD_WRITE | __WASI_RIGHT_FD_FILESTAT_GET
								  | __WASI_RIGHT_POLL_FD_READWRITE;

	process->fdMap.insertOrFail(0, std::make_shared<FDE>(stdIn, stdioRights, 0, "/dev/stdin"));
	process->fdMap.insertOrFail(1, std::make_shared<FDE>(stdOut, stdioRights, 0, "/dev/stdout"));
	process->fdMap.insertOrFail(2, std::make_shared<FDE>(stdErr, stdioRights, 0, "/dev/stderr"));

	if(fileSystem)
	{
		// Map the root directory as both / and ., which allows files to be opened from it using
		// either "/file" or just "file".
		const char* preopenedRootAliases[2] = {"/", "."};
		for(Uptr aliasIndex = 0; aliasIndex < 2; ++aliasIndex)
		{
			VFS::VFD* rootFD = nullptr;
			VFS::Result openResult = fileSystem->open(
				"/", VFS::FileAccessMode::none, VFS::FileCreateMode::openExisting, rootFD);
			if(openResult != VFS::Result::success)
			{
				Errors::fatalf("Error opening WASI root directory: %s",
							   VFS::describeResult(openResult));
			}

			process->fdMap.insertOrFail(
				3 + __wasi_fd_t(aliasIndex),
				std::make_shared<FDE>(rootFD,
									  DIRECTORY_RIGHTS,
									  INHERITING_DIRECTORY_RIGHTS,
									  preopenedRootAliases[aliasIndex],
									  true,
									  __wasi_preopentype_t(__WASI_PREOPENTYPE_DIR)));
		}
	}

	process->processClockOrigin = Platform::getClockTime(Platform::Clock::processCPUTime);

	return process;
}

std::shared_ptr<Process> WASI::createProcess(Runtime::Compartment* compartment,
											 std::vector<std::string>&& inArgs,
											 std::vector<std::string>&& inEnvs,
											 VFS::FileSystem* fileSystem,
											 VFS::VFD* stdIn,
											 VFS::VFD* stdOut,
											 VFS::VFD* stdErr)
{
	::WAVM::IR::FeatureSpec featureSpec(::FeatureLevel::wavm);
	featureSpec.memtagMteIrg = featureSpec.memtagMteSyncIrg = featureSpec.memtagMteSync
		= featureSpec.memtagMte = featureSpec.memtagFull = featureSpec.memtag = false;
	return WASI::createProcessWithFeatureSpec(compartment,
											  ::std::move(inArgs),
											  ::std::move(inEnvs),
											  fileSystem,
											  stdIn,
											  stdOut,
											  stdErr,
											  featureSpec);
}

Resolver& WASI::getProcessResolver(Process& process) { return process.resolver; }

void WASI::setNetworkEnabled(Process& process, bool isEnabled)
{
	process.networkEnabled = isEnabled;
}

Process* WASI::getProcessFromContextRuntimeData(Runtime::ContextRuntimeData* contextRuntimeData)
{
	return (Process*)Runtime::getUserData(
		Runtime::getCompartmentFromContextRuntimeData(contextRuntimeData));
}

Memory* WASI::getProcessMemory(const Process& process) { return process.memory; }
void WASI::setProcessMemory(Process& process, Memory* memory) { process.memory = memory; }

Instance* WASI::getProcessInstance(const Process& process) { return process.instance; }
void WASI::setProcessInstance(Process& process, Instance* instance)
{
	process.instance = instance;
}

I32 WASI::catchExit(std::function<I32()>&& thunk)
{
	try
	{
		return std::move(thunk)();
	}
	catch(ExitException const& exitException)
	{
		return I32(exitException.exitCode);
	}
}

#include "DefineIntrinsicsI32.h"
#include "WASIOther.h"
#if UINT32_MAX < SIZE_MAX
#include "DefineIntrinsicsI64.h"
#include "WASIOther.h"
#endif
