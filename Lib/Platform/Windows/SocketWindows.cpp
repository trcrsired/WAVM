#include <string>
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Platform/Socket.h"
#include "WAVM/VFS/VFS.h"

using namespace WAVM;
using namespace WAVM::Platform;
using namespace WAVM::VFS;

// Socket support is not yet implemented on Windows.

Result Platform::createSocket(SocketAddress::Family family, SocketType type, VFD*& outVFD)
{
	outVFD = nullptr;
	return Result::notSupported;
}

Result Platform::createSocketPair(SocketType type, VFD*& outVFD0, VFD*& outVFD1)
{
	outVFD0 = nullptr;
	outVFD1 = nullptr;
	return Result::notSupported;
}

Result Platform::resolveAddress(const std::string& hostName,
								U16 port,
								bool allowIPv4,
								bool allowIPv6,
								VFS::SocketAddress* outAddresses,
								Uptr* inOutNumAddresses)
{
	return Result::notSupported;
}
