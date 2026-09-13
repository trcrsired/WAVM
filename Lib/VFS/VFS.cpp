#include "WAVM/VFS/VFS.h"
#include "WAVM/Inline/Errors.h"

using namespace WAVM;
using namespace WAVM::VFS;

const char* VFS::describeResult(Result result)
{
	switch(result)
	{
		// clang-format off
#define V(name, description) case VFS::Result::name: return description;
WAVM_ENUM_VFS_RESULTS(V)
#undef V
		// clang-format on

	default: WAVM_UNREACHABLE();
	};
}

Result VFD::sockAccept(VFD*& outVFD,
					   const VFDFlags& acceptedFlags,
					   SocketAddress* outPeerAddress)
{
	return Result::notSupported;
}

Result VFD::sockRecv(const IOReadBuffer* buffers,
					 Uptr numBuffers,
					 bool peek,
					 bool waitAll,
					 bool dontWait,
					 Uptr* outNumBytesRead,
					 bool* outDataTruncated,
					 SocketAddress* outSourceAddress)
{
	return Result::notSupported;
}

Result VFD::sockSend(const IOWriteBuffer* buffers,
					 Uptr numBuffers,
					 const SocketAddress* destAddress,
					 bool dontWait,
					 Uptr* outNumBytesWritten)
{
	return Result::notSupported;
}

Result VFD::sockShutdown(bool shutRead, bool shutWrite) { return Result::notSupported; }

Result VFD::sockBind(const SocketAddress& localAddress) { return Result::notSupported; }

Result VFD::sockListen(U32 backlog) { return Result::notSupported; }

Result VFD::sockConnect(const SocketAddress& remoteAddress) { return Result::notSupported; }

Result VFD::sockGetLocalAddress(SocketAddress& outAddress) { return Result::notSupported; }

Result VFD::sockGetPeerAddress(SocketAddress& outAddress) { return Result::notSupported; }

Result VFD::sockGetStatus(SocketStatus& outStatus) { return Result::notSupported; }

Result VFD::sockSetOpt(SocketOption option, U64 value) { return Result::notSupported; }

Result VFD::sockGetOpt(SocketOption option, U64& outValue) { return Result::notSupported; }

Result VFD::sockJoinMulticastV4(const U8* group, const U8* interfaceAddr)
{
	return Result::notSupported;
}

Result VFD::sockLeaveMulticastV4(const U8* group, const U8* interfaceAddr)
{
	return Result::notSupported;
}

Result VFD::sockJoinMulticastV6(const U8* group, U32 interfaceIndex)
{
	return Result::notSupported;
}

Result VFD::sockLeaveMulticastV6(const U8* group, U32 interfaceIndex)
{
	return Result::notSupported;
}
