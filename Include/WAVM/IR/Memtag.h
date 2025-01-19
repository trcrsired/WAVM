#pragma once

namespace WAVM::LLVMJIT {
	enum class memtagStatus : char unsigned
	{
		none,
		basic,
		full,
		armmte,
		armmteirg
	};

	inline constexpr bool is_memtagstatus_armmte(::WAVM::LLVMJIT::memtagStatus status) noexcept
	{
		return status == ::WAVM::LLVMJIT::memtagStatus::armmte
			   || status == ::WAVM::LLVMJIT::memtagStatus::armmteirg;
	}
}
