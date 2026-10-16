WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasi,
									"poll_oneoff",
									__wasi_errno_return_t,
									wasi_poll_oneoff,
									WASIAddressIPtr inAddress,
									WASIAddressIPtr outAddress,
									WASIAddressIPtr numSubscriptions,
									WASIAddressIPtr outNumEventsAddress)
{
	TRACE_SYSCALL_IPTR("poll_oneoff",
					   "(" WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT
					   ", " WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   inAddress,
					   outAddress,
					   numSubscriptions,
					   outNumEventsAddress);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	const Uptr numSubs = Uptr(numSubscriptions);
	if(numSubs == 0) { return TRACE_SYSCALL_RETURN(__WASI_EINVAL); }

	__wasi_errno_t result = __WASI_ESUCCESS;
	Uptr numEvents = 0;

	Runtime::catchRuntimeExceptions(
		[&] {
			__wasi_subscription_t* subscriptions
				= memoryArrayPtr<__wasi_subscription_t>(process->memory, inAddress, numSubs);
			__wasi_event_t* events
				= memoryArrayPtr<__wasi_event_t>(process->memory, outAddress, numSubs);

			// The host descriptors to wait on, with the subscription each came from.
			std::vector<Platform::PollFD> pollFDs;
			std::vector<Uptr> pollFDSubs;

			// Subscriptions answered without waiting: descriptors without a waitable host
			// descriptor, bad descriptors, and clock subscriptions whose deadline has already
			// passed. Emitting any of these means we must not block.
			struct ImmediateSub
			{
				Uptr index;
				__wasi_errno_t error;
			};
			std::vector<ImmediateSub> immediateSubs;

			// Clock subscriptions, each with an absolute deadline in its own clock's timescale.
			struct ClockSub
			{
				Uptr index;
				Platform::Clock clock;
				U64 deadlineNs;
			};
			std::vector<ClockSub> clockSubs;

			for(Uptr i = 0; i < numSubs; ++i)
			{
				const __wasi_subscription_t& sub = subscriptions[i];
				if(sub.type == __WASI_EVENTTYPE_FD_READ || sub.type == __WASI_EVENTTYPE_FD_WRITE)
				{
					const __wasi_fd_t fd = sub.u.fd_readwrite.fd;

					Uptr hostFD = 0;
					bool pollable = false;
					__wasi_errno_t lookupError = __WASI_ESUCCESS;
					{
						Platform::RWMutex::ExclusiveLock fdsLock(process->fdMapMutex);
						if(fd < process->fdMap.getMinIndex() || fd > process->fdMap.getMaxIndex())
						{
							lookupError = __WASI_EBADF;
						}
						else
						{
							std::shared_ptr<FDE>* fdePointer = process->fdMap.get(fd);
							if(!fdePointer) { lookupError = __WASI_EBADF; }
							else
							{
								std::shared_ptr<FDE> fde = *fdePointer;
								Platform::RWMutex::ExclusiveLock fdeLock(fde->mutex);
								if(!(fde->rights & __WASI_RIGHT_POLL_FD_READWRITE))
								{
									lookupError = __WASI_ENOTCAPABLE;
								}
								else
								{
									pollable = fde->vfd->getPollableFD(hostFD);
								}
							}
						}
					}

					if(lookupError != __WASI_ESUCCESS)
					{
						immediateSubs.push_back({i, lookupError});
					}
					else if(!pollable)
					{
						// The host can't wait on this descriptor, so report it ready now rather
						// than hanging the caller.
						immediateSubs.push_back({i, __WASI_ESUCCESS});
					}
					else
					{
						Platform::PollFD pollFD;
						pollFD.fd = hostFD;
						pollFD.waitRead = sub.type == __WASI_EVENTTYPE_FD_READ;
						pollFD.waitWrite = sub.type == __WASI_EVENTTYPE_FD_WRITE;
						pollFDs.push_back(pollFD);
						pollFDSubs.push_back(i);
					}
				}
				else if(sub.type == __WASI_EVENTTYPE_CLOCK)
				{
					Platform::Clock platformClock;
					bool clockValid = true;
					switch(sub.u.clock.clock_id)
					{
					case __WASI_CLOCK_REALTIME: platformClock = Platform::Clock::realtime; break;
					case __WASI_CLOCK_MONOTONIC: platformClock = Platform::Clock::monotonic; break;
					case __WASI_CLOCK_PROCESS_CPUTIME_ID:
					case __WASI_CLOCK_THREAD_CPUTIME_ID:
						platformClock = Platform::Clock::processCPUTime;
						break;
					default: clockValid = false; break;
					}

					if(!clockValid)
					{
						immediateSubs.push_back({i, __WASI_EINVAL});
					}
					else
					{
						const U64 nowNs = U64(Platform::getClockTime(platformClock).ns);
						const U64 timeoutNs = sub.u.clock.timeout;
						const bool absolute
							= (sub.u.clock.flags & __WASI_SUBSCRIPTION_CLOCK_ABSTIME) != 0;
						const U64 deadlineNs = absolute ? timeoutNs : nowNs + timeoutNs;
						if(deadlineNs <= nowNs)
						{
							immediateSubs.push_back({i, __WASI_ESUCCESS});
						}
						else
						{
							clockSubs.push_back({i, platformClock, deadlineNs});
						}
					}
				}
				else
				{
					result = __WASI_EINVAL;
					return;
				}
			}

			// Work out how long to wait: none if there's an immediate result, otherwise the
			// earliest clock deadline, otherwise forever.
			I64 timeoutNanos = -1;
			if(immediateSubs.size() != 0)
			{
				timeoutNanos = 0;
			}
			else if(clockSubs.size() != 0)
			{
				U64 minRemainingNs = UINT64_MAX;
				for(const ClockSub& clockSub : clockSubs)
				{
					const U64 nowNs = U64(Platform::getClockTime(clockSub.clock).ns);
					const U64 remaining
						= clockSub.deadlineNs > nowNs ? clockSub.deadlineNs - nowNs : 0;
					if(remaining < minRemainingNs) { minRemainingNs = remaining; }
				}
				timeoutNanos = minRemainingNs > U64(INT64_MAX) ? INT64_MAX : I64(minRemainingNs);
			}

			// Wait for a descriptor, a clock deadline, or both.
			if(pollFDs.size() != 0 || timeoutNanos >= 0)
			{
				VFS::Result pollResult;
				do
				{
					pollResult
						= Platform::pollFDs(pollFDs.data(), pollFDs.size(), timeoutNanos);
				} while(pollResult == VFS::Result::interruptedBySignal);

				if(pollResult != VFS::Result::success)
				{
					result = __WASI_EIO;
					return;
				}
			}

			auto emitEvent = [&](Uptr subIndex, __wasi_errno_t error) {
				__wasi_event_t& event = events[numEvents++];
				event = __wasi_event_t{};
				event.userdata = subscriptions[subIndex].userdata;
				event.error = error;
				event.type = subscriptions[subIndex].type;
			};

			for(const ImmediateSub& immediateSub : immediateSubs)
			{
				emitEvent(immediateSub.index, immediateSub.error);
			}
			for(Uptr i = 0; i < pollFDs.size(); ++i)
			{
				if(pollFDs[i].ready) { emitEvent(pollFDSubs[i], __WASI_ESUCCESS); }
			}

			// If the wait ended without a ready descriptor, it must have been a clock deadline.
			if(numEvents == 0)
			{
				for(const ClockSub& clockSub : clockSubs)
				{
					const U64 nowNs = U64(Platform::getClockTime(clockSub.clock).ns);
					if(nowNs >= clockSub.deadlineNs)
					{
						emitEvent(clockSub.index, __WASI_ESUCCESS);
						break;
					}
				}
				if(numEvents == 0 && clockSubs.size() != 0)
				{
					emitEvent(clockSubs[0].index, __WASI_ESUCCESS);
				}
			}

			memoryRef<WASIAddressIPtr>(process->memory, outNumEventsAddress)
				= WASIAddressIPtr(numEvents);
		},
		[&](Exception* exception) {
			WAVM_ERROR_UNLESS(getExceptionType(exception)
							  == ExceptionTypes::outOfBoundsMemoryAccess);
			destroyException(exception);
			result = __WASI_EFAULT;
		});

	return TRACE_SYSCALL_RETURN(result);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasi,
									"proc_exit",
									void,
									wasi_proc_exit,
									__wasi_exitcode_t exitCode)
{
	TRACE_SYSCALL_IPTR("proc_exit", "(%u)", exitCode);
	throw ExitException{exitCode};
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasi,
									"proc_raise",
									__wasi_errno_return_t,
									wasi_proc_raise,
									__wasi_signal_t sig)
{
	// proc_raise will possibly be removed: https://github.com/WebAssembly/WASI/issues/7
	UNIMPLEMENTED_SYSCALL_IPTR("proc_raise", "(%u)", sig);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasi,
									"random_get",
									__wasi_errno_return_t,
									wasi_random_get,
									WASIAddressIPtr bufferAddress,
									WASIAddressIPtr numBufferBytes)
{
	TRACE_SYSCALL_IPTR("random_get",
					   "(" WASIADDRESSIPTR_FORMAT ", " WASIADDRESSIPTR_FORMAT ")",
					   bufferAddress,
					   numBufferBytes);

	Process* process = getProcessFromContextRuntimeData(contextRuntimeData);

	__wasi_errno_t result = __WASI_ESUCCESS;
	Runtime::catchRuntimeExceptions(
		[&] {
			U8* buffer = memoryArrayPtr<U8>(process->memory, bufferAddress, numBufferBytes);
			Platform::getCryptographicRNG(buffer, numBufferBytes);
		},
		[&](Runtime::Exception* exception) {
			WAVM_ASSERT(getExceptionType(exception) == ExceptionTypes::outOfBoundsMemoryAccess);
			result = __WASI_EFAULT;
		});

	return TRACE_SYSCALL_RETURN(result);
}

WAVM_DEFINE_INTRINSIC_FUNCTION_IPTR(wasi, "sched_yield", __wasi_errno_return_t, wasi_sched_yield)
{
	TRACE_SYSCALL_IPTR("sched_yield", "()");
	Platform::yieldToAnotherThread();
	return TRACE_SYSCALL_RETURN(__WASI_ESUCCESS);
}
