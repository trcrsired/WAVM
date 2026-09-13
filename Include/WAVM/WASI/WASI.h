#pragma once

#include <functional>
#include <memory>
#include "WAVM/IR/FeatureSpec.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Runtime/Runtime.h"

namespace WAVM { namespace VFS {
	struct FileSystem;
	struct VFD;
}}

namespace WAVM { namespace Runtime {
	struct Resolver;
}}

namespace WAVM { namespace WASI {

	struct Process;

	WAVM_API std::shared_ptr<Process> createProcess(Runtime::Compartment* compartment,
													std::vector<std::string>&& inArgs,
													std::vector<std::string>&& inEnvs,
													VFS::FileSystem* fileSystem,
													VFS::VFD* stdIn,
													VFS::VFD* stdOut,
													VFS::VFD* stdErr);

	WAVM_API std::shared_ptr<Process> createProcessWithFeatureSpec(
		Runtime::Compartment* compartment,
		std::vector<std::string>&& inArgs,
		std::vector<std::string>&& inEnvs,
		VFS::FileSystem* fileSystem,
		VFS::VFD* stdIn,
		VFS::VFD* stdOut,
		VFS::VFD* stdErr,
		::WAVM::IR::FeatureSpec const& featureSpec);

	WAVM_API Runtime::Resolver& getProcessResolver(Process& process);

	// Grants or revokes the process's permission to use the network. When disabled (the
	// default), all sock_* syscalls fail with __WASI_ENOTCAPABLE.
	WAVM_API void setNetworkEnabled(Process& process, bool isEnabled);

	// Adds a host socket to the process's FD table. If isListening is true the FD gets the
	// right to accept connections; otherwise it gets read/write/shutdown rights. Returns the
	// new WASI FD, or -1 if the FD table is full (the VFD is closed in that case).
	WAVM_API I32 addSocketFD(Process& process, VFS::VFD* socketVFD, bool isListening);

	WAVM_API Process* getProcessFromContextRuntimeData(Runtime::ContextRuntimeData*);
	WAVM_API Runtime::Memory* getProcessMemory(const Process& process);
	WAVM_API void setProcessMemory(Process& process, Runtime::Memory* memory);

	enum class SyscallTraceLevel
	{
		none,
		syscalls,
		syscallsWithCallstacks
	};

	WAVM_API void setSyscallTraceLevel(SyscallTraceLevel newLevel);

	WAVM_API I32 catchExit(std::function<I32()>&& thunk);
}}
