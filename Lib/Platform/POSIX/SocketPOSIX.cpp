#ifndef _GNU_SOURCE
#define _GNU_SOURCE // For accept4, SOCK_CLOEXEC, SOCK_NONBLOCK, MSG_NOSIGNAL
#endif

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <memory>
#include <string>
#include <vector>
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Platform/Socket.h"
#include "WAVM/VFS/VFS.h"

using namespace WAVM;
using namespace WAVM::Platform;
using namespace WAVM::VFS;

static_assert(offsetof(struct iovec, iov_base) == offsetof(IOReadBuffer, data)
				  && offsetof(struct iovec, iov_len) == offsetof(IOReadBuffer, numBytes)
				  && sizeof(((struct iovec*)nullptr)->iov_base) == sizeof(IOReadBuffer::data)
				  && sizeof(((struct iovec*)nullptr)->iov_len) == sizeof(IOReadBuffer::numBytes)
				  && sizeof(struct iovec) == sizeof(IOReadBuffer),
			  "IOReadBuffer must match iovec");

static_assert(offsetof(struct iovec, iov_base) == offsetof(IOWriteBuffer, data)
				  && offsetof(struct iovec, iov_len) == offsetof(IOWriteBuffer, numBytes)
				  && sizeof(((struct iovec*)nullptr)->iov_base) == sizeof(IOWriteBuffer::data)
				  && sizeof(((struct iovec*)nullptr)->iov_len) == sizeof(IOWriteBuffer::numBytes)
				  && sizeof(struct iovec) == sizeof(IOWriteBuffer),
			  "IOWriteBuffer must match iovec");

static Result asSocketVFSResult(int error)
{
	switch(error)
	{
	case EINTR: return Result::interruptedBySignal;
	case EAGAIN:
#if EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK:
#endif
		return Result::wouldBlock;
	case EIO: return Result::ioDeviceError;
	case EFAULT: return Result::inaccessibleBuffer;
	case EPERM: return Result::notPermitted;
	case EACCES: return Result::notAccessible;
	case EMFILE: return Result::outOfProcessFDs;
	case ENFILE: return Result::outOfSystemFDs;
	case ENOMEM: return Result::outOfMemory;
	case EPIPE: return Result::brokenPipe;
	case EBUSY: return Result::busy;
	case ENOTSUP: return Result::notSupported;
#if EOPNOTSUPP != ENOTSUP
	case EOPNOTSUPP: return Result::notSupported;
#endif
	case EAFNOSUPPORT: return Result::notSupported;
	case EPFNOSUPPORT: return Result::notSupported;
	case EPROTONOSUPPORT: return Result::notSupported;
	case ENOPROTOOPT: return Result::notSupported;
	case EBADF: return Result::notPermitted;
	case EINVAL: return Result::notPermitted;

	case ENOTSOCK: return Result::notSocket;
	case ENOTCONN: return Result::notConnected;
	case ECONNREFUSED: return Result::connectionRefused;
	case ECONNRESET: return Result::connectionReset;
	case ECONNABORTED: return Result::connectionAborted;
	case ETIMEDOUT: return Result::timedOut;
	case EADDRINUSE: return Result::addressInUse;
	case EADDRNOTAVAIL: return Result::addressNotAvailable;
	case EHOSTUNREACH: return Result::hostUnreachable;
	case ENETUNREACH: return Result::networkUnreachable;
	case EISCONN: return Result::notPermitted;
	case EALREADY: return Result::ioPending;
	case EINPROGRESS: return Result::ioPending;
	case EDESTADDRREQ: return Result::notConnected;
	case EMSGSIZE: return Result::tooManyBufferBytes;
	case ENOBUFS: return Result::outOfMemory;

	default:
		Errors::fatalfWithCallStack("Unexpected socket error code: %i (%s)", error, strerror(error));
	};
}

static SocketAddress asSocketAddress(const struct sockaddr* addr, socklen_t addrLen)
{
	SocketAddress result;
	memset(&result, 0, sizeof(result));

	if(addr->sa_family == AF_INET && addrLen >= sizeof(struct sockaddr_in))
	{
		const struct sockaddr_in* addr4 = (const struct sockaddr_in*)addr;
		result.family = SocketAddress::Family::ipv4;
		result.port = ntohs(addr4->sin_port);
		result.scopeId = 0;
		memcpy(result.ipBytes, &addr4->sin_addr, 4);
	}
	else if(addr->sa_family == AF_INET6 && addrLen >= sizeof(struct sockaddr_in6))
	{
		const struct sockaddr_in6* addr6 = (const struct sockaddr_in6*)addr;
		result.family = SocketAddress::Family::ipv6;
		result.port = ntohs(addr6->sin6_port);
		result.scopeId = addr6->sin6_scope_id;
		memcpy(result.ipBytes, &addr6->sin6_addr, 16);
	}
	else { WAVM_UNREACHABLE(); }

	return result;
}

static bool asSockAddr(const SocketAddress& address,
					   struct sockaddr_storage& outAddr,
					   socklen_t& outAddrLen)
{
	memset(&outAddr, 0, sizeof(outAddr));

	switch(address.family)
	{
	case SocketAddress::Family::ipv4:
	{
		struct sockaddr_in* addr4 = (struct sockaddr_in*)&outAddr;
		addr4->sin_family = AF_INET;
		addr4->sin_port = htons(address.port);
		memcpy(&addr4->sin_addr, address.ipBytes, 4);
		outAddrLen = sizeof(struct sockaddr_in);
		return true;
	}
	case SocketAddress::Family::ipv6:
	{
		struct sockaddr_in6* addr6 = (struct sockaddr_in6*)&outAddr;
		addr6->sin6_family = AF_INET6;
		addr6->sin6_port = htons(address.port);
		addr6->sin6_scope_id = address.scopeId;
		memcpy(&addr6->sin6_addr, address.ipBytes, 16);
		outAddrLen = sizeof(struct sockaddr_in6);
		return true;
	}
	default: return false;
	};
}

struct POSIXSocketVFD : VFD
{
	const I32 fd;
	const FileType fileType;

	POSIXSocketVFD(I32 inFD, FileType inFileType) : fd(inFD), fileType(inFileType)
	{
		WAVM_ASSERT(fileType == FileType::streamSocket || fileType == FileType::datagramSocket);
	}

	virtual Result close() override
	{
		if(::close(fd)) {}
		delete this;
		return Result::success;
	}

	virtual Result seek(I64 offset, SeekOrigin origin, U64* outAbsoluteOffset = nullptr) override
	{
		return Result::notSeekable;
	}

	virtual Result readv(const IOReadBuffer* buffers,
						 Uptr numBuffers,
						 Uptr* outNumBytesRead = nullptr,
						 const U64* offset = nullptr) override
	{
		if(offset != nullptr) { return Result::notSeekable; }
		if(outNumBytesRead) { *outNumBytesRead = 0; }

		if(numBuffers == 0) { return Result::success; }
#ifdef IOV_MAX
		else if(numBuffers > IOV_MAX) { return Result::tooManyBuffers; }
#endif

		ssize_t result = ::readv(fd, (const struct iovec*)buffers, numBuffers);
		if(result == -1) { return asSocketVFSResult(errno); }

		if(outNumBytesRead) { *outNumBytesRead = result; }
		return Result::success;
	}

	virtual Result writev(const IOWriteBuffer* buffers,
						  Uptr numBuffers,
						  Uptr* outNumBytesWritten = nullptr,
						  const U64* offset = nullptr) override
	{
		if(offset != nullptr) { return Result::notSeekable; }
		if(outNumBytesWritten) { *outNumBytesWritten = 0; }

		if(numBuffers == 0) { return Result::success; }
#ifdef IOV_MAX
		else if(numBuffers > IOV_MAX) { return Result::tooManyBuffers; }
#endif

		// Use sendmsg with MSG_NOSIGNAL so writing to a closed connection doesn't raise
		// SIGPIPE and kill the process.
		struct msghdr message;
		memset(&message, 0, sizeof(message));
		message.msg_iov = (struct iovec*)buffers;
		message.msg_iovlen = numBuffers;

		ssize_t result = ::sendmsg(fd, &message, MSG_NOSIGNAL);
		if(result == -1) { return asSocketVFSResult(errno); }

		if(outNumBytesWritten) { *outNumBytesWritten = result; }
		return Result::success;
	}

	virtual Result sync(SyncType syncType) override { return Result::notSynchronizable; }

	virtual Result getVFDInfo(VFDInfo& outInfo) override
	{
		outInfo.type = fileType;
		outInfo.flags.append = false;
		outInfo.flags.syncLevel = VFDSync::none;

		I32 fdFlags = fcntl(fd, F_GETFL);
		if(fdFlags < 0) { return asSocketVFSResult(errno); }
		outInfo.flags.nonBlocking = fdFlags & O_NONBLOCK;

		return Result::success;
	}

	virtual Result getFileInfo(FileInfo& outInfo) override
	{
		struct stat fdStatus;
		if(fstat(fd, &fdStatus) != 0) { return asSocketVFSResult(errno); }

		memset(&outInfo, 0, sizeof(outInfo));
		outInfo.deviceNumber = fdStatus.st_dev;
		outInfo.fileNumber = fdStatus.st_ino;
		outInfo.type = fileType;
		outInfo.numLinks = fdStatus.st_nlink;
		return Result::success;
	}

	virtual Result setVFDFlags(const VFDFlags& vfsFlags) override
	{
		// Sockets don't support append or sync flags.
		if(vfsFlags.append || vfsFlags.syncLevel != VFDSync::none) { return Result::notSupported; }

		I32 fdFlags = fcntl(fd, F_GETFL);
		if(fdFlags < 0) { return asSocketVFSResult(errno); }

		fdFlags = vfsFlags.nonBlocking ? (fdFlags | O_NONBLOCK) : (fdFlags & ~O_NONBLOCK);
		return fcntl(fd, F_SETFL, fdFlags) == 0 ? Result::success : asSocketVFSResult(errno);
	}

	virtual Result setFileSize(U64 numBytes) override { return Result::notSupported; }

	virtual Result setFileTimes(bool setLastAccessTime,
								Time lastAccessTime,
								bool setLastWriteTime,
								Time lastWriteTime) override
	{
		return Result::notSupported;
	}

	virtual Result openDir(DirEntStream*& outStream) override { return Result::isNotDirectory; }

	virtual Result sockAccept(VFD*& outVFD, const VFDFlags& acceptedFlags) override
	{
		outVFD = nullptr;

		I32 acceptFlags = SOCK_CLOEXEC;
		if(acceptedFlags.nonBlocking) { acceptFlags |= SOCK_NONBLOCK; }

		struct sockaddr_storage peerAddr;
		socklen_t peerAddrLen = sizeof(peerAddr);
#ifdef SOCK_NONBLOCK
		const I32 connectionFD
			= accept4(fd, (struct sockaddr*)&peerAddr, &peerAddrLen, acceptFlags);
#else
		const I32 connectionFD = accept(fd, (struct sockaddr*)&peerAddr, &peerAddrLen);
#endif
		if(connectionFD < 0) { return asSocketVFSResult(errno); }

#ifndef SOCK_NONBLOCK
		// Set the accepted FD's flags manually if accept4 isn't available.
		if(fcntl(connectionFD, F_SETFD, FD_CLOEXEC) != 0
		   || fcntl(connectionFD,
					F_SETFL,
					acceptedFlags.nonBlocking ? O_NONBLOCK : 0)
			   != 0)
		{
			const int fcntlError = errno;
			::close(connectionFD);
			return asSocketVFSResult(fcntlError);
		}
#endif

		outVFD = new POSIXSocketVFD(connectionFD, FileType::streamSocket);
		return Result::success;
	}

	virtual Result sockRecv(const IOReadBuffer* buffers,
							Uptr numBuffers,
							bool peek,
							bool waitAll,
							Uptr* outNumBytesRead,
							bool* outDataTruncated,
							SocketAddress* outSourceAddress) override
	{
		if(outNumBytesRead) { *outNumBytesRead = 0; }
		if(outDataTruncated) { *outDataTruncated = false; }

		if(numBuffers == 0) { return Result::success; }
#ifdef IOV_MAX
		else if(numBuffers > IOV_MAX) { return Result::tooManyBuffers; }
#endif

		struct sockaddr_storage sourceAddr;
		struct msghdr message;
		memset(&message, 0, sizeof(message));
		message.msg_iov = (struct iovec*)buffers;
		message.msg_iovlen = numBuffers;
		if(outSourceAddress)
		{
			message.msg_name = &sourceAddr;
			message.msg_namelen = sizeof(sourceAddr);
		}

		I32 recvFlags = MSG_NOSIGNAL;
		if(peek) { recvFlags |= MSG_PEEK; }
		if(waitAll) { recvFlags |= MSG_WAITALL; }

		const ssize_t result = recvmsg(fd, &message, recvFlags);
		if(result == -1) { return asSocketVFSResult(errno); }

		if(outNumBytesRead) { *outNumBytesRead = Uptr(result); }
		if(outDataTruncated) { *outDataTruncated = (message.msg_flags & MSG_TRUNC) != 0; }
		if(outSourceAddress)
		{
			if(message.msg_namelen >= sizeof(struct sockaddr_in)
			   && (sourceAddr.ss_family == AF_INET || sourceAddr.ss_family == AF_INET6))
			{
				*outSourceAddress
					= asSocketAddress((struct sockaddr*)&sourceAddr, message.msg_namelen);
			}
			else { memset(outSourceAddress, 0, sizeof(*outSourceAddress)); }
		}
		return Result::success;
	}

	virtual Result sockSend(const IOWriteBuffer* buffers,
							Uptr numBuffers,
							const SocketAddress* destAddress,
							Uptr* outNumBytesWritten) override
	{
		if(outNumBytesWritten) { *outNumBytesWritten = 0; }

		if(numBuffers == 0) { return Result::success; }
#ifdef IOV_MAX
		else if(numBuffers > IOV_MAX) { return Result::tooManyBuffers; }
#endif

		struct sockaddr_storage destAddr;
		socklen_t destAddrLen = 0;
		if(destAddress)
		{
			if(!asSockAddr(*destAddress, destAddr, destAddrLen)) { return Result::notSupported; }
		}

		struct msghdr message;
		memset(&message, 0, sizeof(message));
		message.msg_iov = (struct iovec*)buffers;
		message.msg_iovlen = numBuffers;
		if(destAddress)
		{
			message.msg_name = &destAddr;
			message.msg_namelen = destAddrLen;
		}

		const ssize_t result = sendmsg(fd, &message, MSG_NOSIGNAL);
		if(result == -1) { return asSocketVFSResult(errno); }

		if(outNumBytesWritten) { *outNumBytesWritten = Uptr(result); }
		return Result::success;
	}

	virtual Result sockShutdown(bool shutRead, bool shutWrite) override
	{
		if(!shutRead && !shutWrite) { return Result::notPermitted; }
		const I32 how = shutRead && shutWrite ? SHUT_RDWR : shutRead ? SHUT_RD : SHUT_WR;
		return shutdown(fd, how) == 0 ? Result::success : asSocketVFSResult(errno);
	}

	virtual Result sockBind(const SocketAddress& localAddress) override
	{
		struct sockaddr_storage bindAddr;
		socklen_t bindAddrLen = 0;
		if(!asSockAddr(localAddress, bindAddr, bindAddrLen)) { return Result::notSupported; }

		if(bind(fd, (struct sockaddr*)&bindAddr, bindAddrLen) != 0)
		{
			return asSocketVFSResult(errno);
		}
		return Result::success;
	}

	virtual Result sockListen(U32 backlog) override
	{
		return listen(fd, I32(backlog)) == 0 ? Result::success : asSocketVFSResult(errno);
	}

	virtual Result sockConnect(const SocketAddress& remoteAddress) override
	{
		struct sockaddr_storage connectAddr;
		socklen_t connectAddrLen = 0;
		if(!asSockAddr(remoteAddress, connectAddr, connectAddrLen))
		{
			return Result::notSupported;
		}

		if(connect(fd, (struct sockaddr*)&connectAddr, connectAddrLen) != 0)
		{
			return asSocketVFSResult(errno);
		}
		return Result::success;
	}

	virtual Result sockGetLocalAddress(SocketAddress& outAddress) override
	{
		struct sockaddr_storage addr;
		socklen_t addrLen = sizeof(addr);
		if(getsockname(fd, (struct sockaddr*)&addr, &addrLen) != 0)
		{
			return asSocketVFSResult(errno);
		}
		if(addr.ss_family != AF_INET && addr.ss_family != AF_INET6)
		{
			return Result::notSupported;
		}
		outAddress = asSocketAddress((struct sockaddr*)&addr, addrLen);
		return Result::success;
	}

	virtual Result sockGetPeerAddress(SocketAddress& outAddress) override
	{
		struct sockaddr_storage addr;
		socklen_t addrLen = sizeof(addr);
		if(getpeername(fd, (struct sockaddr*)&addr, &addrLen) != 0)
		{
			return asSocketVFSResult(errno);
		}
		if(addr.ss_family != AF_INET && addr.ss_family != AF_INET6)
		{
			return Result::notSupported;
		}
		outAddress = asSocketAddress((struct sockaddr*)&addr, addrLen);
		return Result::success;
	}

	virtual Result sockSetOpt(SocketOptionLevel level, SocketOption option, U32 value) override
	{
		I32 hostLevel = 0;
		I32 hostOption = 0;
		if(!translateSocketOption(level, option, hostLevel, hostOption))
		{
			return Result::notSupported;
		}
		if(option == SocketOption::type || option == SocketOption::error)
		{
			return Result::notPermitted;
		}

		const int hostValue = int(value);
		return setsockopt(fd, hostLevel, hostOption, &hostValue, sizeof(hostValue)) == 0
				   ? Result::success
				   : asSocketVFSResult(errno);
	}

	virtual Result sockGetOpt(SocketOptionLevel level, SocketOption option, U32& outValue) override
	{
		I32 hostLevel = 0;
		I32 hostOption = 0;
		if(!translateSocketOption(level, option, hostLevel, hostOption))
		{
			return Result::notSupported;
		}

		int hostValue = 0;
		socklen_t hostValueLen = sizeof(hostValue);
		if(getsockopt(fd, hostLevel, hostOption, &hostValue, &hostValueLen) != 0)
		{
			return asSocketVFSResult(errno);
		}
		if(option == SocketOption::type)
		{
			// Translate the host's SOCK_* value to the corresponding VFS::FileType value.
			if(hostValue == SOCK_STREAM) { outValue = U32(FileType::streamSocket); }
			else if(hostValue == SOCK_DGRAM) { outValue = U32(FileType::datagramSocket); }
			else { outValue = U32(FileType::unknown); }
			return Result::success;
		}
		outValue = U32(hostValue);
		return Result::success;
	}

private:
	static bool translateSocketOption(SocketOptionLevel level,
									  SocketOption option,
									  I32& outHostLevel,
									  I32& outHostOption)
	{
		switch(level)
		{
		case SocketOptionLevel::socket:
			outHostLevel = SOL_SOCKET;
			switch(option)
			{
			case SocketOption::reuseAddress: outHostOption = SO_REUSEADDR; return true;
#ifdef SO_BROADCAST
			case SocketOption::broadcast: outHostOption = SO_BROADCAST; return true;
#endif
			case SocketOption::keepAlive: outHostOption = SO_KEEPALIVE; return true;
			case SocketOption::type: outHostOption = SO_TYPE; return true;
			case SocketOption::error: outHostOption = SO_ERROR; return true;
			case SocketOption::sendBufferSize: outHostOption = SO_SNDBUF; return true;
			case SocketOption::recvBufferSize: outHostOption = SO_RCVBUF; return true;
			default: return false;
			}
		case SocketOptionLevel::tcp:
			outHostLevel = IPPROTO_TCP;
			switch(option)
			{
			case SocketOption::noDelay: outHostOption = TCP_NODELAY; return true;
			default: return false;
			}
		case SocketOptionLevel::ipv6:
			outHostLevel = IPPROTO_IPV6;
			switch(option)
			{
			case SocketOption::v6Only:
#ifdef IPV6_V6ONLY
				outHostOption = IPV6_V6ONLY;
				return true;
#else
				return false;
#endif
			default: return false;
			}
		default: return false;
		}
	}
};

Result Platform::createSocket(SocketAddress::Family family, SocketType type, VFD*& outVFD)
{
	outVFD = nullptr;

	I32 hostFamily = 0;
	switch(family)
	{
	case SocketAddress::Family::ipv4: hostFamily = AF_INET; break;
	case SocketAddress::Family::ipv6: hostFamily = AF_INET6; break;
	default: return Result::notSupported;
	};

	I32 hostType = 0;
	FileType fileType = FileType::unknown;
	switch(type)
	{
	case SocketType::stream:
		hostType = SOCK_STREAM;
		fileType = FileType::streamSocket;
		break;
	case SocketType::datagram:
		hostType = SOCK_DGRAM;
		fileType = FileType::datagramSocket;
		break;
	default: return Result::notSupported;
	};

#ifdef SOCK_CLOEXEC
	const I32 fd = socket(hostFamily, hostType | SOCK_CLOEXEC, 0);
#else
	const I32 fd = socket(hostFamily, hostType, 0);
#endif
	if(fd < 0) { return asSocketVFSResult(errno); }

#ifndef SOCK_CLOEXEC
	if(fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
	{
		const int fcntlError = errno;
		::close(fd);
		return asSocketVFSResult(fcntlError);
	}
#endif

	outVFD = new POSIXSocketVFD(fd, fileType);
	return Result::success;
}

// Parses "[host:]port", "[ipv6host]:port", or a bare "port" into host and service strings.
// An empty host means the wildcard address.
static bool parseSocketAddress(const std::string& address,
							   std::string& outHost,
							   std::string& outService)
{
	std::string hostPort = address;

	if(!hostPort.empty() && hostPort.front() == '[')
	{
		// "[ipv6host]:port" or "[ipv6host]".
		const Uptr closeBracket = hostPort.find(']');
		if(closeBracket == std::string::npos) { return false; }

		outHost = hostPort.substr(1, closeBracket - 1);
		if(closeBracket + 1 >= hostPort.size()) { return false; }
		if(hostPort[closeBracket + 1] != ':') { return false; }
		outService = hostPort.substr(closeBracket + 2);
	}
	else
	{
		const Uptr colon = hostPort.rfind(':');
		if(colon == std::string::npos)
		{
			// Bare port.
			outHost.clear();
			outService = hostPort;
		}
		else
		{
			outHost = hostPort.substr(0, colon);
			outService = hostPort.substr(colon + 1);
		}
	}

	return !outService.empty();
}

// Resolves a "[host:]port" address string to a list of SocketAddresses via getaddrinfo.
static Result resolveSocketAddress(const std::string& address,
								   bool passive,
								   std::vector<SocketAddress>& outAddresses)
{
	std::string host;
	std::string service;
	if(!parseSocketAddress(address, host, service)) { return Result::notSupported; }

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if(passive && host.empty()) { hints.ai_flags = AI_PASSIVE; }

	struct addrinfo* addrInfoList = nullptr;
	const I32 gaiResult
		= getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &addrInfoList);
	if(gaiResult != 0 || !addrInfoList)
	{
		if(addrInfoList) { freeaddrinfo(addrInfoList); }
		switch(gaiResult)
		{
		case EAI_NONAME: return Result::doesNotExist;
		case EAI_AGAIN: return Result::interruptedBySignal;
		case EAI_MEMORY: return Result::outOfMemory;
		case EAI_ADDRFAMILY:
		case EAI_SOCKTYPE:
		case EAI_SERVICE: return Result::notSupported;
		default: return Result::notSupported;
		};
	}

	for(struct addrinfo* addrInfo = addrInfoList; addrInfo; addrInfo = addrInfo->ai_next)
	{
		if(addrInfo->ai_family == AF_INET || addrInfo->ai_family == AF_INET6)
		{
			outAddresses.push_back(
				asSocketAddress(addrInfo->ai_addr, socklen_t(addrInfo->ai_addrlen)));
		}
	}

	freeaddrinfo(addrInfoList);
	return outAddresses.empty() ? Result::doesNotExist : Result::success;
}

Result Platform::createListenSocket(const std::string& address, VFD*& outVFD, U32 backlog)
{
	outVFD = nullptr;

	std::vector<SocketAddress> addresses;
	Result result = resolveSocketAddress(address, true, addresses);
	if(result != Result::success) { return result; }

	for(const SocketAddress& bindAddress : addresses)
	{
		VFD* listenVFD = nullptr;
		result = createSocket(bindAddress.family, SocketType::stream, listenVFD);
		if(result != Result::success) { continue; }

		// Set SO_REUSEADDR so a listener can rebind a port that has connections in TIME_WAIT.
		listenVFD->sockSetOpt(SocketOptionLevel::socket, SocketOption::reuseAddress, 1);

		result = listenVFD->sockBind(bindAddress);
		if(result == Result::success) { result = listenVFD->sockListen(backlog); }
		if(result == Result::success)
		{
			outVFD = listenVFD;
			return Result::success;
		}

		listenVFD->close();
	}

	return result;
}

Result Platform::createConnectedSocket(const std::string& address, VFD*& outVFD)
{
	outVFD = nullptr;

	std::vector<SocketAddress> addresses;
	Result result = resolveSocketAddress(address, false, addresses);
	if(result != Result::success) { return result; }

	for(const SocketAddress& remoteAddress : addresses)
	{
		VFD* connectVFD = nullptr;
		result = createSocket(remoteAddress.family, SocketType::stream, connectVFD);
		if(result != Result::success) { continue; }

		result = connectVFD->sockConnect(remoteAddress);
		if(result == Result::success)
		{
			outVFD = connectVFD;
			return Result::success;
		}

		connectVFD->close();
	}

	return result;
}
