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

Result Platform::createListenSocket(const std::string& address, VFD*& outVFD, U32 backlog)
{
	outVFD = nullptr;
	return Result::notSupported;
}

Result Platform::createConnectedSocket(const std::string& address, VFD*& outVFD)
{
	outVFD = nullptr;
	return Result::notSupported;
}
