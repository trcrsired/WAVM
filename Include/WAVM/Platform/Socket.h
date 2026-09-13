#pragma once

#include <string>
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Platform/Defines.h"
#include "WAVM/VFS/VFS.h"

namespace WAVM { namespace Platform {

	enum class SocketType
	{
		stream,
		datagram
	};

	// Creates an unbound, unconnected socket of the given family and type.
	WAVM_API VFS::Result createSocket(VFS::SocketAddress::Family family,
									  SocketType type,
									  VFS::VFD*& outVFD);

	// Creates a pair of unnamed connected sockets of the given type.
	WAVM_API VFS::Result createSocketPair(SocketType type, VFS::VFD*& outVFD0, VFS::VFD*& outVFD1);

	// Resolves a host name to IP socket addresses using the host's name resolution. Writes
	// up to *inOutNumAddresses addresses to outAddresses and sets *inOutNumAddresses to the
	// number written. The returned addresses have the given port (host byte order).
	WAVM_API VFS::Result resolveAddress(const std::string& hostName,
										U16 port,
										bool allowIPv4,
										bool allowIPv6,
										VFS::SocketAddress* outAddresses,
										Uptr* inOutNumAddresses);
}}
