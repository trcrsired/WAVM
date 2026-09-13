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

	// Creates a TCP socket bound to and listening on an address, and returns a VFD for it.
	// The address may be "[host:]port", "[ipv6host]:port", or a bare "port" (listens on the
	// wildcard address). Host may be a name or a numeric address.
	WAVM_API VFS::Result createListenSocket(const std::string& address,
											VFS::VFD*& outVFD,
											U32 backlog = 128);

	// Creates a TCP socket connected to the given "host:port" or "[ipv6host]:port" address,
	// and returns a VFD for it.
	WAVM_API VFS::Result createConnectedSocket(const std::string& address, VFS::VFD*& outVFD);
}}
