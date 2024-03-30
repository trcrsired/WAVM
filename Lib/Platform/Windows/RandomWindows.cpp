#include <cstddef>
#include <cstdint>
#include <limits>
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Platform/Random.h"

using namespace WAVM;
using namespace WAVM::Platform;

#if defined(_MSC_VER)
#pragma comment(lib, "advapi32.lib")
#endif

extern "C" __declspec(dllimport) int SystemFunction036(void*, ::std::uint_least32_t) noexcept;
/*
copied from fast_io library
*/
static inline U8* rtl_gen_random_some_impl(U8* first, U8* last) noexcept
{
	constexpr ::std::size_t uintleast32mx{
		static_cast<::std::size_t>(::std::numeric_limits<::std::uint_least32_t>::max())};
	while(first != last)
	{
		::std::size_t toreadthisround{static_cast<::std::size_t>(last - first)};
		if(uintleast32mx < toreadthisround) { toreadthisround = uintleast32mx; }
		if(!SystemFunction036(first, static_cast<::std::uint_least32_t>(toreadthisround)))
		{
			return first;
		}
		first += toreadthisround;
	}
	return last;
}

void Platform::getCryptographicRNG(U8* outRandomBytes, Uptr numBytes)
{
	auto last{outRandomBytes + numBytes};
	WAVM_ERROR_UNLESS(rtl_gen_random_some_impl(outRandomBytes, last) == last);
}
