static bool readUserString(Memory* memory,
						   WASIAddressIPtr stringAddress,
						   WASIAddressIPtr numStringBytes,
						   std::string& outString)
{
	outString.clear();

	bool succeeded = true;
	catchRuntimeExceptions(
		[&] {
			char* stringBytes = memoryArrayPtr<char>(memory, stringAddress, numStringBytes);
			for(Uptr index = 0; index < numStringBytes; ++index)
			{
				outString += stringBytes[index];
			}
		},
		[&succeeded](Exception* exception) {
			WAVM_ERROR_UNLESS(getExceptionType(exception)
							  == ExceptionTypes::outOfBoundsMemoryAccess);
			Log::printf(Log::debug,
						"Caught runtime exception while reading string at address 0x%" PRIx64,
						getExceptionArgument(exception, 1).i64);
			destroyException(exception);

			succeeded = false;
		});

	return succeeded;
}

static __wasi_errno_t validatePath(Process* process,
								   __wasi_fd_t dirFD,
								   __wasi_lookupflags_t lookupFlags,
								   __wasi_rights_t requiredDirRights,
								   __wasi_rights_t requiredDirInheritingRights,
								   WASIAddressIPtr pathAddress,
								   WASIAddressIPtr numPathBytes,
								   std::string& outCanonicalPath)
{
	if(!process->fileSystem) { return __WASI_ENOTCAPABLE; }

	LockedFDE lockedDirFDE
		= getLockedFDE(process, dirFD, requiredDirRights, requiredDirInheritingRights);
	if(lockedDirFDE.error != __WASI_ESUCCESS) { return lockedDirFDE.error; }

	std::string relativePath;
	if(!readUserString(process->memory, pathAddress, numPathBytes, relativePath))
	{
		return __WASI_EFAULT;
	}

	TRACE_SYSCALL_FLOW("Read path from process memory: %s", relativePath.c_str());

	if(!getCanonicalPath(lockedDirFDE.fde->originalPath, relativePath, outCanonicalPath))
	{
		return __WASI_ENOTCAPABLE;
	}

	TRACE_SYSCALL_FLOW("Canonical path: %s", outCanonicalPath.c_str());

	return __WASI_ESUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_prestat_get",
									__wasi_errno_return_t,
									wasi_fd_prestat_get,
									__wasi_fd_t fd,
									WASIAddressIPtr prestatAddress)
{
	TRACE_SYSCALL_IPTR("fd_prestat_get", "(%u, " WASIADDRESSIPTR_FORMAT ")", fd, prestatAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	LockedFDE lockedFDE = getLockedFDE(process, fd, 0, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	if(!lockedFDE.fde->isPreopened) { return TRACE_SYSCALL_RETURN(__WASI_EBADF); }

	if(lockedFDE.fde->originalPath.size() > WASIADDRESSIPTR_MAX)
	{
		return TRACE_SYSCALL_RETURN(__WASI_EOVERFLOW);
	}

	wasi_prestat_iptr& prestat = memoryRef<wasi_prestat_iptr>(process->memory, prestatAddress);
	prestat.pr_type = lockedFDE.fde->preopenedType;
	WAVM_ASSERT(lockedFDE.fde->preopenedType == __WASI_PREOPENTYPE_DIR);
	prestat.u.dir.pr_name_len = static_cast<WASIAddressIPtr>(lockedFDE.fde->originalPath.size());

	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_prestat_dir_name",
									__wasi_errno_return_t,
									wasi_fd_prestat_dir_name,
									__wasi_fd_t fd,
									WASIAddressIPtr bufferAddress,
									WASIAddressIPtr bufferLength)
{
	TRACE_SYSCALL_IPTR("fd_prestat_dir_name",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   fd,
					   bufferAddress,
					   bufferLength);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	LockedFDE lockedFDE = getLockedFDE(process, fd, 0, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	if(!lockedFDE.fde->isPreopened) { return TRACE_SYSCALL_RETURN(__WASI_EBADF); }

	if(bufferLength != lockedFDE.fde->originalPath.size())
	{
		return TRACE_SYSCALL_RETURN(__WASI_EINVAL);
	}

	char* buffer = memoryArrayPtr<char>(process->memory, bufferAddress, bufferLength);
	memcpy(buffer, lockedFDE.fde->originalPath.c_str(), bufferLength);

	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_close",
									__wasi_errno_return_t,
									wasi_fd_close,
									__wasi_fd_t fd)
{
	TRACE_SYSCALL_IPTR("fd_close", "(%u)", fd);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	// Exclusively lock the fds mutex, and look up the FDE corresponding to the FD.
	Platform::RWMutex::ExclusiveLock fdsLock(process->fdMapMutex);
	if(fd < process->fdMap.getMinIndex() || fd > process->fdMap.getMaxIndex())
	{
		return TRACE_SYSCALL_RETURN(__WASI_EBADF);
	}
	std::shared_ptr<FDE>* fdePointer = process->fdMap.get(fd);
	if(!fdePointer) { return TRACE_SYSCALL_RETURN(__WASI_EBADF); }
	std::shared_ptr<FDE> fde = *fdePointer;

	// Exclusively lock the FDE.
	Platform::RWMutex::ExclusiveLock fdeLock(fde->mutex);

	// Remove this FDE from the FD table, and unlock the fds mutex.
	process->fdMap.removeOrFail(fd);
	fdsLock.unlock();

	// Don't allow closing preopened FDs for now.
	if(fde->isPreopened) { return TRACE_SYSCALL_RETURN(__WASI_EBADF); }

	// Close the FDE's underlying VFD+DirEntStream. This can return an error code, but closes the
	// VFD+DirEntStream even if there was an error.
	const VFS::Result result = fde->close();

	return TRACE_SYSCALL_RETURN(asWASIErrNo(result));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_datasync",
									__wasi_errno_return_t,
									wasi_fd_datasync,
									__wasi_fd_t fd)
{
	TRACE_SYSCALL_IPTR("fd_datasync", "(%u)", fd);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	LockedFDE lockedFDE = getLockedFDE(process, fd, __WASI_RIGHT_FD_DATASYNC, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	return TRACE_SYSCALL_RETURN(asWASIErrNo(lockedFDE.fde->vfd->sync(SyncType::contents)));
}

static __wasi_errno_t readImpl(Process* process,
							   __wasi_fd_t fd,
							   WASIAddressIPtr iovsAddress,
							   WASIAddressIPtr numIOVs,
							   const __wasi_filesize_t* offset,
							   Uptr& outNumBytesRead)
{
	const __wasi_rights_t requiredRights
		= __WASI_RIGHT_FD_READ | (offset ? __WASI_RIGHT_FD_SEEK : 0);
	LockedFDE lockedFDE = getLockedFDE(process, fd, requiredRights, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return lockedFDE.error; }

	if(numIOVs > __WASI_IOV_MAX) { return __WASI_EINVAL; }

	// Allocate memory for the IOReadBuffers.
	IOReadBuffer* vfsReadBuffers = (IOReadBuffer*)malloc(numIOVs * sizeof(IOReadBuffer));
	if(vfsReadBuffers == nullptr) { return __WASI_ENOMEM; }

	// Catch any out-of-bounds memory access exceptions that are thrown.
	__wasi_errno_t result = __WASI_ESUCCESS;
	Runtime::catchRuntimeExceptions(
		[&] {
			// Translate the IOVs to IOReadBuffers.
			const wasi_iovec_iptr* iovs
				= memoryArrayPtr<wasi_iovec_iptr>(process->memory, iovsAddress, numIOVs);
			U64 numBufferBytes = 0;
			for(WASIAddressIPtr iovIndex = 0; iovIndex < numIOVs; ++iovIndex)
			{
				wasi_iovec_iptr iov = iovs[iovIndex];
				TRACE_SYSCALL_FLOW("IOV[" WASIADDRESSIPTR_FORMAT "]=(buf=" WASIADDRESSIPTR_FORMAT
								   ", buf_len=" WASIADDRESSIPTR_FORMAT ")",
								   iovIndex,
								   iov.buf,
								   iov.buf_len);
				vfsReadBuffers[iovIndex].data
					= memoryArrayPtr<U8>(process->memory, iov.buf, iov.buf_len);
				vfsReadBuffers[iovIndex].numBytes = iov.buf_len;
				numBufferBytes += iov.buf_len;
			}
			if(numBufferBytes > WASIADDRESSIPTR_MAX) { result = __WASI_EOVERFLOW; }
			else
			{
				// Do the read.
				result = asWASIErrNo(
					lockedFDE.fde->vfd->readv(vfsReadBuffers, numIOVs, &outNumBytesRead, offset));
			}
		},
		[&](Exception* exception) {
			// If we catch an out-of-bounds memory exception, return EFAULT.
			WAVM_ERROR_UNLESS(getExceptionType(exception)
							  == ExceptionTypes::outOfBoundsMemoryAccess);
			Log::printf(Log::debug,
						"Caught runtime exception while reading memory at address 0x%" PRIx64,
						getExceptionArgument(exception, 1).i64);
			destroyException(exception);
			result = __WASI_EFAULT;
		});

	// Free the VFS read buffers.
	free(vfsReadBuffers);

	return result;
}

static __wasi_errno_t writeImpl(Process* process,
								__wasi_fd_t fd,
								WASIAddressIPtr iovsAddress,
								WASIAddressIPtr numIOVs,
								const __wasi_filesize_t* offset,
								Uptr& outNumBytesWritten)
{
	const __wasi_rights_t requiredRights
		= __WASI_RIGHT_FD_WRITE | (offset ? __WASI_RIGHT_FD_SEEK : 0);
	LockedFDE lockedFDE = getLockedFDE(process, fd, requiredRights, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return lockedFDE.error; }

	if(numIOVs > __WASI_IOV_MAX) { return __WASI_EINVAL; }

	// Allocate memory for the IOWriteBuffers.
	IOWriteBuffer* vfsWriteBuffers = (IOWriteBuffer*)malloc(numIOVs * sizeof(IOWriteBuffer));
	if(vfsWriteBuffers == nullptr) { return __WASI_ENOMEM; }

	// Catch any out-of-bounds memory access exceptions that are thrown.
	__wasi_errno_t result = __WASI_ESUCCESS;
	Runtime::catchRuntimeExceptions(
		[&] {
			// Translate the IOVs to IOWriteBuffers
			const wasi_ciovec_iptr* iovs
				= memoryArrayPtr<wasi_ciovec_iptr>(process->memory, iovsAddress, numIOVs);
			U64 numBufferBytes = 0;
			for(WASIAddressIPtr iovIndex = 0; iovIndex < numIOVs; ++iovIndex)
			{
				wasi_ciovec_iptr iov = iovs[iovIndex];
				TRACE_SYSCALL_FLOW("IOV[" WASIADDRESSIPTR_FORMAT "]=(buf=" WASIADDRESSIPTR_FORMAT
								   ", buf_len=" WASIADDRESSIPTR_FORMAT ")",
								   iovIndex,
								   iov.buf,
								   iov.buf_len);
				vfsWriteBuffers[iovIndex].data
					= memoryArrayPtr<const U8>(process->memory, iov.buf, iov.buf_len);
				vfsWriteBuffers[iovIndex].numBytes = iov.buf_len;
				numBufferBytes += iov.buf_len;
			}
			if(numBufferBytes > WASIADDRESSIPTR_MAX) { result = __WASI_EOVERFLOW; }
			else
			{
				// Do the writes.
				result = asWASIErrNo(lockedFDE.fde->vfd->writev(
					vfsWriteBuffers, numIOVs, &outNumBytesWritten, offset));
			}
		},
		[&](Exception* exception) {
			// If we catch an out-of-bounds memory exception, return EFAULT.
			WAVM_ERROR_UNLESS(getExceptionType(exception)
							  == ExceptionTypes::outOfBoundsMemoryAccess);
			Log::printf(Log::debug,
						"Caught runtime exception while reading memory at address 0x%" PRIx64,
						getExceptionArgument(exception, 1).i64);
			destroyException(exception);
			result = __WASI_EFAULT;
		});

	// Free the VFS write buffers.
	free(vfsWriteBuffers);

	return result;
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_pread",
									__wasi_errno_return_t,
									wasi_fd_pread,
									__wasi_fd_t fd,
									WASIAddressIPtr iovsAddress,
									WASIAddressIPtr numIOVs,
									__wasi_filesize_t offset,
									WASIAddressIPtr numBytesReadAddress)
{
	TRACE_SYSCALL_IPTR("fd_pread",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT " , %" PRIu64
					   ", " WASIADDRESSIPTR_FORMAT ")",
					   fd,
					   iovsAddress,
					   numIOVs,
					   offset,
					   numBytesReadAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	Uptr numBytesRead = 0;
	const __wasi_errno_t result
		= readImpl(process, fd, iovsAddress, numIOVs, &offset, numBytesRead);

	// Write the number of bytes read to memory.
	WAVM_ASSERT(numBytesRead <= WASIADDRESSIPTR_MAX);
	memoryRef<WASIAddressIPtr>(process->memory, numBytesReadAddress)
		= WASIAddressIPtr(numBytesRead);

	return TRACE_SYSCALL_RETURN(result, "(numBytesRead=%" WAVM_PRIuPTR ")", numBytesRead);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_pwrite",
									__wasi_errno_return_t,
									wasi_fd_pwrite,
									__wasi_fd_t fd,
									WASIAddressIPtr iovsAddress,
									WASIAddressIPtr numIOVs,
									__wasi_filesize_t offset,
									WASIAddressIPtr numBytesWrittenAddress)
{
	TRACE_SYSCALL_IPTR("fd_pwrite",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ", %" PRIu64
					   ", " WASIADDRESSIPTR_FORMAT ")",
					   fd,
					   iovsAddress,
					   numIOVs,
					   offset,
					   numBytesWrittenAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	Uptr numBytesWritten = 0;
	const __wasi_errno_t result
		= writeImpl(process, fd, iovsAddress, numIOVs, &offset, numBytesWritten);

	// Write the number of bytes written to memory.
	WAVM_ASSERT(numBytesWritten <= WASIADDRESSIPTR_MAX);
	memoryRef<WASIAddressIPtr>(process->memory, numBytesWrittenAddress)
		= WASIAddressIPtr(numBytesWritten);

	return TRACE_SYSCALL_RETURN(result, "(numBytesWritten=%" WAVM_PRIuPTR ")", numBytesWritten);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_read",
									__wasi_errno_return_t,
									wasi_fd_read,
									__wasi_fd_t fd,
									WASIAddressIPtr iovsAddress,
									WASIAddressIPtr numIOVs,
									WASIAddressIPtr numBytesReadAddress)
{
	TRACE_SYSCALL_IPTR("fd_read",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
					   ", " WASIADDRESSIPTR_FORMAT ")",
					   fd,
					   iovsAddress,
					   numIOVs,
					   numBytesReadAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	Uptr numBytesRead = 0;
	const __wasi_errno_t result
		= readImpl(process, fd, iovsAddress, numIOVs, nullptr, numBytesRead);

	// Write the number of bytes read to memory.
	WAVM_ASSERT(numBytesRead <= WASIADDRESSIPTR_MAX);
	memoryRef<WASIAddressIPtr>(process->memory, numBytesReadAddress)
		= WASIAddressIPtr(numBytesRead);

	return TRACE_SYSCALL_RETURN(result, "(numBytesRead=%" WAVM_PRIuPTR ")", numBytesRead);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_write",
									__wasi_errno_return_t,
									wasi_fd_write,
									__wasi_fd_t fd,
									WASIAddressIPtr iovsAddress,
									WASIAddressIPtr numIOVs,
									WASIAddressIPtr numBytesWrittenAddress)
{
	TRACE_SYSCALL_IPTR("fd_write",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
					   ", " WASIADDRESSIPTR_FORMAT ")",
					   fd,
					   iovsAddress,
					   numIOVs,
					   numBytesWrittenAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	Uptr numBytesWritten = 0;
	const __wasi_errno_t result
		= writeImpl(process, fd, iovsAddress, numIOVs, nullptr, numBytesWritten);

	// Write the number of bytes written to memory.
	WAVM_ASSERT(numBytesWritten <= WASIADDRESSIPTR_MAX);
	memoryRef<WASIAddressIPtr>(process->memory, numBytesWrittenAddress)
		= WASIAddressIPtr(numBytesWritten);

	return TRACE_SYSCALL_RETURN(result, "(numBytesWritten=%" WAVM_PRIuPTR ")", numBytesWritten);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_renumber",
									__wasi_errno_return_t,
									wasi_fd_renumber,
									__wasi_fd_t fromFD,
									__wasi_fd_t toFD)
{
	TRACE_SYSCALL_IPTR("fd_renumber", "(%u, %u)", fromFD, toFD);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	// Exclusively lock the fds mutex.
	Platform::RWMutex::ExclusiveLock fdsLock(process->fdMapMutex);

	// Look up the FDE for the source FD.
	if(fromFD < process->fdMap.getMinIndex() || fromFD > process->fdMap.getMaxIndex())
	{
		return TRACE_SYSCALL_RETURN(__WASI_EBADF);
	}
	std::shared_ptr<FDE>* fromFDEPointer = process->fdMap.get(fromFD);
	if(!fromFDEPointer) { return TRACE_SYSCALL_RETURN(__WASI_EBADF); }
	std::shared_ptr<FDE> fromFDE = *fromFDEPointer;

	// Look up the FDE for the destination FD.
	if(toFD < process->fdMap.getMinIndex() || toFD > process->fdMap.getMaxIndex())
	{
		return TRACE_SYSCALL_RETURN(__WASI_EBADF);
	}
	std::shared_ptr<FDE>* toFDEPointer = process->fdMap.get(toFD);
	if(!toFDEPointer) { return TRACE_SYSCALL_RETURN(__WASI_EBADF); }
	std::shared_ptr<FDE> toFDE = *toFDEPointer;

	// Don't allow renumbering preopened files.
	if(fromFDE->isPreopened || toFDE->isPreopened) { return TRACE_SYSCALL_RETURN(__WASI_ENOTSUP); }

	// Exclusively lock the FDE being replaced at the destination FD.
	Platform::RWMutex::ExclusiveLock fromFDELock(fromFDE->mutex);

	// Close the FDE being replaced. This can return an error code, but closes the VFD+DirEntStream
	// even if there was an error.
	Result result = toFDE->close();

	// Move the FDE from fromFD to toFD in the fds map.
	process->fdMap[toFD] = std::move(fromFDE);
	process->fdMap.removeOrFail(fromFD);

	return TRACE_SYSCALL_RETURN(asWASIErrNo(result));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_seek",
									__wasi_errno_return_t,
									wasi_fd_seek,
									__wasi_fd_t fd,
									__wasi_filedelta_t offset,
									U32 whence,
									WASIAddressIPtr newOffsetAddress)
{
	TRACE_SYSCALL_IPTR("fd_seek",
					   "(%u, %" PRIi64 ", %s, " WASIADDRESSIPTR_FORMAT ")",
					   fd,
					   offset,
					   describeSeekWhence(whence).c_str(),
					   newOffsetAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	LockedFDE lockedFDE = getLockedFDE(process, fd, __WASI_RIGHT_FD_SEEK, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	SeekOrigin origin;
	switch(whence)
	{
	case __WASI_WHENCE_CUR: origin = SeekOrigin::cur; break;
	case __WASI_WHENCE_END: origin = SeekOrigin::end; break;
	case __WASI_WHENCE_SET: origin = SeekOrigin::begin; break;
	default: return TRACE_SYSCALL_RETURN(__WASI_EINVAL);
	};

	U64 newOffset;
	const VFS::Result result = lockedFDE.fde->vfd->seek(offset, origin, &newOffset);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }

	memoryRef<__wasi_filesize_t>(process->memory, newOffsetAddress) = newOffset;
	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_tell",
									__wasi_errno_return_t,
									wasi_fd_tell,
									__wasi_fd_t fd,
									WASIAddressIPtr offsetAddress)
{
	TRACE_SYSCALL_IPTR("fd_tell", "(%u, " WASIADDRESSIPTR_FORMAT ")", fd, offsetAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	LockedFDE lockedFDE = getLockedFDE(process, fd, __WASI_RIGHT_FD_TELL, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	U64 currentOffset;
	const VFS::Result result = lockedFDE.fde->vfd->seek(0, SeekOrigin::cur, &currentOffset);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }

	memoryRef<__wasi_filesize_t>(process->memory, offsetAddress) = currentOffset;
	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_fdstat_get",
									__wasi_errno_return_t,
									wasi_fd_fdstat_get,
									__wasi_fd_t fd,
									WASIAddressIPtr fdstatAddress)
{
	TRACE_SYSCALL_IPTR("fd_fdstat_get", "(%u, " WASIADDRESSIPTR_FORMAT ")", fd, fdstatAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	LockedFDE lockedFDE = getLockedFDE(process, fd, 0, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	VFDInfo fdInfo;
	const VFS::Result result = lockedFDE.fde->vfd->getVFDInfo(fdInfo);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }

	__wasi_fdstat_t& fdstat = memoryRef<__wasi_fdstat_t>(process->memory, fdstatAddress);
	fdstat.fs_filetype = asWASIFileType(fdInfo.type);
	fdstat.fs_flags = 0;

	if(fdInfo.flags.append) { fdstat.fs_flags |= __WASI_FDFLAG_APPEND; }
	if(fdInfo.flags.nonBlocking) { fdstat.fs_flags |= __WASI_FDFLAG_NONBLOCK; }
	switch(fdInfo.flags.syncLevel)
	{
	case VFDSync::none: break;
	case VFDSync::contentsAfterWrite: fdstat.fs_flags |= __WASI_FDFLAG_DSYNC; break;
	case VFDSync::contentsAndMetadataAfterWrite: fdstat.fs_flags |= __WASI_FDFLAG_SYNC; break;
	case VFDSync::contentsAfterWriteAndBeforeRead:
		fdstat.fs_flags |= __WASI_FDFLAG_DSYNC | __WASI_FDFLAG_RSYNC;
		break;
	case VFDSync::contentsAndMetadataAfterWriteAndBeforeRead:
		fdstat.fs_flags |= __WASI_FDFLAG_SYNC | __WASI_FDFLAG_RSYNC;
		break;

	default: WAVM_UNREACHABLE();
	}

	fdstat.fs_rights_base = lockedFDE.fde->rights;
	fdstat.fs_rights_inheriting = lockedFDE.fde->inheritingRights;

	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_fdstat_set_flags",
									__wasi_errno_return_t,
									wasi_fd_fdstat_set_flags,
									__wasi_fd_t fd,
									__wasi_fdflags_t flags)
{
	TRACE_SYSCALL_IPTR("fd_fdstat_set_flags", "(%u, 0x%04x)", fd, flags);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	__wasi_rights_t requiredRights = 0;
	VFDFlags vfsVFDFlags = translateWASIVFDFlags(flags, requiredRights);

	LockedFDE lockedFDE
		= getLockedFDE(process, fd, __WASI_RIGHT_FD_FDSTAT_SET_FLAGS | requiredRights, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	const VFS::Result result = lockedFDE.fde->vfd->setVFDFlags(vfsVFDFlags);
	return TRACE_SYSCALL_RETURN(asWASIErrNo(result));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_fdstat_set_rights",
									__wasi_errno_return_t,
									wasi_fd_fdstat_set_rights,
									__wasi_fd_t fd,
									__wasi_rights_t rights,
									__wasi_rights_t inheritingRights)
{
	TRACE_SYSCALL_IPTR("fd_fdstat_set_rights",
					   "(%u, 0x%" PRIx64 ", 0x %" PRIx64 ") ",
					   fd,
					   rights,
					   inheritingRights);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	LockedFDE lockedFDE
		= getLockedFDE(process, fd, rights, inheritingRights, Platform::RWMutex::exclusive);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	// Narrow the FD's rights.
	lockedFDE.fde->rights = rights;
	lockedFDE.fde->inheritingRights = inheritingRights;

	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_sync",
									__wasi_errno_return_t,
									wasi_fd_sync,
									__wasi_fd_t fd)
{
	TRACE_SYSCALL_IPTR("fd_sync", "(%u)", fd);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	LockedFDE lockedFDE = getLockedFDE(process, fd, __WASI_RIGHT_FD_SYNC, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	const VFS::Result result = lockedFDE.fde->vfd->sync(SyncType::contentsAndMetadata);
	return TRACE_SYSCALL_RETURN(asWASIErrNo(result));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_advise",
									__wasi_errno_return_t,
									wasi_fd_advise,
									__wasi_fd_t fd,
									__wasi_filesize_t offset,
									__wasi_filesize_t numBytes,
									__wasi_advice_t advice)
{
	TRACE_SYSCALL_IPTR(
		"fd_advise", "(%u, %" PRIu64 ", %" PRIu64 ", 0x%02x)", fd, offset, numBytes, advice);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	LockedFDE lockedFDE = getLockedFDE(process, fd, __WASI_RIGHT_FD_ADVISE, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	switch(advice)
	{
	case __WASI_ADVICE_DONTNEED:
	case __WASI_ADVICE_NOREUSE:
	case __WASI_ADVICE_NORMAL:
	case __WASI_ADVICE_RANDOM:
	case __WASI_ADVICE_SEQUENTIAL:
	case __WASI_ADVICE_WILLNEED:
		// It's safe to ignore the advice, so just return success for now.
		// TODO: do something with the advice!
		return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS);
	default: return TRACE_SYSCALL_RETURN(__WASI_EINVAL);
	}
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_allocate",
									__wasi_errno_return_t,
									wasi_fd_allocate,
									__wasi_fd_t fd,
									__wasi_filesize_t offset,
									__wasi_filesize_t numBytes)
{
	UNIMPLEMENTED_SYSCALL_IPTR(
		"fd_allocate", "(%u, %" PRIu64 ", %" PRIu64 ")", fd, offset, numBytes);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"path_link",
									__wasi_errno_return_t,
									wasi_path_link,
									__wasi_fd_t dirFD,
									__wasi_lookupflags_t lookupFlags,
									WASIAddressIPtr oldPathAddress,
									WASIAddressIPtr numOldPathBytes,
									__wasi_fd_t newFD,
									WASIAddressIPtr newPathAddress,
									WASIAddressIPtr numNewPathBytes)
{
	UNIMPLEMENTED_SYSCALL_IPTR("path_link",
							   "(%u, 0x%08x, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
							   ", %u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
							   dirFD,
							   lookupFlags,
							   oldPathAddress,
							   numOldPathBytes,
							   newFD,
							   newPathAddress,
							   numNewPathBytes);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"path_open",
									__wasi_errno_return_t,
									wasi_path_open,
									__wasi_fd_t dirFD,
									__wasi_lookupflags_t lookupFlags,
									WASIAddressIPtr pathAddress,
									WASIAddressIPtr numPathBytes,
									__wasi_oflags_t openFlags,
									__wasi_rights_t requestedRights,
									__wasi_rights_t requestedInheritingRights,
									__wasi_fdflags_t fdFlags,
									WASIAddressIPtr fdAddress)
{
	TRACE_SYSCALL_IPTR("path_open",
					   "(%u, 0x%08x, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
					   " , 0x%x , 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%04x, " WASIADDRESSIPTR_FORMAT
					   ")",
					   dirFD,
					   lookupFlags,
					   pathAddress,
					   numPathBytes,
					   openFlags,
					   requestedRights,
					   requestedInheritingRights,
					   fdFlags,
					   fdAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	__wasi_rights_t requiredDirRights = __WASI_RIGHT_PATH_OPEN;
	__wasi_rights_t requiredDirInheritingRights = requestedRights | requestedInheritingRights;

	const bool read = requestedRights & (__WASI_RIGHT_FD_READ | __WASI_RIGHT_FD_READDIR);
	const bool write = requestedRights
					   & (__WASI_RIGHT_FD_DATASYNC | __WASI_RIGHT_FD_WRITE
						  | __WASI_RIGHT_FD_ALLOCATE | __WASI_RIGHT_FD_FILESTAT_SET_SIZE);
	const FileAccessMode accessMode = read && write ? FileAccessMode::readWrite
									  : read        ? FileAccessMode::readOnly
									  : write       ? FileAccessMode::writeOnly
													: FileAccessMode::none;

	FileCreateMode createMode = FileCreateMode::openExisting;
	switch(openFlags & (__WASI_O_CREAT | __WASI_O_EXCL | __WASI_O_TRUNC))
	{
	case __WASI_O_CREAT | __WASI_O_EXCL: createMode = FileCreateMode::createNew; break;
	case __WASI_O_CREAT | __WASI_O_TRUNC: createMode = FileCreateMode::createAlways; break;
	case __WASI_O_CREAT: createMode = FileCreateMode::openAlways; break;
	case __WASI_O_TRUNC: createMode = FileCreateMode::truncateExisting; break;
	case 0:
		createMode = FileCreateMode::openExisting;
		break;

		// Undefined oflag combinations
	case __WASI_O_CREAT | __WASI_O_EXCL | __WASI_O_TRUNC:
	case __WASI_O_EXCL | __WASI_O_TRUNC:
	case __WASI_O_EXCL:
	default: return TRACE_SYSCALL_RETURN(__WASI_EINVAL);
	};

	if(openFlags & __WASI_O_CREAT) { requiredDirRights |= __WASI_RIGHT_PATH_CREATE_FILE; }
	if(openFlags & __WASI_O_TRUNC) { requiredDirRights |= __WASI_RIGHT_PATH_FILESTAT_SET_SIZE; }

	VFDFlags vfsVFDFlags = translateWASIVFDFlags(fdFlags, requiredDirInheritingRights);
	if(write && !(fdFlags & __WASI_FDFLAG_APPEND) && !(openFlags & __WASI_O_TRUNC))
	{
		requiredDirInheritingRights |= __WASI_RIGHT_FD_SEEK;
	}

	std::string canonicalPath;
	const __wasi_errno_t pathError = validatePath(process,
												  dirFD,
												  lookupFlags,
												  requiredDirRights,
												  requiredDirInheritingRights,
												  pathAddress,
												  numPathBytes,
												  canonicalPath);
	if(pathError != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(pathError); }

	VFD* openedVFD = nullptr;
	VFS::Result result
		= process->fileSystem->open(canonicalPath, accessMode, createMode, openedVFD, vfsVFDFlags);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }

	Platform::RWMutex::ExclusiveLock fdsLock(process->fdMapMutex);
	__wasi_fd_t fd = process->fdMap.add(
		UINT32_MAX,
		std::make_shared<FDE>(
			openedVFD, requestedRights, requestedInheritingRights, std::move(canonicalPath)));
	if(fd == UINT32_MAX)
	{
		result = openedVFD->close();
		if(result != VFS::Result::success)
		{
			Log::printf(Log::Category::debug,
						"Error when closing newly opened VFD due to full FD table: %s\n",
						VFS::describeResult(result));
		}
		return TRACE_SYSCALL_RETURN(__WASI_EMFILE);
	}

	memoryRef<__wasi_fd_t>(process->memory, fdAddress) = fd;

	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS, "(%u)", fd);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_readdir",
									__wasi_errno_return_t,
									wasi_fd_readdir,
									__wasi_fd_t dirFD,
									WASIAddressIPtr bufferAddress,
									WASIAddressIPtr numBufferBytes,
									__wasi_dircookie_t firstCookie,
									WASIAddressIPtr outNumBufferBytesUsedAddress)
{
	TRACE_SYSCALL_IPTR("fd_readdir",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ", 0x%" PRIx64
					   ", " WASIADDRESSIPTR_FORMAT ")",
					   dirFD,
					   bufferAddress,
					   numBufferBytes,
					   firstCookie,
					   outNumBufferBytesUsedAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	LockedFDE lockedFDE
		= getLockedFDE(process, dirFD, __WASI_RIGHT_FD_READDIR, 0, Platform::RWMutex::exclusive);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	// If this is the first time readdir was called, open a DirEntStream for the FD.
	if(!lockedFDE.fde->dirEntStream)
	{
		if(firstCookie != __WASI_DIRCOOKIE_START) { return TRACE_SYSCALL_RETURN(__WASI_EINVAL); }

		const VFS::Result result = lockedFDE.fde->vfd->openDir(lockedFDE.fde->dirEntStream);
		if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }
	}
	else if(lockedFDE.fde->dirEntStream->tell() != firstCookie)
	{
		if(!lockedFDE.fde->dirEntStream->seek(firstCookie))
		{
			return TRACE_SYSCALL_RETURN(__WASI_EINVAL);
		}
	}

	U8* buffer = memoryArrayPtr<U8>(process->memory, bufferAddress, numBufferBytes);
	Uptr numBufferBytesUsed = 0;

	while(numBufferBytesUsed < numBufferBytes)
	{
		DirEnt dirEnt;
		if(!lockedFDE.fde->dirEntStream->getNext(dirEnt)) { break; }

		WAVM_ERROR_UNLESS(dirEnt.name.size() <= WASIADDRESSIPTR_MAX);

		__wasi_dirent_t wasiDirEnt;
		wasiDirEnt.d_next = lockedFDE.fde->dirEntStream->tell();
		wasiDirEnt.d_ino = dirEnt.fileNumber;
		wasiDirEnt.d_namlen = static_cast<WASIAddressIPtr>(dirEnt.name.size());
		wasiDirEnt.d_type = asWASIFileType(dirEnt.type);

		numBufferBytesUsed += truncatingMemcpy(buffer + numBufferBytesUsed,
											   &wasiDirEnt,
											   sizeof(wasiDirEnt),
											   numBufferBytes - numBufferBytesUsed);

		numBufferBytesUsed += truncatingMemcpy(buffer + numBufferBytesUsed,
											   dirEnt.name.c_str(),
											   dirEnt.name.size(),
											   numBufferBytes - numBufferBytesUsed);
	};

	WAVM_ASSERT(numBufferBytesUsed <= numBufferBytes);
	memoryRef<WASIAddressIPtr>(process->memory, outNumBufferBytesUsedAddress)
		= WASIAddressIPtr(numBufferBytesUsed);

	return TRACE_SYSCALL_RETURN(
		__WASI_ESUCCESS, "(numBufferBytesUsed=%" WAVM_PRIuPTR ")", numBufferBytesUsed);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"path_readlink",
									__wasi_errno_return_t,
									wasi_path_readlink,
									__wasi_fd_t fd,
									WASIAddressIPtr pathAddress,
									WASIAddressIPtr numPathBytes,
									WASIAddressIPtr bufferAddress,
									WASIAddressIPtr numBufferBytes,
									WASIAddressIPtr outNumBufferBytesUsedAddress)
{
	UNIMPLEMENTED_SYSCALL_IPTR("path_readlink",
							   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
							   ", " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
							   ", " WASIADDRESSIPTR_FORMAT ")",
							   fd,
							   pathAddress,
							   numPathBytes,
							   bufferAddress,
							   numBufferBytes,
							   outNumBufferBytesUsedAddress);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"path_rename",
									__wasi_errno_return_t,
									wasi_path_rename,
									__wasi_fd_t oldFD,
									WASIAddressIPtr oldPathAddress,
									WASIAddressIPtr numOldPathBytes,
									__wasi_fd_t newFD,
									WASIAddressIPtr newPathAddress,
									WASIAddressIPtr numNewPathBytes)
{
	TRACE_SYSCALL_IPTR("path_rename",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
					   ", %u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   oldFD,
					   oldPathAddress,
					   numOldPathBytes,
					   newFD,
					   newPathAddress,
					   numNewPathBytes);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	std::string canonicalOldPath;
	const __wasi_errno_t oldPathError = validatePath(process,
													 oldFD,
													 0,
													 __WASI_RIGHT_PATH_RENAME_SOURCE,
													 0,
													 oldPathAddress,
													 numOldPathBytes,
													 canonicalOldPath);
	if(oldPathError != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(oldPathError); }

	std::string canonicalNewPath;
	const __wasi_errno_t newPathError = validatePath(process,
													 newFD,
													 0,
													 __WASI_RIGHT_PATH_RENAME_TARGET,
													 0,
													 newPathAddress,
													 numNewPathBytes,
													 canonicalNewPath);
	if(newPathError != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(newPathError); }

	return TRACE_SYSCALL_RETURN(
		asWASIErrNo(process->fileSystem->renameFile(canonicalOldPath, canonicalNewPath)));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_filestat_get",
									__wasi_errno_return_t,
									wasi_fd_filestat_get,
									__wasi_fd_t fd,
									WASIAddressIPtr filestatAddress)
{
	TRACE_SYSCALL_IPTR("fd_filestat_get", "(%u, " WASIADDRESSIPTR_FORMAT ")", fd, filestatAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	LockedFDE lockedFDE = getLockedFDE(process, fd, __WASI_RIGHT_FD_FILESTAT_GET, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	FileInfo fileInfo;
	const VFS::Result result = lockedFDE.fde->vfd->getFileInfo(fileInfo);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }

	__wasi_filestat_t& fileStat = memoryRef<__wasi_filestat_t>(process->memory, filestatAddress);

	fileStat.st_dev = fileInfo.deviceNumber;
	fileStat.st_ino = fileInfo.fileNumber;
	fileStat.st_filetype = asWASIFileType(fileInfo.type);
	fileStat.st_nlink = fileInfo.numLinks;
	fileStat.st_size = fileInfo.numBytes;
	fileStat.st_atim = __wasi_timestamp_t(fileInfo.lastAccessTime.ns);
	fileStat.st_mtim = __wasi_timestamp_t(fileInfo.lastWriteTime.ns);
	fileStat.st_ctim = __wasi_timestamp_t(fileInfo.creationTime.ns);

	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS,
								"(st_dev=%" PRIu64 ", st_ino=%" PRIu64
								", st_filetype=%u"
								", st_nlink=%" PRIu64 ", st_size=%" PRIu64 ", st_atim=%" PRIu64
								", st_mtim=%" PRIu64 ", st_ctim=%" PRIu64 ")",
								fileStat.st_dev,
								fileStat.st_ino,
								fileStat.st_filetype,
								fileStat.st_nlink,
								fileStat.st_size,
								fileStat.st_atim,
								fileStat.st_mtim,
								fileStat.st_ctim);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_filestat_set_times",
									__wasi_errno_return_t,
									wasi_fd_filestat_set_times,
									__wasi_fd_t fd,
									__wasi_timestamp_t lastAccessTime64,
									__wasi_timestamp_t lastWriteTime64,
									__wasi_fstflags_t flags)
{
	TRACE_SYSCALL_IPTR("fd_filestat_set_times",
					   "(%u, %" PRIu64 ", %" PRIu64 ", 0x%04x)",
					   fd,
					   lastAccessTime64,
					   lastWriteTime64,
					   flags);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	LockedFDE lockedFDE = getLockedFDE(process, fd, __WASI_RIGHT_FD_FILESTAT_SET_TIMES, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	Time now = Platform::getClockTime(Platform::Clock::realtime);

	bool setLastAccessTime = false;
	Time lastAccessTime;
	if(flags & __WASI_FILESTAT_SET_ATIM)
	{
		lastAccessTime.ns = lastAccessTime64;
		setLastAccessTime = true;
	}
	else if(flags & __WASI_FILESTAT_SET_ATIM_NOW)
	{
		lastAccessTime = now;
		setLastAccessTime = true;
	}

	bool setLastWriteTime = false;
	Time lastWriteTime;
	if(flags & __WASI_FILESTAT_SET_MTIM)
	{
		lastWriteTime.ns = lastWriteTime64;
		setLastWriteTime = true;
	}
	else if(flags & __WASI_FILESTAT_SET_MTIM_NOW)
	{
		lastWriteTime = now;
		setLastWriteTime = true;
	}

	const VFS::Result result = lockedFDE.fde->vfd->setFileTimes(
		setLastAccessTime, lastAccessTime, setLastWriteTime, lastWriteTime);

	return TRACE_SYSCALL_RETURN(asWASIErrNo(result));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"fd_filestat_set_size",
									__wasi_errno_return_t,
									wasi_fd_filestat_set_size,
									__wasi_fd_t fd,
									__wasi_filesize_t numBytes)
{
	TRACE_SYSCALL_IPTR("fd_filestat_set_size", "(%u, %" PRIu64 ")", fd, numBytes);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	LockedFDE lockedFDE = getLockedFDE(process, fd, __WASI_RIGHT_FD_FILESTAT_SET_SIZE, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	return TRACE_SYSCALL_RETURN(asWASIErrNo(lockedFDE.fde->vfd->setFileSize(numBytes)));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"path_filestat_get",
									__wasi_errno_return_t,
									wasi_path_filestat_get,
									__wasi_fd_t dirFD,
									__wasi_lookupflags_t lookupFlags,
									WASIAddressIPtr pathAddress,
									WASIAddressIPtr numPathBytes,
									WASIAddressIPtr filestatAddress)
{
	TRACE_SYSCALL_IPTR("path_filestat_get",
					   "(%u, 0x%08x, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
					   ", " WASIADDRESSIPTR_FORMAT ")",
					   dirFD,
					   lookupFlags,
					   pathAddress,
					   numPathBytes,
					   filestatAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	std::string canonicalPath;
	const __wasi_errno_t pathError = validatePath(process,
												  dirFD,
												  lookupFlags,
												  __WASI_RIGHT_PATH_FILESTAT_GET,
												  0,
												  pathAddress,
												  numPathBytes,
												  canonicalPath);
	if(pathError != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(pathError); }

	FileInfo fileInfo;
	const VFS::Result result = process->fileSystem->getFileInfo(canonicalPath, fileInfo);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }

	__wasi_filestat_t& fileStat = memoryRef<__wasi_filestat_t>(process->memory, filestatAddress);

	fileStat.st_dev = fileInfo.deviceNumber;
	fileStat.st_ino = fileInfo.fileNumber;
	fileStat.st_filetype = asWASIFileType(fileInfo.type);
	fileStat.st_nlink = fileInfo.numLinks;
	fileStat.st_size = fileInfo.numBytes;
	fileStat.st_atim = __wasi_timestamp_t(fileInfo.lastAccessTime.ns);
	fileStat.st_mtim = __wasi_timestamp_t(fileInfo.lastWriteTime.ns);
	fileStat.st_ctim = __wasi_timestamp_t(fileInfo.creationTime.ns);

	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS,
								"(st_dev=%" PRIu64 ", st_ino=%" PRIu64
								", st_filetype=%u"
								", st_nlink=%" PRIu64 ", st_size=%" PRIu64 ", st_atim=%" PRIu64
								", st_mtim=%" PRIu64 ", st_ctim=%" PRIu64 ")",
								fileStat.st_dev,
								fileStat.st_ino,
								fileStat.st_filetype,
								fileStat.st_nlink,
								fileStat.st_size,
								fileStat.st_atim,
								fileStat.st_mtim,
								fileStat.st_ctim);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"path_filestat_set_times",
									__wasi_errno_return_t,
									wasi_path_filestat_set_times,
									__wasi_fd_t dirFD,
									__wasi_lookupflags_t lookupFlags,
									WASIAddressIPtr pathAddress,
									WASIAddressIPtr numPathBytes,
									__wasi_timestamp_t lastAccessTime64,
									__wasi_timestamp_t lastWriteTime64,
									__wasi_fstflags_t flags)
{
	TRACE_SYSCALL_IPTR("path_filestat_set_times",
					   "(%u, 0x%08x, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
					   ", %" PRIu64 ", %" PRIu64 ", 0x%04x)",
					   dirFD,
					   lookupFlags,
					   pathAddress,
					   numPathBytes,
					   lastAccessTime64,
					   lastWriteTime64,
					   flags);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	std::string canonicalPath;
	const __wasi_errno_t pathError = validatePath(process,
												  dirFD,
												  lookupFlags,
												  __WASI_RIGHT_PATH_FILESTAT_SET_TIMES,
												  0,
												  pathAddress,
												  numPathBytes,
												  canonicalPath);
	if(pathError != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(pathError); }

	Time now = Platform::getClockTime(Platform::Clock::realtime);

	bool setLastAccessTime = false;
	Time lastAccessTime;
	if(flags & __WASI_FILESTAT_SET_ATIM)
	{
		lastAccessTime.ns = lastAccessTime64;
		setLastAccessTime = true;
	}
	else if(flags & __WASI_FILESTAT_SET_ATIM_NOW)
	{
		lastAccessTime = now;
		setLastAccessTime = true;
	}

	bool setLastWriteTime = false;
	Time lastWriteTime;
	if(flags & __WASI_FILESTAT_SET_MTIM)
	{
		lastWriteTime.ns = lastWriteTime64;
		setLastWriteTime = true;
	}
	else if(flags & __WASI_FILESTAT_SET_MTIM_NOW)
	{
		lastWriteTime = now;
		setLastWriteTime = true;
	}

	const VFS::Result result = process->fileSystem->setFileTimes(
		canonicalPath, setLastAccessTime, lastAccessTime, setLastWriteTime, lastWriteTime);
	return TRACE_SYSCALL_RETURN(asWASIErrNo(result));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"path_symlink",
									__wasi_errno_return_t,
									wasi_path_symlink,
									WASIAddressIPtr oldPathAddress,
									WASIAddressIPtr numOldPathBytes,
									__wasi_fd_t fd,
									WASIAddressIPtr newPathAddress,
									WASIAddressIPtr numNewPathBytes)
{
	UNIMPLEMENTED_SYSCALL_IPTR("path_symlink",
							   "(" WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
							   ", %u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
							   oldPathAddress,
							   numOldPathBytes,
							   fd,
							   newPathAddress,
							   numNewPathBytes);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"path_unlink_file",
									__wasi_errno_return_t,
									wasi_path_unlink_file,
									__wasi_fd_t dirFD,
									WASIAddressIPtr pathAddress,
									WASIAddressIPtr numPathBytes)
{
	TRACE_SYSCALL_IPTR("path_unlink_file",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   dirFD,
					   pathAddress,
					   numPathBytes);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	std::string canonicalPath;
	const __wasi_errno_t pathError = validatePath(process,
												  dirFD,
												  0,
												  __WASI_RIGHT_PATH_UNLINK_FILE,
												  0,
												  pathAddress,
												  numPathBytes,
												  canonicalPath);
	if(pathError != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(pathError); }

	if(!process->fileSystem) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	Result result = process->fileSystem->unlinkFile(canonicalPath);
	return TRACE_SYSCALL_RETURN(result == VFS::Result::isDirectory ? __WASI_EPERM
																   : asWASIErrNo(result));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"path_remove_directory",
									__wasi_errno_return_t,
									wasi_path_remove_directory,
									__wasi_fd_t dirFD,
									WASIAddressIPtr pathAddress,
									WASIAddressIPtr numPathBytes)
{
	TRACE_SYSCALL_IPTR("path_remove_directory",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   dirFD,
					   pathAddress,
					   numPathBytes);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	std::string canonicalPath;
	const __wasi_errno_t pathError = validatePath(process,
												  dirFD,
												  0,
												  __WASI_RIGHT_PATH_REMOVE_DIRECTORY,
												  0,
												  pathAddress,
												  numPathBytes,
												  canonicalPath);
	if(pathError != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(pathError); }

	if(!process->fileSystem) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	const VFS::Result result = process->fileSystem->removeDir(canonicalPath);
	return TRACE_SYSCALL_RETURN(asWASIErrNo(result));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"path_create_directory",
									__wasi_errno_return_t,
									wasi_path_create_directory,
									__wasi_fd_t dirFD,
									WASIAddressIPtr pathAddress,
									WASIAddressIPtr numPathBytes)
{
	TRACE_SYSCALL_IPTR("path_create_directory",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   dirFD,
					   pathAddress,
					   numPathBytes);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	std::string canonicalPath;
	const __wasi_errno_t pathError = validatePath(process,
												  dirFD,
												  0,
												  __WASI_RIGHT_PATH_CREATE_DIRECTORY,
												  0,
												  pathAddress,
												  numPathBytes,
												  canonicalPath);
	if(pathError != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(pathError); }

	const VFS::Result result = process->fileSystem->createDir(canonicalPath);
	return TRACE_SYSCALL_RETURN(asWASIErrNo(result));
}

// Socket operations. These implement both the WASI preview1 socket functions
// (sock_accept/sock_recv/sock_send/sock_shutdown) and the WASIX socket extension ABI
// (sock_status/sock_addr_local/sock_addr_peer/sock_open/sock_pair/sock_bind/
// sock_listen/sock_accept_v2/sock_connect/sock_recv_from/sock_send_to/sock_send_file/
// sock_set/get_opt_flag/time/size, the multicast join/leave calls, and resolve). All of
// them are gated on the process's networkEnabled permission.

// Marshals a WASI iovec array into a vector of IOReadBuffers. Returns EFAULT if the iovec
// array or the buffers it references are out of bounds.
static __wasi_errno_t marshalReadIOVs(Process* process,
									WASIAddressIPtr iovsAddress,
									WASIAddressIPtr numIOVs,
									std::vector<IOReadBuffer>& outBuffers)
{
	if(numIOVs > __WASI_IOV_MAX) { return __WASI_EINVAL; }

	__wasi_errno_t result = __WASI_ESUCCESS;
	Runtime::catchRuntimeExceptions(
		[&] {
			const wasi_iovec_iptr* iovs
				= memoryArrayPtr<wasi_iovec_iptr>(process->memory, iovsAddress, numIOVs);
			outBuffers.resize(numIOVs);
			U64 numBufferBytes = 0;
			for(WASIAddressIPtr iovIndex = 0; iovIndex < numIOVs; ++iovIndex)
			{
				outBuffers[iovIndex].data
					= memoryArrayPtr<U8>(process->memory, iovs[iovIndex].buf, iovs[iovIndex].buf_len);
				outBuffers[iovIndex].numBytes = iovs[iovIndex].buf_len;
				numBufferBytes += iovs[iovIndex].buf_len;
			}
			if(numBufferBytes > WASIADDRESSIPTR_MAX) { result = __WASI_EOVERFLOW; }
		},
		[&](Exception* exception) {
			WAVM_ERROR_UNLESS(getExceptionType(exception)
							  == ExceptionTypes::outOfBoundsMemoryAccess);
			destroyException(exception);
			result = __WASI_EFAULT;
		});

	return result;
}

// Marshals a WASI ciovec array into a vector of IOWriteBuffers.
static __wasi_errno_t marshalWriteIOVs(Process* process,
									 WASIAddressIPtr iovsAddress,
									 WASIAddressIPtr numIOVs,
									 std::vector<IOWriteBuffer>& outBuffers)
{
	if(numIOVs > __WASI_IOV_MAX) { return __WASI_EINVAL; }

	__wasi_errno_t result = __WASI_ESUCCESS;
	Runtime::catchRuntimeExceptions(
		[&] {
			const wasi_ciovec_iptr* iovs
				= memoryArrayPtr<wasi_ciovec_iptr>(process->memory, iovsAddress, numIOVs);
			outBuffers.resize(numIOVs);
			U64 numBufferBytes = 0;
			for(WASIAddressIPtr iovIndex = 0; iovIndex < numIOVs; ++iovIndex)
			{
				outBuffers[iovIndex].data = memoryArrayPtr<const U8>(
					process->memory, iovs[iovIndex].buf, iovs[iovIndex].buf_len);
				outBuffers[iovIndex].numBytes = iovs[iovIndex].buf_len;
				numBufferBytes += iovs[iovIndex].buf_len;
			}
			if(numBufferBytes > WASIADDRESSIPTR_MAX) { result = __WASI_EOVERFLOW; }
		},
		[&](Exception* exception) {
			WAVM_ERROR_UNLESS(getExceptionType(exception)
							  == ExceptionTypes::outOfBoundsMemoryAccess);
			destroyException(exception);
			result = __WASI_EFAULT;
		});

	return result;
}

// Reads a WASIX __wasi_addr_port_t (110 bytes) from the module's memory. Ports are stored
// in host byte order; IP addresses in network order.
static __wasi_errno_t readAddrPort(Process* process,
								 WASIAddressIPtr address,
								 VFS::SocketAddress& outAddress)
{
	__wasi_errno_t result = __WASI_ESUCCESS;
	Runtime::catchRuntimeExceptions(
		[&] {
			const U8* bytes = memoryArrayPtr<U8>(process->memory, address, 110);
			const U8 tag = bytes[0];
			outAddress.port = U16(bytes[2]) | (U16(bytes[3]) << 8);
			switch(tag)
			{
			case __WASI_ADDRESS_FAMILY_INET4:
				outAddress.family = VFS::SocketAddress::Family::ipv4;
				outAddress.scopeId = 0;
				memset(outAddress.ipBytes, 0, sizeof(outAddress.ipBytes));
				memcpy(outAddress.ipBytes, bytes + 4, 4);
				break;
			case __WASI_ADDRESS_FAMILY_INET6:
				outAddress.family = VFS::SocketAddress::Family::ipv6;
				memcpy(outAddress.ipBytes, bytes + 4, 16);
				// flowinfo at bytes 20-23 is ignored; scope_id is split into two u16s.
				outAddress.scopeId = (U32(U16(bytes[24]) | (U16(bytes[25]) << 8)) << 16)
									 | U32(U16(bytes[26]) | (U16(bytes[27]) << 8));
				break;
			default: result = __WASI_EAFNOSUPPORT; return;
			};
		},
		[&](Exception* exception) {
			WAVM_ERROR_UNLESS(getExceptionType(exception)
							  == ExceptionTypes::outOfBoundsMemoryAccess);
			destroyException(exception);
			result = __WASI_EFAULT;
		});

	return result;
}

// Writes a SocketAddress to the module's memory as a WASIX __wasi_addr_port_t (110 bytes).
static __wasi_errno_t writeAddrPort(Process* process,
									const VFS::SocketAddress& address,
									WASIAddressIPtr addressPtr)
{
	U8 bytes[110];
	memset(bytes, 0, sizeof(bytes));
	if(address.family == VFS::SocketAddress::Family::ipv4)
	{
		bytes[0] = __WASI_ADDRESS_FAMILY_INET4;
		bytes[2] = U8(address.port);
		bytes[3] = U8(address.port >> 8);
		memcpy(bytes + 4, address.ipBytes, 4);
	}
	else
	{
		bytes[0] = __WASI_ADDRESS_FAMILY_INET6;
		bytes[2] = U8(address.port);
		bytes[3] = U8(address.port >> 8);
		memcpy(bytes + 4, address.ipBytes, 16);
		// flowinfo is always 0; scope_id is split into two little-endian u16s.
		bytes[24] = U8(address.scopeId >> 16);
		bytes[25] = U8(address.scopeId >> 24);
		bytes[26] = U8(address.scopeId);
		bytes[27] = U8(address.scopeId >> 8);
	}

	__wasi_errno_t result = __WASI_ESUCCESS;
	Runtime::catchRuntimeExceptions(
		[&] {
			U8* dest = memoryArrayPtr<U8>(process->memory, addressPtr, sizeof(bytes));
			memcpy(dest, bytes, sizeof(bytes));
		},
		[&](Exception* exception) {
			WAVM_ERROR_UNLESS(getExceptionType(exception)
							  == ExceptionTypes::outOfBoundsMemoryAccess);
			destroyException(exception);
			result = __WASI_EFAULT;
		});

	return result;
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_accept",
									__wasi_errno_return_t,
									wasi_sock_accept,
									__wasi_fd_t sock,
									__wasi_fdflags_t fdFlags,
									WASIAddressIPtr outFDAddress)
{
	TRACE_SYSCALL_IPTR("sock_accept",
					   "(%u, 0x%04x, " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   fdFlags,
					   outFDAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	if(fdFlags & ~__WASI_FDFLAG_NONBLOCK) { return TRACE_SYSCALL_RETURN(__WASI_EINVAL); }

	LockedFDE lockedFDE = getLockedFDE(process, sock, __WASI_RIGHT_SOCK_ACCEPT, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(result); }
	}

	VFDFlags acceptedFlags;
	acceptedFlags.nonBlocking = (fdFlags & __WASI_FDFLAG_NONBLOCK) != 0;

	VFD* acceptedVFD = nullptr;
	const VFS::Result result = lockedFDE.fde->vfd->sockAccept(acceptedVFD, acceptedFlags);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }
	WAVM_ASSERT(acceptedVFD);

	const __wasi_fd_t newFD = addVFD(process, acceptedVFD, SOCKET_RIGHTS, "socket");
	if(newFD == UINT32_MAX) { return TRACE_SYSCALL_RETURN(__WASI_EMFILE); }

	memoryRef<__wasi_fd_t>(process->memory, outFDAddress) = newFD;
	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS, "(%u)", newFD);
}

static __wasi_errno_t sockRecvImpl(Process* process,
								 __wasi_fd_t sock,
								 WASIAddressIPtr riDataAddress,
								 WASIAddressIPtr numRIData,
								 __wasi_riflags_t riFlags,
								 WASIAddressIPtr outDataLenAddress,
								 WASIAddressIPtr outFlagsAddress,
								 WASIAddressIPtr outAddrPortAddress)
{
	if(riFlags & ~(__WASI_SOCK_RECV_PEEK | __WASI_SOCK_RECV_WAITALL
				   | __WASI_RIFLAGS_RECV_DONT_WAIT))
	{
		return __WASI_EINVAL;
	}

	LockedFDE lockedFDE = getLockedFDE(process, sock, __WASI_RIGHT_FD_READ, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return lockedFDE.error; }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return result; }
	}

	std::vector<IOReadBuffer> buffers;
	__wasi_errno_t result
		= marshalReadIOVs(process, riDataAddress, numRIData, buffers);
	if(result != __WASI_ESUCCESS) { return result; }

	Uptr numBytesRead = 0;
	bool dataTruncated = false;
	VFS::SocketAddress sourceAddr;
	memset(&sourceAddr, 0, sizeof(sourceAddr));
	const VFS::Result recvResult = lockedFDE.fde->vfd->sockRecv(
		buffers.data(),
		buffers.size(),
		(riFlags & __WASI_SOCK_RECV_PEEK) != 0,
		(riFlags & __WASI_SOCK_RECV_WAITALL) != 0,
		(riFlags & __WASI_RIFLAGS_RECV_DONT_WAIT) != 0,
		&numBytesRead,
		&dataTruncated,
		outAddrPortAddress ? &sourceAddr : nullptr);
	if(recvResult != VFS::Result::success) { return asWASIErrNo(recvResult); }

	if(outAddrPortAddress)
	{
		result = writeAddrPort(process, sourceAddr, outAddrPortAddress);
		if(result != __WASI_ESUCCESS) { return result; }
	}

	memoryRef<WASIAddressIPtr>(process->memory, outDataLenAddress)
		= WASIAddressIPtr(numBytesRead);
	memoryRef<__wasi_roflags_t>(process->memory, outFlagsAddress)
		= dataTruncated ? __WASI_SOCK_RECV_DATA_TRUNCATED : __wasi_roflags_t(0);
	return __WASI_ESUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_recv",
									__wasi_errno_return_t,
									wasi_sock_recv,
									__wasi_fd_t sock,
									WASIAddressIPtr riDataAddress,
									WASIAddressIPtr numRIData,
									__wasi_riflags_t riFlags,
									WASIAddressIPtr outDataLenAddress,
									WASIAddressIPtr outFlagsAddress)
{
	TRACE_SYSCALL_IPTR("sock_recv",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
					   ", 0x%04x, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   riDataAddress,
					   numRIData,
					   riFlags,
					   outDataLenAddress,
					   outFlagsAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	return TRACE_SYSCALL_RETURN(sockRecvImpl(process,
										   sock,
										   riDataAddress,
										   numRIData,
										   riFlags,
										   outDataLenAddress,
										   outFlagsAddress,
										   0));
}

static __wasi_errno_t sockSendImpl(Process* process,
								 __wasi_fd_t sock,
								 WASIAddressIPtr siDataAddress,
								 WASIAddressIPtr numSIData,
								 __wasi_siflags_t siFlags,
								 WASIAddressIPtr destAddrPortAddress,
								 WASIAddressIPtr outDataLenAddress)
{
	if(siFlags & ~__WASI_SIFLAGS_SEND_DONT_WAIT) { return __WASI_EINVAL; }

	LockedFDE lockedFDE = getLockedFDE(process, sock, __WASI_RIGHT_FD_WRITE, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return lockedFDE.error; }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return result; }
	}

	std::vector<IOWriteBuffer> buffers;
	__wasi_errno_t result = marshalWriteIOVs(process, siDataAddress, numSIData, buffers);
	if(result != __WASI_ESUCCESS) { return result; }

	VFS::SocketAddress destAddr;
	const VFS::SocketAddress* destAddrPtr = nullptr;
	if(destAddrPortAddress)
	{
		result = readAddrPort(process, destAddrPortAddress, destAddr);
		if(result != __WASI_ESUCCESS) { return result; }
		destAddrPtr = &destAddr;
	}

	Uptr numBytesWritten = 0;
	const VFS::Result sendResult = lockedFDE.fde->vfd->sockSend(
		buffers.data(),
		buffers.size(),
		destAddrPtr,
		(siFlags & __WASI_SIFLAGS_SEND_DONT_WAIT) != 0,
		&numBytesWritten);
	if(sendResult != VFS::Result::success) { return asWASIErrNo(sendResult); }

	memoryRef<WASIAddressIPtr>(process->memory, outDataLenAddress)
		= WASIAddressIPtr(numBytesWritten);
	return __WASI_ESUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_send",
									__wasi_errno_return_t,
									wasi_sock_send,
									__wasi_fd_t sock,
									WASIAddressIPtr siDataAddress,
									WASIAddressIPtr numSIData,
									__wasi_siflags_t siFlags,
									WASIAddressIPtr outDataLenAddress)
{
	TRACE_SYSCALL_IPTR("sock_send",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
					   ", 0x%04x, " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   siDataAddress,
					   numSIData,
					   siFlags,
					   outDataLenAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	return TRACE_SYSCALL_RETURN(sockSendImpl(
		process, sock, siDataAddress, numSIData, siFlags, 0, outDataLenAddress));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_shutdown",
									__wasi_errno_return_t,
									wasi_sock_shutdown,
									__wasi_fd_t sock,
									__wasi_sdflags_t how)
{
	TRACE_SYSCALL_IPTR("sock_shutdown", "(%u, 0x%02x)", sock, how);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	if(how == 0 || (how & ~(__WASI_SHUT_RD | __WASI_SHUT_WR)))
	{
		return TRACE_SYSCALL_RETURN(__WASI_EINVAL);
	}

	LockedFDE lockedFDE = getLockedFDE(process, sock, __WASI_RIGHT_SOCK_SHUTDOWN, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(result); }
	}

	const VFS::Result result = lockedFDE.fde->vfd->sockShutdown(
		(how & __WASI_SHUT_RD) != 0, (how & __WASI_SHUT_WR) != 0);
	return TRACE_SYSCALL_RETURN(asWASIErrNo(result));
}

// WASIX socket extension intrinsics. Signatures and memory layouts match wasix-libc's
// <wasi/api_wasix.h>: addresses are __wasi_addr_port_t (110 bytes), resolve results are
// __wasi_addr_ip_t records (18 bytes), and option values are passed via the
// flag/time/size syscall triple.

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_status",
									__wasi_errno_return_t,
									wasi_sock_status,
									__wasi_fd_t sock,
									WASIAddressIPtr outStatusAddress)
{
	TRACE_SYSCALL_IPTR("sock_status", "(%u, " WASIADDRESSIPTR_FORMAT ")", sock, outStatusAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	LockedFDE lockedFDE = getLockedFDE(process, sock, 0, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(result); }
	}

	VFS::SocketStatus status = VFS::SocketStatus::failed;
	const VFS::Result result = lockedFDE.fde->vfd->sockGetStatus(status);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }

	memoryRef<U8>(process->memory, outStatusAddress) = U8(status);
	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS, "(status=%u)", U32(status));
}

static __wasi_errno_t sockGetAddressImpl(Process* process,
									   __wasi_fd_t sock,
									   WASIAddressIPtr outAddrPortAddress,
									   bool peer)
{
	LockedFDE lockedFDE = getLockedFDE(process, sock, 0, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return lockedFDE.error; }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return result; }
	}

	VFS::SocketAddress address;
	memset(&address, 0, sizeof(address));
	const VFS::Result result = peer ? lockedFDE.fde->vfd->sockGetPeerAddress(address)
									: lockedFDE.fde->vfd->sockGetLocalAddress(address);
	if(result != VFS::Result::success) { return asWASIErrNo(result); }

	return writeAddrPort(process, address, outAddrPortAddress);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_addr_local",
									__wasi_errno_return_t,
									wasi_sock_addr_local,
									__wasi_fd_t sock,
									WASIAddressIPtr outAddrPortAddress)
{
	TRACE_SYSCALL_IPTR("sock_addr_local",
					   "(%u, " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   outAddrPortAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	return TRACE_SYSCALL_RETURN(
		sockGetAddressImpl(process, sock, outAddrPortAddress, false));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_addr_peer",
									__wasi_errno_return_t,
									wasi_sock_addr_peer,
									__wasi_fd_t sock,
									WASIAddressIPtr outAddrPortAddress)
{
	TRACE_SYSCALL_IPTR("sock_addr_peer",
					   "(%u, " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   outAddrPortAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	return TRACE_SYSCALL_RETURN(sockGetAddressImpl(process, sock, outAddrPortAddress, true));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_open",
									__wasi_errno_return_t,
									wasi_sock_open,
									U8 addressFamily,
									U8 socketType,
									U16 sockProto,
									WASIAddressIPtr outFDAddress)
{
	TRACE_SYSCALL_IPTR("sock_open",
					   "(%u, %u, %u, " WASIADDRESSIPTR_FORMAT ")",
					   addressFamily,
					   socketType,
					   sockProto,
					   outFDAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	VFS::SocketAddress::Family family;
	Platform::SocketType type;
	const __wasi_errno_t typeResult
		= wasixSocketType(addressFamily, socketType, sockProto, family, type);
	if(typeResult != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(typeResult); }

	VFD* socketVFD = nullptr;
	const VFS::Result result = Platform::createSocket(family, type, socketVFD);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }
	WAVM_ASSERT(socketVFD);

	const __wasi_fd_t newFD = addVFD(process, socketVFD, SOCKET_LISTEN_RIGHTS, "socket");
	if(newFD == UINT32_MAX) { return TRACE_SYSCALL_RETURN(__WASI_EMFILE); }

	memoryRef<__wasi_fd_t>(process->memory, outFDAddress) = newFD;
	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS, "(%u)", newFD);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_pair",
									__wasi_errno_return_t,
									wasi_sock_pair,
									U8 addressFamily,
									U8 socketType,
									U16 sockProto,
									WASIAddressIPtr outFD0Address,
									WASIAddressIPtr outFD1Address)
{
	TRACE_SYSCALL_IPTR("sock_pair",
					   "(%u, %u, %u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   addressFamily,
					   socketType,
					   sockProto,
					   outFD0Address,
					   outFD1Address);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	VFS::SocketAddress::Family family;
	Platform::SocketType type;
	const __wasi_errno_t typeResult
		= wasixSocketType(addressFamily, socketType, sockProto, family, type);
	if(typeResult != __WASI_ESUCCESS && typeResult != __WASI_EAFNOSUPPORT)
	{
		return TRACE_SYSCALL_RETURN(typeResult);
	}
	if(typeResult == __WASI_EAFNOSUPPORT && addressFamily != __WASI_ADDRESS_FAMILY_UNIX)
	{
		return TRACE_SYSCALL_RETURN(typeResult);
	}

	VFD* vfd0 = nullptr;
	VFD* vfd1 = nullptr;
	const VFS::Result result = Platform::createSocketPair(type, vfd0, vfd1);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }

	const __wasi_fd_t newFD0 = addVFD(process, vfd0, SOCKET_RIGHTS, "socket");
	if(newFD0 == UINT32_MAX) { vfd1->close(); return TRACE_SYSCALL_RETURN(__WASI_EMFILE); }
	const __wasi_fd_t newFD1 = addVFD(process, vfd1, SOCKET_RIGHTS, "socket");
	if(newFD1 == UINT32_MAX)
	{
		// Roll back the first FD: remove it from the table and close it.
		Platform::RWMutex::ExclusiveLock fdsLock(process->fdMapMutex);
		std::shared_ptr<FDE>* fdePointer = process->fdMap.get(newFD0);
		std::shared_ptr<FDE> fde = *fdePointer;
		process->fdMap.removeOrFail(newFD0);
		fdsLock.unlock();
		fde->close();
		return TRACE_SYSCALL_RETURN(__WASI_EMFILE);
	}

	memoryRef<__wasi_fd_t>(process->memory, outFD0Address) = newFD0;
	memoryRef<__wasi_fd_t>(process->memory, outFD1Address) = newFD1;
	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS, "(%u, %u)", newFD0, newFD1);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_bind",
									__wasi_errno_return_t,
									wasi_sock_bind,
									__wasi_fd_t sock,
									WASIAddressIPtr addrPortAddress)
{
	TRACE_SYSCALL_IPTR("sock_bind", "(%u, " WASIADDRESSIPTR_FORMAT ")", sock, addrPortAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	LockedFDE lockedFDE = getLockedFDE(process, sock, 0, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(result); }
	}

	VFS::SocketAddress bindAddress;
	const __wasi_errno_t readResult = readAddrPort(process, addrPortAddress, bindAddress);
	if(readResult != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(readResult); }

	const VFS::Result result = lockedFDE.fde->vfd->sockBind(bindAddress);
	return TRACE_SYSCALL_RETURN(asWASIErrNo(result));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_listen",
									__wasi_errno_return_t,
									wasi_sock_listen,
									__wasi_fd_t sock,
									WASIAddressIPtr backlog)
{
	TRACE_SYSCALL_IPTR("sock_listen",
					   "(%u, " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   backlog);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	// Require the right to accept connections, since listening without it is useless.
	LockedFDE lockedFDE = getLockedFDE(process, sock, __WASI_RIGHT_SOCK_ACCEPT, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(result); }
	}

	const VFS::Result result = lockedFDE.fde->vfd->sockListen(backlog);
	return TRACE_SYSCALL_RETURN(asWASIErrNo(result));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_accept_v2",
									__wasi_errno_return_t,
									wasi_sock_accept_v2,
									__wasi_fd_t sock,
									__wasi_fdflags_t fdFlags,
									WASIAddressIPtr outFDAddress,
									WASIAddressIPtr outAddrPortAddress)
{
	TRACE_SYSCALL_IPTR("sock_accept_v2",
					   "(%u, 0x%04x, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   fdFlags,
					   outFDAddress,
					   outAddrPortAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	if(fdFlags & ~__WASI_FDFLAG_NONBLOCK) { return TRACE_SYSCALL_RETURN(__WASI_EINVAL); }

	LockedFDE lockedFDE = getLockedFDE(process, sock, __WASI_RIGHT_SOCK_ACCEPT, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(result); }
	}

	VFDFlags acceptedFlags;
	acceptedFlags.nonBlocking = (fdFlags & __WASI_FDFLAG_NONBLOCK) != 0;

	VFS::SocketAddress peerAddress;
	VFD* acceptedVFD = nullptr;
	const VFS::Result result
		= lockedFDE.fde->vfd->sockAccept(acceptedVFD, acceptedFlags, &peerAddress);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }
	WAVM_ASSERT(acceptedVFD);

	const __wasi_fd_t newFD = addVFD(process, acceptedVFD, SOCKET_RIGHTS, "socket");
	if(newFD == UINT32_MAX) { return TRACE_SYSCALL_RETURN(__WASI_EMFILE); }

	memoryRef<__wasi_fd_t>(process->memory, outFDAddress) = newFD;
	const __wasi_errno_t writeResult
		= writeAddrPort(process, peerAddress, outAddrPortAddress);
	if(writeResult != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(writeResult); }
	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS, "(%u)", newFD);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_connect",
									__wasi_errno_return_t,
									wasi_sock_connect,
									__wasi_fd_t sock,
									WASIAddressIPtr addrPortAddress)
{
	TRACE_SYSCALL_IPTR("sock_connect",
					   "(%u, " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   addrPortAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	LockedFDE lockedFDE = getLockedFDE(process, sock, 0, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(result); }
	}

	VFS::SocketAddress remoteAddress;
	const __wasi_errno_t readResult = readAddrPort(process, addrPortAddress, remoteAddress);
	if(readResult != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(readResult); }

	const VFS::Result result = lockedFDE.fde->vfd->sockConnect(remoteAddress);
	return TRACE_SYSCALL_RETURN(asWASIErrNo(result));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_recv_from",
									__wasi_errno_return_t,
									wasi_sock_recv_from,
									__wasi_fd_t sock,
									WASIAddressIPtr riDataAddress,
									WASIAddressIPtr numRIData,
									__wasi_riflags_t riFlags,
									WASIAddressIPtr outDataLenAddress,
									WASIAddressIPtr outFlagsAddress,
									WASIAddressIPtr outAddrPortAddress)
{
	TRACE_SYSCALL_IPTR("sock_recv_from",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
					   ", 0x%04x, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
					   ", " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   riDataAddress,
					   numRIData,
					   riFlags,
					   outDataLenAddress,
					   outFlagsAddress,
					   outAddrPortAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	return TRACE_SYSCALL_RETURN(sockRecvImpl(process,
										   sock,
										   riDataAddress,
										   numRIData,
										   riFlags,
										   outDataLenAddress,
										   outFlagsAddress,
										   outAddrPortAddress));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_send_to",
									__wasi_errno_return_t,
									wasi_sock_send_to,
									__wasi_fd_t sock,
									WASIAddressIPtr siDataAddress,
									WASIAddressIPtr numSIData,
									__wasi_siflags_t siFlags,
									WASIAddressIPtr destAddrPortAddress,
									WASIAddressIPtr outDataLenAddress)
{
	TRACE_SYSCALL_IPTR("sock_send_to",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
					   ", 0x%04x, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   siDataAddress,
					   numSIData,
					   siFlags,
					   destAddrPortAddress,
					   outDataLenAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	return TRACE_SYSCALL_RETURN(sockSendImpl(process,
										   sock,
										   siDataAddress,
										   numSIData,
										   siFlags,
										   destAddrPortAddress,
										   outDataLenAddress));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_send_file",
									__wasi_errno_return_t,
									wasi_sock_send_file,
									__wasi_fd_t outSock,
									__wasi_fd_t inFD,
									U64 offset,
									U64 count,
									WASIAddressIPtr outSentAddress)
{
	TRACE_SYSCALL_IPTR("sock_send_file",
					   "(%u, %u, %llu, %llu, " WASIADDRESSIPTR_FORMAT ")",
					   outSock,
					   inFD,
					   offset,
					   count,
					   outSentAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	LockedFDE outFDE = getLockedFDE(process, outSock, __WASI_RIGHT_FD_WRITE, 0);
	if(outFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(outFDE.error); }
	{
		const __wasi_errno_t result = requireSocket(*outFDE.fde);
		if(result != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(result); }
	}

	LockedFDE inFDE = getLockedFDE(process, inFD, __WASI_RIGHT_FD_READ, 0);
	if(inFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(inFDE.error); }

	// Pump the input through a bounce buffer.
	U8 buffer[65536];
	U64 numSent = 0;
	while(numSent < count)
	{
		const Uptr chunkBytes
			= count - numSent < U64(sizeof(buffer)) ? Uptr(count - numSent) : sizeof(buffer);

		IOReadBuffer readBuffer;
		readBuffer.data = buffer;
		readBuffer.numBytes = chunkBytes;
		Uptr numRead = 0;
		const U64 readOffset = offset + numSent;
		const VFS::Result readResult
			= inFDE.fde->vfd->readv(&readBuffer, 1, &numRead, &readOffset);
		if(readResult != VFS::Result::success)
		{
			return TRACE_SYSCALL_RETURN(asWASIErrNo(readResult));
		}
		if(numRead == 0) { break; }

		Uptr numWritten = 0;
		while(numWritten < numRead)
		{
			IOWriteBuffer writeBuffer;
			writeBuffer.data = buffer + numWritten;
			writeBuffer.numBytes = numRead - numWritten;
			Uptr wrote = 0;
			const VFS::Result sendResult
				= outFDE.fde->vfd->sockSend(&writeBuffer, 1, nullptr, false, &wrote);
			if(sendResult != VFS::Result::success)
			{
				return TRACE_SYSCALL_RETURN(asWASIErrNo(sendResult));
			}
			if(wrote == 0) { break; }
			numWritten += wrote;
		}
		numSent += numWritten;
		if(numWritten < numRead) { break; }
	}

	memoryRef<U64>(process->memory, outSentAddress) = numSent;
	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS, "(%llu sent)", numSent);
}

// Reads the FD + option for the sock_*_opt_* syscalls, translating the WASIX option tag to
// a VFS::SocketOption (the tag values coincide with the VFS enum values). On failure the
// returned LockedFDE carries the errno. Defined in WASIFile.cpp since it has no
// WASIAddressIPtr parameters.

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_set_opt_flag",
									__wasi_errno_return_t,
									wasi_sock_set_opt_flag,
									__wasi_fd_t sock,
									U8 option,
									U8 flag)
{
	TRACE_SYSCALL_IPTR("sock_set_opt_flag", "(%u, %u, %u)", sock, option, flag);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	if(option > __WASI_SOCK_OPTION_PROTO
	   || classifySockOption(option) != WASIXSockOptKind::flag)
	{
		return TRACE_SYSCALL_RETURN(__WASI_ENOPROTOOPT);
	}

	LockedFDE lockedFDE = getLockedFDE(process, sock, 0, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(result); }
	}

	return TRACE_SYSCALL_RETURN(asWASIErrNo(lockedFDE.fde->vfd->sockSetOpt(
		VFS::SocketOption(option), flag != 0 ? 1 : 0)));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_get_opt_flag",
									__wasi_errno_return_t,
									wasi_sock_get_opt_flag,
									__wasi_fd_t sock,
									U8 option,
									WASIAddressIPtr outFlagAddress)
{
	TRACE_SYSCALL_IPTR("sock_get_opt_flag",
					   "(%u, %u, " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   option,
					   outFlagAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	VFS::SocketOption vfsOption;
	LockedFDE lockedFDE = getSockOptFDE(
		process, sock, option, WASIXSockOptKind::flag, vfsOption);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	U64 value = 0;
	const VFS::Result result = lockedFDE.fde->vfd->sockGetOpt(vfsOption, value);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }

	memoryRef<U8>(process->memory, outFlagAddress) = U8(value);
	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS, "(%u)", U32(value));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_set_opt_time",
									__wasi_errno_return_t,
									wasi_sock_set_opt_time,
									__wasi_fd_t sock,
									U8 option,
									WASIAddressIPtr timeoutAddress)
{
	TRACE_SYSCALL_IPTR("sock_set_opt_time",
					   "(%u, %u, " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   option,
					   timeoutAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	if(option > __WASI_SOCK_OPTION_PROTO
	   || classifySockOption(option) != WASIXSockOptKind::time)
	{
		return TRACE_SYSCALL_RETURN(__WASI_ENOPROTOOPT);
	}

	LockedFDE lockedFDE = getLockedFDE(process, sock, 0, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(result); }
	}

	// __wasi_option_timestamp_t: {u8 tag; u8 pad[7]; u64 some}.
	__wasi_errno_t readResult = __WASI_ESUCCESS;
	U64 value = 0;
	Runtime::catchRuntimeExceptions(
		[&] {
			const U8 tag = memoryRef<U8>(process->memory, timeoutAddress);
			if(tag == __WASI_OPTION_SOME)
			{
				value = memoryRef<U64>(process->memory, timeoutAddress + 8);
			}
		},
		[&](Exception* exception) {
			WAVM_ERROR_UNLESS(getExceptionType(exception)
							  == ExceptionTypes::outOfBoundsMemoryAccess);
			destroyException(exception);
			readResult = __WASI_EFAULT;
		});
	if(readResult != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(readResult); }

	return TRACE_SYSCALL_RETURN(asWASIErrNo(
		lockedFDE.fde->vfd->sockSetOpt(VFS::SocketOption(option), value)));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_get_opt_time",
									__wasi_errno_return_t,
									wasi_sock_get_opt_time,
									__wasi_fd_t sock,
									U8 option,
									WASIAddressIPtr outTimeoutAddress)
{
	TRACE_SYSCALL_IPTR("sock_get_opt_time",
					   "(%u, %u, " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   option,
					   outTimeoutAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	VFS::SocketOption vfsOption;
	LockedFDE lockedFDE = getSockOptFDE(
		process, sock, option, WASIXSockOptKind::time, vfsOption);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	U64 value = 0;
	const VFS::Result result = lockedFDE.fde->vfd->sockGetOpt(vfsOption, value);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }

	memoryRef<U8>(process->memory, outTimeoutAddress)
		= value ? __WASI_OPTION_SOME : __WASI_OPTION_NONE;
	memoryRef<U64>(process->memory, outTimeoutAddress + 8) = value;
	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS, "(%llu ns)", value);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_set_opt_size",
									__wasi_errno_return_t,
									wasi_sock_set_opt_size,
									__wasi_fd_t sock,
									U8 option,
									U64 size)
{
	TRACE_SYSCALL_IPTR("sock_set_opt_size", "(%u, %u, %llu)", sock, option, size);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	if(option > __WASI_SOCK_OPTION_PROTO
	   || classifySockOption(option) != WASIXSockOptKind::size)
	{
		return TRACE_SYSCALL_RETURN(__WASI_ENOPROTOOPT);
	}

	LockedFDE lockedFDE = getLockedFDE(process, sock, 0, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(result); }
	}

	return TRACE_SYSCALL_RETURN(asWASIErrNo(
		lockedFDE.fde->vfd->sockSetOpt(VFS::SocketOption(option), size)));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_get_opt_size",
									__wasi_errno_return_t,
									wasi_sock_get_opt_size,
									__wasi_fd_t sock,
									U8 option,
									WASIAddressIPtr outSizeAddress)
{
	TRACE_SYSCALL_IPTR("sock_get_opt_size",
					   "(%u, %u, " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   option,
					   outSizeAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }

	VFS::SocketOption vfsOption;
	LockedFDE lockedFDE = getSockOptFDE(
		process, sock, option, WASIXSockOptKind::size, vfsOption);
	if(lockedFDE.error != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(lockedFDE.error); }

	U64 value = 0;
	const VFS::Result result = lockedFDE.fde->vfd->sockGetOpt(vfsOption, value);
	if(result != VFS::Result::success) { return TRACE_SYSCALL_RETURN(asWASIErrNo(result)); }

	if(vfsOption == VFS::SocketOption::type)
	{
		// Translate the VFS FileType value to a WASIX __wasi_sock_type_t value.
		value = FileType(value) == FileType::streamSocket ? __WASI_SOCK_TYPE_SOCKET_STREAM
				: FileType(value) == FileType::datagramSocket ? __WASI_SOCK_TYPE_SOCKET_DGRAM
															 : __WASI_SOCK_TYPE_SOCKET_UNUSED;
	}

	memoryRef<U64>(process->memory, outSizeAddress) = value;
	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS, "(%llu)", value);
}

static __wasi_errno_t sockMulticastImpl(Process* process,
									  __wasi_fd_t sock,
									  WASIAddressIPtr groupAddress,
									  WASIAddressIPtr interfaceAddress,
									  U32 interfaceIndex,
									  bool ipv6,
									  bool join)
{
	LockedFDE lockedFDE = getLockedFDE(process, sock, 0, 0);
	if(lockedFDE.error != __WASI_ESUCCESS) { return lockedFDE.error; }
	{
		const __wasi_errno_t result = requireSocket(*lockedFDE.fde);
		if(result != __WASI_ESUCCESS) { return result; }
	}

	U8 group[16];
	U8 interfaceAddr[4];
	__wasi_errno_t result = __WASI_ESUCCESS;
	Runtime::catchRuntimeExceptions(
		[&] {
			memcpy(group,
				   memoryArrayPtr<const U8>(process->memory, groupAddress, ipv6 ? 16 : 4),
				   ipv6 ? 16 : 4);
			if(interfaceAddress)
			{
				memcpy(interfaceAddr,
					   memoryArrayPtr<const U8>(process->memory, interfaceAddress, 4),
					   4);
			}
			else { memset(interfaceAddr, 0, 4); }
		},
		[&](Exception* exception) {
			WAVM_ERROR_UNLESS(getExceptionType(exception)
							  == ExceptionTypes::outOfBoundsMemoryAccess);
			destroyException(exception);
			result = __WASI_EFAULT;
		});
	if(result != __WASI_ESUCCESS) { return result; }

	const VFS::Result mcastResult
		= ipv6 ? (join ? lockedFDE.fde->vfd->sockJoinMulticastV6(group, interfaceIndex)
					   : lockedFDE.fde->vfd->sockLeaveMulticastV6(group, interfaceIndex))
			   : (join ? lockedFDE.fde->vfd->sockJoinMulticastV4(group, interfaceAddr)
					   : lockedFDE.fde->vfd->sockLeaveMulticastV4(group, interfaceAddr));
	return asWASIErrNo(mcastResult);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_join_multicast_v4",
									__wasi_errno_return_t,
									wasi_sock_join_multicast_v4,
									__wasi_fd_t sock,
									WASIAddressIPtr groupAddress,
									WASIAddressIPtr interfaceAddress)
{
	TRACE_SYSCALL_IPTR("sock_join_multicast_v4",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   groupAddress,
					   interfaceAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }
	return TRACE_SYSCALL_RETURN(
		sockMulticastImpl(process, sock, groupAddress, interfaceAddress, 0, false, true));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_leave_multicast_v4",
									__wasi_errno_return_t,
									wasi_sock_leave_multicast_v4,
									__wasi_fd_t sock,
									WASIAddressIPtr groupAddress,
									WASIAddressIPtr interfaceAddress)
{
	TRACE_SYSCALL_IPTR("sock_leave_multicast_v4",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   sock,
					   groupAddress,
					   interfaceAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }
	return TRACE_SYSCALL_RETURN(
		sockMulticastImpl(process, sock, groupAddress, interfaceAddress, 0, false, false));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_join_multicast_v6",
									__wasi_errno_return_t,
									wasi_sock_join_multicast_v6,
									__wasi_fd_t sock,
									WASIAddressIPtr groupAddress,
									U32 interfaceIndex)
{
	TRACE_SYSCALL_IPTR("sock_join_multicast_v6",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", %u)",
					   sock,
					   groupAddress,
					   interfaceIndex);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }
	return TRACE_SYSCALL_RETURN(
		sockMulticastImpl(process, sock, groupAddress, 0, interfaceIndex, true, true));
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"sock_leave_multicast_v6",
									__wasi_errno_return_t,
									wasi_sock_leave_multicast_v6,
									__wasi_fd_t sock,
									WASIAddressIPtr groupAddress,
									U32 interfaceIndex)
{
	TRACE_SYSCALL_IPTR("sock_leave_multicast_v6",
					   "(%u, " WASIADDRESSIPTR_FORMAT ", %u)",
					   sock,
					   groupAddress,
					   interfaceIndex);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }
	return TRACE_SYSCALL_RETURN(
		sockMulticastImpl(process, sock, groupAddress, 0, interfaceIndex, true, false));
}

// Resolves a host name via the host's resolver. The output is an array of 18-byte
// __wasi_addr_ip_t records: {u8 tag; u8 pad; u8 ip[4 or 16]} with the union at offset 2.
WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasiFile,
									"resolve",
									__wasi_errno_return_t,
									wasi_resolve,
									WASIAddressIPtr hostNameAddress,
									U16 port,
									WASIAddressIPtr outAddressesAddress,
									WASIAddressIPtr numAddressesCapacity,
									WASIAddressIPtr outNumAddressesAddress)
{
	TRACE_SYSCALL_IPTR("resolve",
					   "(" WASIADDRESSIPTR_FORMAT ", %u, " WASIADDRESSIPTR_FORMAT
					   ", " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   hostNameAddress,
					   port,
					   outAddressesAddress,
					   numAddressesCapacity,
					   outNumAddressesAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);
	if(!process->networkEnabled) { return TRACE_SYSCALL_RETURN(__WASI_ENOTCAPABLE); }
	if(!hostNameAddress || numAddressesCapacity == 0)
	{
		return TRACE_SYSCALL_RETURN(__WASI_EINVAL);
	}

	// Read the NUL-terminated host name out of the sandbox, bounding the scan.
	__wasi_errno_t result = __WASI_ESUCCESS;
	std::string hostName;
	Runtime::catchRuntimeExceptions(
		[&] {
			const char* hostChars = memoryArrayPtr<const char>(
				process->memory, hostNameAddress, 256);
			Uptr nameLen = 0;
			while(nameLen < 256 && hostChars[nameLen]) { ++nameLen; }
			hostName.assign(hostChars, nameLen);
		},
		[&](Exception* exception) {
			WAVM_ERROR_UNLESS(getExceptionType(exception)
							  == ExceptionTypes::outOfBoundsMemoryAccess);
			destroyException(exception);
			result = __WASI_EFAULT;
		});
	if(result != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(result); }

	const Uptr capacity
		= numAddressesCapacity < WASIAddressIPtr(64) ? Uptr(numAddressesCapacity) : 64;
	SocketAddress resolvedAddresses[64];
	Uptr numAddresses = capacity;
	const VFS::Result resolveResult = Platform::resolveAddress(
		hostName, port, true, true, resolvedAddresses, &numAddresses);
	if(resolveResult != VFS::Result::success)
	{
		return TRACE_SYSCALL_RETURN(asWASIErrNo(resolveResult));
	}

	// Serialize each result as an 18-byte __wasi_addr_ip_t record.
	Runtime::catchRuntimeExceptions(
		[&] {
			U8* dest = memoryArrayPtr<U8>(
				process->memory, outAddressesAddress, numAddresses * Uptr(18));
			for(Uptr index = 0; index < numAddresses; ++index)
			{
				U8* record = dest + index * Uptr(18);
				memset(record, 0, 18);
				if(resolvedAddresses[index].family == SocketAddress::Family::ipv4)
				{
					record[0] = __WASI_ADDRESS_FAMILY_INET4;
					memcpy(record + 2, resolvedAddresses[index].ipBytes, 4);
				}
				else
				{
					record[0] = __WASI_ADDRESS_FAMILY_INET6;
					memcpy(record + 2, resolvedAddresses[index].ipBytes, 16);
				}
			}
		},
		[&](Exception* exception) {
			WAVM_ERROR_UNLESS(getExceptionType(exception)
							  == ExceptionTypes::outOfBoundsMemoryAccess);
			destroyException(exception);
			result = __WASI_EFAULT;
		});
	if(result != __WASI_ESUCCESS) { return TRACE_SYSCALL_RETURN(result); }

	memoryRef<WASIAddressIPtr>(process->memory, outNumAddressesAddress)
		= WASIAddressIPtr(numAddresses);
	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS, "(%u addresses)", U32(numAddresses));
}
