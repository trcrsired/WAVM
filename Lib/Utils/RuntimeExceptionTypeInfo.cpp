#ifndef _MSC_VER
#include <cstdint>
#include <memory>
#include <typeinfo>

namespace WAVM::Runtime {

	struct ExceptionTypeTag
	{
		::std::uint_least64_t tag;
		::std::uint_least64_t ehptr;
	};

	::std::type_info const* getRttiFromExceptionTypeTag()
	{
		return std::addressof(typeid(::WAVM::Runtime::ExceptionTypeTag));
	}

}
#endif
