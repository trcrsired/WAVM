#pragma once

#include "WAVM/Inline/BasicTypes.h"

namespace WAVM { namespace IR {
	inline constexpr U64 maxMemory32Pages = 65536;           // 2^16 pages -> 2^32 bytes
	inline constexpr U64 maxMemory64Pages = 281474976710656; // 2^48 pages -> 2^64 bytes
	inline constexpr U64 maxTable32Elems = UINT32_MAX;
	inline constexpr U64 maxTable64Elems = UINT64_MAX;
	inline constexpr Uptr numBytesPerPage = 65536;
	inline constexpr Uptr numBytesPerPageLog2 = 16;
	inline constexpr Uptr numBytesTaggedPerPage = numBytesPerPage / 16u;
	inline constexpr Uptr numBytesTaggedPerPageLog2 = 12;
	inline constexpr Uptr maxReturnValues = 16;
	inline constexpr U64 maxMemory64WASMBytes =
#if defined(WAVM_ENABLE_TSAN) && WAVM_ENABLE_TSAN
		(U64(8) * 1024 * 1024 * 1024); // 8GB
#else
		(U64(1) * 1024 * 1024 * 1024 * 1024); // 1TB
#endif
	inline constexpr U64 maxMemory64WASMPages = maxMemory64WASMBytes >> IR::numBytesPerPageLog2;
	inline constexpr U64 maxMemory64WASMMask = maxMemory64WASMBytes - 1u;
}}
