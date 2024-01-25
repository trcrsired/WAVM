#pragma once
#include <cstdint>

namespace WAVM::Runtime {

	struct ExceptionTypeTag
	{
		::std::uint_least64_t tag;
		::std::uint_least64_t ehptr;
	};

}
