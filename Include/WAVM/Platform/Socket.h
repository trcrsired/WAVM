#pragma once

#include <string>
#include <vector>
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

	// One address assigned to a host network interface.
	struct InterfaceAddress
	{
		std::string name;
		VFS::SocketAddress::Family family;
		U8 ipBytes[16];      // In network byte order; only the first 4 bytes are used for IPv4.
		U8 netmaskBytes[16]; // In network byte order; all zero if !hasNetmask.
		bool hasNetmask;
		// POSIX IFF_* bits: IFF_UP=0x1, IFF_BROADCAST=0x2, IFF_LOOPBACK=0x8,
		// IFF_POINTOPOINT=0x10, IFF_RUNNING=0x40, IFF_MULTICAST=0x1000.
		U32 flags;
	};

	// Enumerates the host's network interfaces, one entry per interface address. POSIX uses
	// getifaddrs(3); Windows has no such call, so GetAdaptersAddresses is flattened and its
	// adapter/unicast records are translated to the same fields (prefix length to netmask,
	// oper status/IfType to IFF_* bits).
	WAVM_API VFS::Result getInterfaceAddresses(std::vector<InterfaceAddress>& outAddresses);

	// A descriptor to wait on, with the operations pollFDs is waiting for.
	struct PollFD
	{
		Uptr fd;
		bool waitRead;
		bool waitWrite;
		bool ready;
	};

	// Waits until any of the given descriptors is ready for the requested operation, or until
	// timeoutNanos elapses (or indefinitely if timeoutNanos < 0). Sets fds[i].ready to whether
	// fds[i] is ready. Returns the error from the underlying poll if it failed.
	WAVM_API VFS::Result pollFDs(PollFD* fds, Uptr numFDs, I64 timeoutNanos);
}}
