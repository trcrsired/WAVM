#pragma once

namespace WAVM::LLVMJIT {
	enum class memtagStatus : char unsigned
	{
		none,
		basic,
		full,
		armmte
	};
}
