#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// winsock2.h must come before windows.h to suppress the legacy winsock.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#undef min
#undef max

#include <memory>
#include <mutex>
#include <string>
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Platform/Socket.h"
#include "WAVM/VFS/VFS.h"

using namespace WAVM;
using namespace WAVM::Platform;
using namespace WAVM::VFS;

// Unlike POSIX, Windows sockets are not file handles: fd_read/fd_write on a socket fd
// reach VFD::readv/writev, which must go through WSARecv/WSASend rather than
// ReadFile/WriteFile.

static void ensureWSAStartup()
{
	static std::once_flag once;
	std::call_once(once, [] {
		WSADATA wsaData;
		if(WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		{
			Errors::fatalf("WSAStartup failed: %d", WSAGetLastError());
		}
	});
}

static Result asSocketVFSResult(int wsaError)
{
	switch(wsaError)
	{
	case WSAEINTR: return Result::interruptedBySignal;
	case WSAEWOULDBLOCK: return Result::wouldBlock;
	case WSA_IO_PENDING: return Result::ioPending;
	case WSAEACCES: return Result::notAccessible;
	case WSAEFAULT: return Result::inaccessibleBuffer;
	case WSAEMFILE: return Result::outOfProcessFDs;
	case WSAENOBUFS: return Result::outOfMemory;
	case WSAENOTSOCK: return Result::notSocket;
	case WSAENOTCONN: return Result::notConnected;
	case WSAECONNREFUSED: return Result::connectionRefused;
	case WSAECONNRESET: return Result::connectionReset;
	case WSAECONNABORTED: return Result::connectionAborted;
	case WSAESHUTDOWN: return Result::notConnected;
	case WSAETIMEDOUT: return Result::timedOut;
	case WSAEADDRINUSE: return Result::addressInUse;
	case WSAEADDRNOTAVAIL: return Result::addressNotAvailable;
	case WSAEHOSTUNREACH:
	case WSAEHOSTDOWN: return Result::hostUnreachable;
	case WSAENETUNREACH:
	case WSAENETDOWN:
	case WSAENETRESET: return Result::networkUnreachable;
	case WSAEISCONN: return Result::notPermitted;
	case WSAEALREADY:
	case WSAEINPROGRESS: return Result::ioPending;
	case WSAEDESTADDRREQ: return Result::notConnected;
	case WSAEMSGSIZE: return Result::tooManyBufferBytes;
	case WSAEOPNOTSUPP:
	case WSAEAFNOSUPPORT:
	case WSAEPFNOSUPPORT:
	case WSAEPROTONOSUPPORT:
	case WSAEPROTOTYPE:
	case WSAENOPROTOOPT:
	case WSAESOCKTNOSUPPORT: return Result::notSupported;
	case WSAEINVAL:
	case WSAEDISCON:
	case WSATYPE_NOT_FOUND:
	case WSASYSNOTREADY:
	case WSANOTINITIALISED:
	case WSAVERNOTSUPPORTED:
	default: return Result::notPermitted;
	};
}

static SocketAddress asSocketAddress(const struct sockaddr* addr, int addrLen)
{
	SocketAddress result;
	memset(&result, 0, sizeof(result));

	if(addr->sa_family == AF_INET && addrLen >= (int)sizeof(struct sockaddr_in))
	{
		const struct sockaddr_in* addr4 = (const struct sockaddr_in*)addr;
		result.family = SocketAddress::Family::ipv4;
		result.port = ntohs(addr4->sin_port);
		result.scopeId = 0;
		memcpy(result.ipBytes, &addr4->sin_addr, 4);
	}
	else if(addr->sa_family == AF_INET6 && addrLen >= (int)sizeof(struct sockaddr_in6))
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
					   int& outAddrLen)
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

// WinSock has no iovec; marshal the VFS buffers into a stack WSABUF array.
// WSABUF is {u_long len; char* buf} so the VFS buffers can't be aliased.
static constexpr Uptr maxStackBuffers = 64;
static Uptr marshalBuffers(const IOReadBuffer* buffers, Uptr numBuffers, WSABUF* outWSABufs)
{
	const Uptr count = numBuffers < maxStackBuffers ? numBuffers : maxStackBuffers;
	for(Uptr i = 0; i < count; ++i)
	{
		outWSABufs[i].len = ULONG(buffers[i].numBytes > ULONG_MAX ? ULONG_MAX
																: buffers[i].numBytes);
		outWSABufs[i].buf = (CHAR*)buffers[i].data;
	}
	return count;
}
static Uptr marshalBuffers(const IOWriteBuffer* buffers, Uptr numBuffers, WSABUF* outWSABufs)
{
	const Uptr count = numBuffers < maxStackBuffers ? numBuffers : maxStackBuffers;
	for(Uptr i = 0; i < count; ++i)
	{
		outWSABufs[i].len = ULONG(buffers[i].numBytes > ULONG_MAX ? ULONG_MAX
																: buffers[i].numBytes);
		outWSABufs[i].buf = (CHAR*)buffers[i].data;
	}
	return count;
}

struct WindowsSocketVFD : VFD
{
	const SOCKET sock;
	const FileType fileType;
	SocketStatus status;
	bool isListening;

	WindowsSocketVFD(SOCKET inSock,
					 FileType inFileType,
					 SocketStatus inStatus = SocketStatus::opening)
	: sock(inSock), fileType(inFileType), status(inStatus), isListening(false)
	{
		WAVM_ASSERT(fileType == FileType::streamSocket || fileType == FileType::datagramSocket);
	}

	virtual Result close() override
	{
		closesocket(sock);
		status = SocketStatus::closed;
		delete this;
		return Result::success;
	}

	virtual Result seek(I64 offset, SeekOrigin origin, U64* outAbsoluteOffset = nullptr) override
	{
		return Result::notSeekable;
	}

	// fd_read on a socket: WSARecv with scatter buffers.
	virtual Result readv(const IOReadBuffer* buffers,
						 Uptr numBuffers,
						 Uptr* outNumBytesRead = nullptr,
						 const U64* offset = nullptr) override
	{
		if(offset != nullptr) { return Result::notSeekable; }
		if(outNumBytesRead) { *outNumBytesRead = 0; }
		if(numBuffers == 0) { return Result::success; }

		WSABUF wsaBufs[maxStackBuffers];
		const Uptr count = marshalBuffers(buffers, numBuffers, wsaBufs);

		DWORD numReceived = 0;
		DWORD flags = 0;
		if(WSARecv(sock, wsaBufs, DWORD(count), &numReceived, &flags, nullptr, nullptr)
		   == SOCKET_ERROR)
		{
			return asSocketVFSResult(WSAGetLastError());
		}
		if(outNumBytesRead) { *outNumBytesRead = numReceived; }
		return Result::success;
	}

	// fd_write on a socket: WSASend with gather buffers.
	virtual Result writev(const IOWriteBuffer* buffers,
						  Uptr numBuffers,
						  Uptr* outNumBytesWritten = nullptr,
						  const U64* offset = nullptr) override
	{
		if(offset != nullptr) { return Result::notSeekable; }
		if(outNumBytesWritten) { *outNumBytesWritten = 0; }
		if(numBuffers == 0) { return Result::success; }

		WSABUF wsaBufs[maxStackBuffers];
		const Uptr count = marshalBuffers(buffers, numBuffers, wsaBufs);

		DWORD numSent = 0;
		if(WSASend(sock, wsaBufs, DWORD(count), &numSent, 0, nullptr, nullptr)
		   == SOCKET_ERROR)
		{
			return asSocketVFSResult(WSAGetLastError());
		}
		if(outNumBytesWritten) { *outNumBytesWritten = numSent; }
		return Result::success;
	}

	virtual Result sync(SyncType syncType) override { return Result::notSynchronizable; }

	virtual Result getVFDInfo(VFDInfo& outInfo) override
	{
		outInfo.type = fileType;
		outInfo.flags.append = false;
		outInfo.flags.syncLevel = VFDSync::none;
		outInfo.flags.nonBlocking = isNonBlocking();
		return Result::success;
	}

	virtual Result getFileInfo(FileInfo& outInfo) override
	{
		// Windows sockets are not files; there is no fstat equivalent.
		memset(&outInfo, 0, sizeof(outInfo));
		outInfo.type = fileType;
		outInfo.numLinks = 1;
		return Result::success;
	}

	virtual Result setVFDFlags(const VFDFlags& vfsFlags) override
	{
		if(vfsFlags.append || vfsFlags.syncLevel != VFDSync::none)
		{
			return Result::notSupported;
		}
		u_long nonBlockingFlag = vfsFlags.nonBlocking ? 1 : 0;
		if(ioctlsocket(sock, FIONBIO, &nonBlockingFlag) != 0)
		{
			return asSocketVFSResult(WSAGetLastError());
		}
		nonBlocking = vfsFlags.nonBlocking;
		return Result::success;
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

	virtual Result sockAccept(VFD*& outVFD,
							  const VFDFlags& acceptedFlags,
							  SocketAddress* outPeerAddress = nullptr) override
	{
		outVFD = nullptr;
		if(outPeerAddress) { memset(outPeerAddress, 0, sizeof(*outPeerAddress)); }

		struct sockaddr_storage peerAddr;
		int peerAddrLen = sizeof(peerAddr);
		const SOCKET connectionSock
			= accept(sock, (struct sockaddr*)&peerAddr, &peerAddrLen);
		if(connectionSock == INVALID_SOCKET) { return asSocketVFSResult(WSAGetLastError()); }

		// Make the accepted socket non-inheritable and apply the requested blocking mode.
		SetHandleInformation((HANDLE)connectionSock, HANDLE_FLAG_INHERIT, 0);
		u_long nonBlockingFlag = acceptedFlags.nonBlocking ? 1 : 0;
		if(ioctlsocket(connectionSock, FIONBIO, &nonBlockingFlag) != 0)
		{
			const int ioctlError = WSAGetLastError();
			closesocket(connectionSock);
			return asSocketVFSResult(ioctlError);
		}

		if(outPeerAddress
		   && (peerAddr.ss_family == AF_INET || peerAddr.ss_family == AF_INET6))
		{
			*outPeerAddress = asSocketAddress((struct sockaddr*)&peerAddr, peerAddrLen);
		}

		std::unique_ptr<WindowsSocketVFD> acceptedVFD = std::make_unique<WindowsSocketVFD>(
			connectionSock, FileType::streamSocket, SocketStatus::opened);
		acceptedVFD->nonBlocking = acceptedFlags.nonBlocking;
		outVFD = acceptedVFD.release();
		return Result::success;
	}

	virtual Result sockRecv(const IOReadBuffer* buffers,
							Uptr numBuffers,
							bool peek,
							bool waitAll,
							bool dontWait,
							Uptr* outNumBytesRead,
							bool* outDataTruncated,
							SocketAddress* outSourceAddress) override
	{
		if(outNumBytesRead) { *outNumBytesRead = 0; }
		if(outDataTruncated) { *outDataTruncated = false; }
		if(numBuffers == 0) { return Result::success; }

		WSABUF wsaBufs[maxStackBuffers];
		const Uptr count = marshalBuffers(buffers, numBuffers, wsaBufs);

		struct sockaddr_storage sourceAddr;
		int sourceAddrLen = sizeof(sourceAddr);
		DWORD flags = 0;
		if(peek) { flags |= MSG_PEEK; }
		if(waitAll) { flags |= MSG_WAITALL; }

		// WinSock has no MSG_DONTWAIT; emulate it by temporarily putting the socket
		// in non-blocking mode.
		u_long nonBlockingFlag = 1;
		if(dontWait && !nonBlocking && ioctlsocket(sock, FIONBIO, &nonBlockingFlag) != 0)
		{
			return asSocketVFSResult(WSAGetLastError());
		}

		DWORD numReceived = 0;
		const int result = WSARecvFrom(sock,
									   wsaBufs,
									   DWORD(count),
									   &numReceived,
									   &flags,
									   outSourceAddress ? (struct sockaddr*)&sourceAddr : nullptr,
									   outSourceAddress ? &sourceAddrLen : nullptr,
									   nullptr,
									   nullptr);
		if(dontWait && !nonBlocking)
		{
			u_long restoreBlocking = 0;
			ioctlsocket(sock, FIONBIO, &restoreBlocking);
		}

		bool truncated = false;
		if(result == SOCKET_ERROR)
		{
			const int wsaError = WSAGetLastError();
			// A truncated datagram reports WSAEMSGSIZE with numReceived set to the
			// copied byte count; POSIX reports success with MSG_TRUNC in msg_flags
			// instead, which is what the WASI layer expects.
			if(wsaError == WSAEMSGSIZE) { truncated = true; }
			else { return asSocketVFSResult(wsaError); }
		}
		if(outDataTruncated) { *outDataTruncated = truncated; }
		if(outNumBytesRead) { *outNumBytesRead = numReceived; }

		if(outSourceAddress)
		{
			if(sourceAddrLen >= (int)sizeof(struct sockaddr_in)
			   && (sourceAddr.ss_family == AF_INET || sourceAddr.ss_family == AF_INET6))
			{
				*outSourceAddress
					= asSocketAddress((struct sockaddr*)&sourceAddr, sourceAddrLen);
			}
			else { memset(outSourceAddress, 0, sizeof(*outSourceAddress)); }
		}
		return Result::success;
	}

	virtual Result sockSend(const IOWriteBuffer* buffers,
							Uptr numBuffers,
							const SocketAddress* destAddress,
							bool dontWait,
							Uptr* outNumBytesWritten) override
	{
		if(outNumBytesWritten) { *outNumBytesWritten = 0; }
		if(numBuffers == 0) { return Result::success; }

		struct sockaddr_storage destAddr;
		int destAddrLen = 0;
		if(destAddress)
		{
			if(!asSockAddr(*destAddress, destAddr, destAddrLen))
			{
				return Result::notSupported;
			}
		}

		WSABUF wsaBufs[maxStackBuffers];
		const Uptr count = marshalBuffers(buffers, numBuffers, wsaBufs);

		u_long nonBlockingFlag = 1;
		if(dontWait && !nonBlocking && ioctlsocket(sock, FIONBIO, &nonBlockingFlag) != 0)
		{
			return asSocketVFSResult(WSAGetLastError());
		}

		DWORD numSent = 0;
		const int sendResult = WSASendTo(sock,
										 wsaBufs,
										 DWORD(count),
										 &numSent,
										 0,
										 destAddress ? (const struct sockaddr*)&destAddr
													 : nullptr,
										 destAddrLen,
										 nullptr,
										 nullptr);
		if(dontWait && !nonBlocking)
		{
			u_long restoreBlocking = 0;
			ioctlsocket(sock, FIONBIO, &restoreBlocking);
		}
		if(sendResult == SOCKET_ERROR)
		{
			return asSocketVFSResult(WSAGetLastError());
		}
		if(outNumBytesWritten) { *outNumBytesWritten = numSent; }
		return Result::success;
	}

	virtual Result sockShutdown(bool shutRead, bool shutWrite) override
	{
		if(!shutRead && !shutWrite) { return Result::notPermitted; }
		const int how = shutRead && shutWrite ? SD_BOTH : shutRead ? SD_RECEIVE : SD_SEND;
		return shutdown(sock, how) == 0 ? Result::success
										: asSocketVFSResult(WSAGetLastError());
	}

	virtual Result sockBind(const SocketAddress& localAddress) override
	{
		struct sockaddr_storage bindAddr;
		int bindAddrLen = 0;
		if(!asSockAddr(localAddress, bindAddr, bindAddrLen)) { return Result::notSupported; }

		if(bind(sock, (struct sockaddr*)&bindAddr, bindAddrLen) == SOCKET_ERROR)
		{
			return asSocketVFSResult(WSAGetLastError());
		}
		status = SocketStatus::opened;
		return Result::success;
	}

	virtual Result sockListen(U32 backlog) override
	{
		if(listen(sock, int(backlog)) == SOCKET_ERROR)
		{
			return asSocketVFSResult(WSAGetLastError());
		}
		status = SocketStatus::opened;
		isListening = true;
		return Result::success;
	}

	virtual Result sockConnect(const SocketAddress& remoteAddress) override
	{
		struct sockaddr_storage connectAddr;
		int connectAddrLen = 0;
		if(!asSockAddr(remoteAddress, connectAddr, connectAddrLen))
		{
			return Result::notSupported;
		}

		if(connect(sock, (struct sockaddr*)&connectAddr, connectAddrLen) == SOCKET_ERROR)
		{
			const Result result = asSocketVFSResult(WSAGetLastError());
			if(result != Result::ioPending && result != Result::wouldBlock)
			{
				status = SocketStatus::failed;
			}
			return result;
		}
		status = SocketStatus::opened;
		return Result::success;
	}

	virtual Result sockGetLocalAddress(SocketAddress& outAddress) override
	{
		struct sockaddr_storage addr;
		int addrLen = sizeof(addr);
		if(getsockname(sock, (struct sockaddr*)&addr, &addrLen) == SOCKET_ERROR)
		{
			return asSocketVFSResult(WSAGetLastError());
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
		int addrLen = sizeof(addr);
		if(getpeername(sock, (struct sockaddr*)&addr, &addrLen) == SOCKET_ERROR)
		{
			return asSocketVFSResult(WSAGetLastError());
		}
		if(addr.ss_family != AF_INET && addr.ss_family != AF_INET6)
		{
			return Result::notSupported;
		}
		outAddress = asSocketAddress((struct sockaddr*)&addr, addrLen);
		return Result::success;
	}

	virtual Result sockGetStatus(SocketStatus& outStatus) override
	{
		outStatus = status;
		return Result::success;
	}

	virtual Result sockSetOpt(SocketOption option, U64 value) override
	{
		switch(option)
		{
		case SocketOption::noop: return Result::success;

		// Get-only and unsupported options.
		case SocketOption::listening:
		case SocketOption::lastError:
		case SocketOption::type:
		case SocketOption::protocol:
		case SocketOption::promiscuous:
		case SocketOption::connectTimeout:
		case SocketOption::acceptTimeout: return Result::notPermitted;

		case SocketOption::linger:
		{
			struct linger hostLinger;
			hostLinger.l_onoff = u_short(value != 0);
			hostLinger.l_linger = u_short(value / 1000000000);
			return setsockopt(sock,
							  SOL_SOCKET,
							  SO_LINGER,
							  (const char*)&hostLinger,
							  sizeof(hostLinger))
						   == 0
					   ? Result::success
					   : asSocketVFSResult(WSAGetLastError());
		}
		case SocketOption::recvTimeout:
		case SocketOption::sendTimeout:
		{
			const int hostOption
				= option == SocketOption::recvTimeout ? SO_RCVTIMEO : SO_SNDTIMEO;
			// Windows takes a DWORD timeout in milliseconds, not a timeval.
			DWORD timeoutMs = DWORD(value / 1000000);
			return setsockopt(sock,
							  SOL_SOCKET,
							  hostOption,
							  (const char*)&timeoutMs,
							  sizeof(timeoutMs))
						   == 0
					   ? Result::success
					   : asSocketVFSResult(WSAGetLastError());
		}
		default:
		{
			int hostLevel = 0;
			int hostOption = 0;
			if(!translateSocketOption(option, hostLevel, hostOption))
			{
				return Result::notSupported;
			}
			const int hostValue = int(value);
			return setsockopt(sock,
							  hostLevel,
							  hostOption,
							  (const char*)&hostValue,
							  sizeof(hostValue))
						   == 0
					   ? Result::success
					   : asSocketVFSResult(WSAGetLastError());
		}
		}
	}

	virtual Result sockGetOpt(SocketOption option, U64& outValue) override
	{
		outValue = 0;

		switch(option)
		{
		case SocketOption::noop: return Result::success;
		case SocketOption::listening:
			outValue = isListening ? 1 : 0;
			return Result::success;
		case SocketOption::linger:
		{
			struct linger hostLinger;
			int hostLingerLen = sizeof(hostLinger);
			if(getsockopt(
				   sock, SOL_SOCKET, SO_LINGER, (char*)&hostLinger, &hostLingerLen)
			   != 0)
			{
				return asSocketVFSResult(WSAGetLastError());
			}
			outValue = hostLinger.l_onoff ? U64(hostLinger.l_linger) * 1000000000 : 0;
			return Result::success;
		}
		case SocketOption::recvTimeout:
		case SocketOption::sendTimeout:
		{
			const int hostOption
				= option == SocketOption::recvTimeout ? SO_RCVTIMEO : SO_SNDTIMEO;
			DWORD timeoutMs = 0;
			int timeoutLen = sizeof(timeoutMs);
			if(getsockopt(sock, SOL_SOCKET, hostOption, (char*)&timeoutMs, &timeoutLen)
			   != 0)
			{
				return asSocketVFSResult(WSAGetLastError());
			}
			outValue = U64(timeoutMs) * 1000000;
			return Result::success;
		}
		default:
		{
			int hostLevel = 0;
			int hostOption = 0;
			if(!translateSocketOption(option, hostLevel, hostOption))
			{
				return Result::notSupported;
			}

			int hostValue = 0;
			int hostValueLen = sizeof(hostValue);
			if(getsockopt(sock, hostLevel, hostOption, (char*)&hostValue, &hostValueLen)
			   != 0)
			{
				return asSocketVFSResult(WSAGetLastError());
			}
			if(option == SocketOption::type)
			{
				// Translate the host's SOCK_* value to the corresponding VFS::FileType
				// value.
				if(hostValue == SOCK_STREAM) { outValue = U64(FileType::streamSocket); }
				else if(hostValue == SOCK_DGRAM)
				{
					outValue = U64(FileType::datagramSocket);
				}
				else { outValue = U64(FileType::unknown); }
				return Result::success;
			}
			outValue = U64(U32(hostValue));
			return Result::success;
		}
		}
	}

	virtual Result sockJoinMulticastV4(const U8* group, const U8* interfaceAddr) override
	{
		return setMulticastV4(IP_ADD_MEMBERSHIP, group, interfaceAddr);
	}
	virtual Result sockLeaveMulticastV4(const U8* group, const U8* interfaceAddr) override
	{
		return setMulticastV4(IP_DROP_MEMBERSHIP, group, interfaceAddr);
	}
	virtual Result sockJoinMulticastV6(const U8* group, U32 interfaceIndex) override
	{
		return setMulticastV6(IPV6_ADD_MEMBERSHIP, group, interfaceIndex);
	}
	virtual Result sockLeaveMulticastV6(const U8* group, U32 interfaceIndex) override
	{
		return setMulticastV6(IPV6_DROP_MEMBERSHIP, group, interfaceIndex);
	}

private:
	// Tracks O_NONBLOCK ourselves: there is no getsockopt for the blocking mode and
	// ioctlsocket FIONBIO is write-only.
	bool nonBlocking = false;

	bool isNonBlocking() { return nonBlocking; }

	Result setMulticastV4(int hostOption, const U8* group, const U8* interfaceAddr)
	{
		struct ip_mreq mreq;
		memset(&mreq, 0, sizeof(mreq));
		memcpy(&mreq.imr_multiaddr, group, 4);
		memcpy(&mreq.imr_interface, interfaceAddr, 4);
		return setsockopt(sock, IPPROTO_IP, hostOption, (const char*)&mreq, sizeof(mreq))
					   == 0
				   ? Result::success
				   : asSocketVFSResult(WSAGetLastError());
	}
	Result setMulticastV6(int hostOption, const U8* group, U32 interfaceIndex)
	{
		struct ipv6_mreq mreq;
		memset(&mreq, 0, sizeof(mreq));
		memcpy(&mreq.ipv6mr_multiaddr, group, 16);
		mreq.ipv6mr_interface = interfaceIndex;
		return setsockopt(
				   sock, IPPROTO_IPV6, hostOption, (const char*)&mreq, sizeof(mreq))
					   == 0
				   ? Result::success
				   : asSocketVFSResult(WSAGetLastError());
	}

	static bool translateSocketOption(SocketOption option, int& outHostLevel, int& outHostOption)
	{
		outHostLevel = SOL_SOCKET;
		switch(option)
		{
		case SocketOption::reusePort:
#ifdef SO_REUSEPORT
			outHostOption = SO_REUSEPORT;
			return true;
#else
			return false;
#endif
		case SocketOption::reuseAddress: outHostOption = SO_REUSEADDR; return true;
		case SocketOption::noDelay:
			outHostLevel = IPPROTO_TCP;
			outHostOption = TCP_NODELAY;
			return true;
		case SocketOption::dontRoute: outHostOption = SO_DONTROUTE; return true;
		case SocketOption::v6Only:
			outHostLevel = IPPROTO_IPV6;
			outHostOption = IPV6_V6ONLY;
			return true;
		case SocketOption::broadcast: outHostOption = SO_BROADCAST; return true;
		case SocketOption::multicastLoopV4:
			outHostLevel = IPPROTO_IP;
			outHostOption = IP_MULTICAST_LOOP;
			return true;
		case SocketOption::multicastLoopV6:
			outHostLevel = IPPROTO_IPV6;
			outHostOption = IPV6_MULTICAST_LOOP;
			return true;
		case SocketOption::keepAlive: outHostOption = SO_KEEPALIVE; return true;
		case SocketOption::oobInline: outHostOption = SO_OOBINLINE; return true;
		case SocketOption::recvBufferSize: outHostOption = SO_RCVBUF; return true;
		case SocketOption::sendBufferSize: outHostOption = SO_SNDBUF; return true;
		case SocketOption::recvLowat: outHostOption = SO_RCVLOWAT; return true;
		case SocketOption::sendLowat: outHostOption = SO_SNDLOWAT; return true;
		case SocketOption::ttl:
			outHostLevel = IPPROTO_IP;
			outHostOption = IP_TTL;
			return true;
		case SocketOption::multicastTTLV4:
			outHostLevel = IPPROTO_IP;
			outHostOption = IP_MULTICAST_TTL;
			return true;
		case SocketOption::lastError: outHostOption = SO_ERROR; return true;
		case SocketOption::type: outHostOption = SO_TYPE; return true;
		case SocketOption::protocol:
#ifdef SO_PROTOCOL_INFOW
			// SO_PROTOCOL_INFO requires a WSAPROTOCOL_INFO output struct; report
			// the protocol stored at creation instead.
			return false;
#else
			return false;
#endif
		default: return false;
		}
	}
};

Result Platform::createSocket(SocketAddress::Family family, SocketType type, VFD*& outVFD)
{
	outVFD = nullptr;
	ensureWSAStartup();

	int hostFamily = 0;
	switch(family)
	{
	case SocketAddress::Family::ipv4: hostFamily = AF_INET; break;
	case SocketAddress::Family::ipv6: hostFamily = AF_INET6; break;
	default: return Result::notSupported;
	};

	int hostType = 0;
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

	const SOCKET sock = socket(hostFamily, hostType, 0);
	if(sock == INVALID_SOCKET) { return asSocketVFSResult(WSAGetLastError()); }

	// Windows sockets are created inheritable; make it non-inheritable.
	SetHandleInformation((HANDLE)sock, HANDLE_FLAG_INHERIT, 0);

	std::unique_ptr<WindowsSocketVFD> socketVFD = std::make_unique<WindowsSocketVFD>(sock, fileType);
	outVFD = socketVFD.release();
	return Result::success;
}

// WinSock has no socketpair(); emulate one with a loopback TCP connection for
// stream sockets. Datagram pairs are not emulated.
Result Platform::createSocketPair(SocketType type, VFD*& outVFD0, VFD*& outVFD1)
{
	outVFD0 = nullptr;
	outVFD1 = nullptr;
	ensureWSAStartup();

	if(type != SocketType::stream) { return Result::notSupported; }

	// Listen on a loopback ephemeral port.
	const SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
	if(listenSock == INVALID_SOCKET) { return asSocketVFSResult(WSAGetLastError()); }

	struct sockaddr_in loopback;
	memset(&loopback, 0, sizeof(loopback));
	loopback.sin_family = AF_INET;
	loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	Result result = Result::success;
	if(bind(listenSock, (struct sockaddr*)&loopback, sizeof(loopback)) == SOCKET_ERROR
	   || listen(listenSock, 1) == SOCKET_ERROR)
	{
		result = asSocketVFSResult(WSAGetLastError());
		closesocket(listenSock);
		return result;
	}

	int loopbackLen = sizeof(loopback);
	if(getsockname(listenSock, (struct sockaddr*)&loopback, &loopbackLen)
	   == SOCKET_ERROR)
	{
		result = asSocketVFSResult(WSAGetLastError());
		closesocket(listenSock);
		return result;
	}

	const SOCKET sock0 = socket(AF_INET, SOCK_STREAM, 0);
	if(sock0 == INVALID_SOCKET)
	{
		result = asSocketVFSResult(WSAGetLastError());
		closesocket(listenSock);
		return result;
	}

	if(connect(sock0, (struct sockaddr*)&loopback, loopbackLen) == SOCKET_ERROR)
	{
		result = asSocketVFSResult(WSAGetLastError());
		closesocket(sock0);
		closesocket(listenSock);
		return result;
	}

	const SOCKET sock1 = accept(listenSock, nullptr, nullptr);
	closesocket(listenSock);
	if(sock1 == INVALID_SOCKET)
	{
		result = asSocketVFSResult(WSAGetLastError());
		closesocket(sock0);
		return result;
	}

	SetHandleInformation((HANDLE)sock0, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation((HANDLE)sock1, HANDLE_FLAG_INHERIT, 0);

	std::unique_ptr<WindowsSocketVFD> vfd0 = std::make_unique<WindowsSocketVFD>(
		sock0, FileType::streamSocket, SocketStatus::opened);
	std::unique_ptr<WindowsSocketVFD> vfd1 = std::make_unique<WindowsSocketVFD>(
		sock1, FileType::streamSocket, SocketStatus::opened);
	outVFD0 = vfd0.release();
	outVFD1 = vfd1.release();
	return Result::success;
}

Result Platform::resolveAddress(const std::string& hostName,
								U16 port,
								bool allowIPv4,
								bool allowIPv6,
								SocketAddress* outAddresses,
								Uptr* inOutNumAddresses)
{
	WAVM_ASSERT(outAddresses && inOutNumAddresses);
	const Uptr capacity = *inOutNumAddresses;

	ensureWSAStartup();

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = allowIPv4 && allowIPv6 ? AF_UNSPEC : (allowIPv6 ? AF_INET6 : AF_INET);
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;

	char service[8];
	snprintf(service, sizeof(service), "%u", U32(port));

	struct addrinfo* addrInfoList = nullptr;
	const int gaiResult = getaddrinfo(hostName.c_str(), service, &hints, &addrInfoList);
	if(gaiResult != 0 || !addrInfoList)
	{
		if(addrInfoList) { freeaddrinfo(addrInfoList); }
		switch(gaiResult)
		{
		case EAI_NONAME: return Result::nameLookupFailed;
		case EAI_AGAIN: return Result::wouldBlock;
		case EAI_MEMORY: return Result::outOfMemory;
		case EAI_FAIL:
		case WSANO_DATA: return Result::nameLookupFailed;
		default: return Result::notSupported;
		};
	}

	Uptr numAddresses = 0;
	for(struct addrinfo* addrInfo = addrInfoList;
		addrInfo && numAddresses < capacity;
		addrInfo = addrInfo->ai_next)
	{
		const bool isIPv4 = addrInfo->ai_family == AF_INET;
		const bool isIPv6 = addrInfo->ai_family == AF_INET6;
		if((isIPv4 && allowIPv4) || (isIPv6 && allowIPv6))
		{
			outAddresses[numAddresses++]
				= asSocketAddress(addrInfo->ai_addr, int(addrInfo->ai_addrlen));
		}
	}

	freeaddrinfo(addrInfoList);
	*inOutNumAddresses = numAddresses;
	return numAddresses ? Result::success : Result::nameLookupFailed;
}
