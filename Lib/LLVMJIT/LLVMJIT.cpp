#include "WAVM/LLVMJIT/LLVMJIT.h"
#include <unwind.h>
#include <cstdio>
#include <utility>
#include "LLVMJITPrivate.h"
#include "WAVM/IR/FeatureSpec.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Inline/HashMap.h"
PUSH_DISABLE_WARNINGS_FOR_LLVM_HEADERS
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
POP_DISABLE_WARNINGS_FOR_LLVM_HEADERS

namespace llvm {
	class Constant;
}

using namespace WAVM;
using namespace WAVM::IR;
using namespace WAVM::LLVMJIT;

#if defined(__x86__) || defined(_M_IX86) || defined(__i386__) || defined(_M_X64)
#define WAVM_IS_X86
#endif

namespace LLVMRuntimeSymbols {

	extern "C" void wavm_throw_wasm_ehtag(::std::uint_least64_t, ::std::uint_least64_t);
	extern "C" void wavm_memtag_trap_function();
	extern "C" void wavm_aarch64_mte_settag(void*, ::std::size_t) noexcept;
	extern "C" void wavm_aarch64_mte_settag_zero(void*, ::std::size_t) noexcept;

#if (defined(_WIN32) && !defined(__WINE__)) || defined(__CYGWIN__)
	// the LLVM X86 code generator calls __chkstk when allocating more than 4KB of stack space
#if defined(__MINGW32__) || defined(__CYGWIN__)
	extern "C" void ___chkstk_ms();
	extern "C" void __gxx_personality_seh0();
#else
	extern "C" void __chkstk();
	extern "C" void pseudo_gxx_personality_seh0(PEXCEPTION_RECORD ms_exc,
												void* this_frame,
												PCONTEXT ms_orig_context,
												PDISPATCHER_CONTEXT ms_disp)
	{
		return _GCC_specific_handler(
			ms_exc, this_frame, ms_orig_context, ms_disp, __gxx_personality_imp);
	}
#endif
#else
#if defined(__APPLE__)
	// LLVM's memset intrinsic lowers to calling __bzero on MacOS when writing a constant zero.
	extern "C" void __bzero();
#endif
#if defined(__i386__) || defined(__x86_64__)
	extern "C" void wavm_probe_stack();
#endif
	extern "C" int __gxx_personality_v0();
	extern "C" void* __cxa_begin_catch(void*) throw();
	extern "C" void __cxa_end_catch();
#endif

	static HashMap<std::string, void*> map = {
		{"memmove", (void*)&memmove},
		{"memset", (void*)&memset},
		{"_Unwind_Resume", (void*)&_Unwind_Resume},
		{"wavm_throw_wasm_ehtag", (void*)&wavm_throw_wasm_ehtag},
		{"wavm_memtag_trap_function", (void*)&wavm_memtag_trap_function},
#if defined(__aarch64__) && (!defined(_MSC_VER) || defined(__clang__))
		{"wavm_aarch64_mte_settag", (void*)&wavm_aarch64_mte_settag},
		{"wavm_aarch64_mte_settag_zero", (void*)&wavm_aarch64_mte_settag_zero},
#endif
#ifdef _WIN32
#ifdef __MINGW32__
#ifdef WAVM_IS_X86
		{"___chkstk_ms", (void*)&___chkstk_ms},
#endif
		{"__gxx_personality_seh0", (void*)&__gxx_personality_seh0},
#else
#ifdef WAVM_IS_X86
		{"__chkstk", (void*)&__chkstk},
#endif
		{"__CxxFrameHandler3", (void*)&__CxxFrameHandler3},
#endif
#else
#if defined(__APPLE__)
		{"__bzero", (void*)&__bzero},
#endif
#if defined(__i386__) || defined(__x86_64__)
		{"wavm_probe_stack", (void*)&wavm_probe_stack},
#endif
#ifdef __ELF__
		{"DW.ref.__gxx_personality_v0", (void*)&__gxx_personality_v0},
#endif
		{"__gxx_personality_v0", (void*)&__gxx_personality_v0},
		{"__cxa_begin_catch", (void*)&__cxa_begin_catch},
		{"__cxa_end_catch", (void*)&__cxa_end_catch},
#endif
	};
}

llvm::JITEvaluatedSymbol LLVMJIT::resolveJITImport(llvm::StringRef name)
{
	// Allow some intrinsics used by LLVM
	void** symbolValue = LLVMRuntimeSymbols::map.get(name.str());
	if(!symbolValue)
	{
		Errors::fatalf("LLVM generated code references unknown external symbol: %s",
					   name.str().c_str());
	}

	return llvm::JITEvaluatedSymbol(reinterpret_cast<Uptr>(*symbolValue),
									llvm::JITSymbolFlags::None);
}

static bool globalInitLLVM()
{
	llvm::InitializeAllTargetInfos();
	llvm::InitializeAllTargets();
	llvm::InitializeAllTargetMCs();
	llvm::InitializeAllAsmPrinters();
	llvm::InitializeAllAsmParsers();
	llvm::InitializeAllDisassemblers();
	llvm::InitializeNativeTarget();
	llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
	return true;
}

static void globalInitLLVMOnce()
{
	static bool isLLVMInitialized = globalInitLLVM();
	WAVM_ASSERT(isLLVMInitialized);
}

LLVMContext::LLVMContext()
{
	globalInitLLVMOnce();

	i8Type = llvm::Type::getInt8Ty(*this);
	i16Type = llvm::Type::getInt16Ty(*this);
	i32Type = llvm::Type::getInt32Ty(*this);
	i64Type = llvm::Type::getInt64Ty(*this);
	f32Type = llvm::Type::getFloatTy(*this);
	f64Type = llvm::Type::getDoubleTy(*this);
#if LLVM_VERSION_MAJOR > 14
	i8PtrType = llvm::PointerType::get(*this, 0);
	externrefType = i8PtrType;
#else
	i8PtrType = i8Type->getPointerTo();
	externrefType = llvm::StructType::create("Object", i8Type)->getPointerTo();
#endif

	i8x8Type = FixedVectorType::get(i8Type, 8);
	i16x4Type = FixedVectorType::get(i16Type, 4);
	i32x2Type = FixedVectorType::get(i32Type, 2);
	i64x1Type = FixedVectorType::get(i64Type, 1);
	f32x2Type = FixedVectorType::get(f32Type, 2);
	f64x1Type = FixedVectorType::get(f64Type, 1);

	i8x16Type = FixedVectorType::get(i8Type, 16);
	i16x8Type = FixedVectorType::get(i16Type, 8);
	i32x4Type = FixedVectorType::get(i32Type, 4);
	i64x2Type = FixedVectorType::get(i64Type, 2);
	f32x4Type = FixedVectorType::get(f32Type, 4);
	f64x2Type = FixedVectorType::get(f64Type, 2);

	i8x32Type = FixedVectorType::get(i8Type, 32);
	i16x16Type = FixedVectorType::get(i16Type, 16);
	i32x8Type = FixedVectorType::get(i32Type, 8);
	i64x4Type = FixedVectorType::get(i64Type, 4);

	i8x48Type = FixedVectorType::get(i8Type, 48);
	i16x24Type = FixedVectorType::get(i16Type, 24);
	i32x12Type = FixedVectorType::get(i32Type, 12);
	i64x6Type = FixedVectorType::get(i64Type, 6);

	i8x64Type = FixedVectorType::get(i8Type, 64);
	i16x32Type = FixedVectorType::get(i16Type, 32);
	i32x16Type = FixedVectorType::get(i32Type, 16);
	i64x8Type = FixedVectorType::get(i64Type, 8);

	valueTypes[(Uptr)ValueType::none] = valueTypes[(Uptr)ValueType::any] = nullptr;
	valueTypes[(Uptr)ValueType::i32] = i32Type;
	valueTypes[(Uptr)ValueType::i64] = i64Type;
	valueTypes[(Uptr)ValueType::f32] = f32Type;
	valueTypes[(Uptr)ValueType::f64] = f64Type;
	valueTypes[(Uptr)ValueType::v128] = i64x2Type;
	valueTypes[(Uptr)ValueType::externref] = externrefType;
	valueTypes[(Uptr)ValueType::funcref] = externrefType;

	// Create zero constants of each type.
	typedZeroConstants[(Uptr)ValueType::none] = nullptr;
	typedZeroConstants[(Uptr)ValueType::any] = nullptr;
	typedZeroConstants[(Uptr)ValueType::i32] = emitLiteral(*this, (U32)0);
	typedZeroConstants[(Uptr)ValueType::i64] = emitLiteral(*this, (U64)0);
	typedZeroConstants[(Uptr)ValueType::f32] = emitLiteral(*this, (F32)0.0f);
	typedZeroConstants[(Uptr)ValueType::f64] = emitLiteral(*this, (F64)0.0);
	typedZeroConstants[(Uptr)ValueType::v128] = emitLiteral(*this, V128());
	typedZeroConstants[(Uptr)ValueType::externref] = typedZeroConstants[(Uptr)ValueType::funcref]
		= llvm::Constant::getNullValue(externrefType);
}

TargetSpec LLVMJIT::getHostTargetSpec()
{
	TargetSpec result;
	result.triple = llvm::sys::getProcessTriple();
	result.cpu = std::string(llvm::sys::getHostCPUName());
	return result;
}

std::unique_ptr<llvm::TargetMachine> LLVMJIT::getTargetMachine(
	[[maybe_unused]] const TargetSpec& targetSpec)
{
	globalInitLLVMOnce();

	llvm::Triple triple(targetSpec.triple);
	llvm::SmallVector<std::string, 1> targetAttributes;

#if LLVM_VERSION_MAJOR < 10
	if(triple.getArch() == llvm::Triple::x86 || triple.getArch() == llvm::Triple::x86_64)
	{
		// Disable AVX-512 on X86 targets to workaround a LLVM backend bug:
		// https://bugs.llvm.org/show_bug.cgi?id=43750
		targetAttributes.push_back("-avx512f");
	}
#endif
	llvm::EngineBuilder engineBuilder;
	::std::string errormessage;
	engineBuilder.setErrorStr(::std::addressof(errormessage));
	std::unique_ptr<llvm::TargetMachine> targetMachine(
		engineBuilder.selectTarget(triple, "", targetSpec.cpu, targetAttributes));
	if(!targetMachine)
	{
		fprintf(stderr, "llvm::EngineBuilder failed: %s\n", errormessage.c_str());
	}
	return targetMachine;
}

TargetValidationResult LLVMJIT::validateTargetMachine(
	const std::unique_ptr<llvm::TargetMachine>& targetMachine,
	const FeatureSpec& featureSpec)
{
	const llvm::Triple::ArchType targetArch = targetMachine->getTargetTriple().getArch();
	if(targetArch == llvm::Triple::x86_64)
	{
		// If the SIMD feature is enabled, then require the SSE4.1 CPU feature.
		if(featureSpec.simd && !targetMachine->getMCSubtargetInfo()->checkFeatures("+sse4.1"))
		{
			return TargetValidationResult::x86CPUDoesNotSupportSSE41;
		}

		return TargetValidationResult::valid;
	}
	else if(targetArch == llvm::Triple::aarch64)
	{
		if(featureSpec.simd && !targetMachine->getMCSubtargetInfo()->checkFeatures("+neon"))
		{
			return TargetValidationResult::wavmDoesNotSupportSIMDOnArch;
		}

		return TargetValidationResult::valid;
	}
	else
	{
		if(featureSpec.simd) { return TargetValidationResult::wavmDoesNotSupportSIMDOnArch; }
		if(featureSpec.memory64) { return TargetValidationResult::memory64Requires64bitTarget; }
		if(featureSpec.table64) { return TargetValidationResult::table64Requires64bitTarget; }
		return TargetValidationResult::unsupportedArchitecture;
	}
}

TargetValidationResult LLVMJIT::validateTarget(const TargetSpec& targetSpec,
											   IR::FeatureSpec const& featureSpec)
{
	std::unique_ptr<llvm::TargetMachine> targetMachine = getTargetMachine(targetSpec);
	if(!targetMachine) { return TargetValidationResult::invalidTargetSpec; }
	return validateTargetMachine(targetMachine, featureSpec);
}

TargetValidationResult LLVMJIT::validateTargetWithFeatureSpecUpdate(const TargetSpec& targetSpec,
																	IR::FeatureSpec& featureSpec)
{
	std::unique_ptr<llvm::TargetMachine> targetMachine = getTargetMachine(targetSpec);
	bool memtagMteSupported{};
	if(targetMachine)
	{
		if(8 <= targetMachine->getProgramPointerSize())
		{
			featureSpec.memory64 = true;
			featureSpec.table64 = true;
			if(targetMachine->getTargetTriple().getArch() == ::llvm::Triple::aarch64
			   && targetMachine->getMCSubtargetInfo()->checkFeatures("+mte"))
			{
				memtagMteSupported = true;
			}
		}
	}
	if((!memtagMteSupported)
	   && (featureSpec.memtagMte || featureSpec.memtagMteSync || featureSpec.memtagMteIrg
		   || featureSpec.memtagMteSyncIrg))
	{
		featureSpec.memtagMteIrg = featureSpec.memtagMteSyncIrg = featureSpec.memtagMte
			= featureSpec.memtagMteSync = false;
		featureSpec.memtag = true;
	}
	if(featureSpec.memtagFull) { featureSpec.memtag = false; }
	if(!targetMachine) { return TargetValidationResult::invalidTargetSpec; }
	return validateTargetMachine(targetMachine, featureSpec);
}

Version LLVMJIT::getVersion()
{
	return Version{LLVM_VERSION_MAJOR, LLVM_VERSION_MINOR, LLVM_VERSION_PATCH, 5};
}
