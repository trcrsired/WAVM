#include <stddef.h>
#include <unwind.h>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include "EmitFunctionContext.h"
#include "EmitModuleContext.h"
#include "LLVMJITPrivate.h"
#include "WAVM/IR/Module.h"
#include "WAVM/IR/Operators.h"
#include "WAVM/IR/Types.h"
#include "WAVM/IR/Value.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Platform/Signal.h"
#include "WAVM/Runtime/Runtime.h"
#include "WAVM/RuntimeABI/RuntimeABI.h"

PUSH_DISABLE_WARNINGS_FOR_LLVM_HEADERS
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
POP_DISABLE_WARNINGS_FOR_LLVM_HEADERS

using namespace WAVM;
using namespace WAVM::IR;
using namespace WAVM::LLVMJIT;
using namespace WAVM::Runtime;

namespace {
	inline constexpr ::std::uint_least64_t exceptionclass{0x334c4aa53cddfc65};

#if defined(_MSC_VER)
	// MSVC uses the Windows funclet-based exception handling model. A wasm exception is thrown via
	// _CxxThrowException as a pointer to this record, and caught by a catchswitch/catchpad whose
	// handler extracts it with llvm.eh.exceptionpointer.
	struct wavm_eh_record
	{
		::std::uint_least64_t magic; // = exceptionclass
		::std::uint_least64_t ehtag;
		::std::uint_least64_t userdata;
	};

	inline constexpr ::std::size_t EhTagOffset{8};
	inline constexpr ::std::size_t UserDataOffset{16};

	// The _ThrowInfo for the wasm exception record. It only needs to be non-null for
	// _CxxThrowException; catch-all catchpads match any exception.
	struct WavmMsvcThrowInfo
	{
		::std::uint_least32_t attributes;
		void* pCatchableTypeArray;
		void* pThrowUnwindMap;
		const void* pCatchableTypes;
	};
	WavmMsvcThrowInfo wavmThrowInfo{0, nullptr, nullptr, nullptr};

	extern "C" void __cdecl _CxxThrowException(void*, void*);
#else
	struct wavm_eh_tag_unwind_eh
	{
		_Unwind_Exception itaniumeh;
		::std::uint_least64_t ehtag;
		::std::uint_least64_t userdata;
	};

	inline constexpr ::std::size_t EhTagOffset{__builtin_offsetof(wavm_eh_tag_unwind_eh, ehtag)};
	inline constexpr ::std::size_t UserDataOffset{
		__builtin_offsetof(wavm_eh_tag_unwind_eh, userdata)};
#endif

	// The host exception record type used on each platform.
#if defined(_MSC_VER)
	typedef wavm_eh_record WavmEhRecord;
#else
	typedef wavm_eh_tag_unwind_eh WavmEhRecord;
#endif

	// One entry per exception record that may still be named by a live exnref value, registered
	// when a try_table catch_ref/catch_all_ref clause accepts the exception. `bound` is an address
	// in the stack frame that registered the entry: on downward-growing stacks, entries pushed by
	// calls deeper than the current one, or by code in the current frame that has since finished,
	// have a bound at or below the current bound and are stale.
	struct CaughtExceptionEntry
	{
		Uptr bound;
		WavmEhRecord* record;
	};

	// All tracking of owned exception records. The destructor frees every remaining record at
	// thread exit: records may appear in more than one list (a held record can be in flight),
	// so they are deduplicated before freeing.
	struct EhTrackingState
	{
		std::vector<CaughtExceptionEntry> caughtExceptions;

		// Records currently inside a raise call: marked before
		// _Unwind_RaiseException/_CxxThrowException and unmarked when a handler claims the record
		// (wavm_eh_catch_entered/wavm_eh_table_caught) or the raise returns uncaught.
		std::vector<WavmEhRecord*> inFlightRecords;

		// Records whose catch-scope entries went stale. They are kept alive, up to
		// maxDeadRecords, so an exnref that outlived its catch scope can still be rethrown by
		// throw_ref (legal per the spec, though clang only emits exnrefs scoped to their
		// handler). Once evicted, a throw_ref naming them fails closed.
		std::vector<WavmEhRecord*> deadRecords;

		~EhTrackingState()
		{
			std::vector<WavmEhRecord*> records;
			for(const CaughtExceptionEntry& entry : caughtExceptions)
			{
				if(!ehVectorContains(records, entry.record)) { records.push_back(entry.record); }
			}
			for(WavmEhRecord* record : deadRecords)
			{
				if(!ehVectorContains(records, record)) { records.push_back(record); }
			}
			for(WavmEhRecord* record : inFlightRecords)
			{
				if(!ehVectorContains(records, record)) { records.push_back(record); }
			}
			for(WavmEhRecord* record : records) { delete record; }
		}

		static bool ehVectorContains(const std::vector<WavmEhRecord*>& vec,
									 const WavmEhRecord* record)
		{
			for(const WavmEhRecord* element : vec)
			{
				if(element == record) { return true; }
			}
			return false;
		}
	};
	thread_local EhTrackingState ehTracking;

	inline constexpr Uptr maxDeadRecords{32};

	static bool ehVectorContains(const std::vector<WavmEhRecord*>& vec, const WavmEhRecord* record)
	{
		for(const WavmEhRecord* element : vec)
		{
			if(element == record) { return true; }
		}
		return false;
	}

	// Removes the first occurrence of `record` from `vec`, returning whether it was present.
	static bool ehVectorErase(std::vector<WavmEhRecord*>& vec, const WavmEhRecord* record)
	{
		for(auto it = vec.begin(); it != vec.end(); ++it)
		{
			if(*it == record)
			{
				vec.erase(it);
				return true;
			}
		}
		return false;
	}

	static bool ehRecordIsHeld(const WavmEhRecord* record)
	{
		for(const CaughtExceptionEntry& entry : ehTracking.caughtExceptions)
		{
			if(entry.record == record) { return true; }
		}
		return false;
	}

	// Frees a record once nothing can reference it: not held by a catch scope and not in flight.
	static void ehReleaseRecord(WavmEhRecord* record)
	{
		if(!record) { return; }
		if(ehRecordIsHeld(record) || ehVectorContains(ehTracking.inFlightRecords, record)) { return; }
		ehVectorErase(ehTracking.deadRecords, record);
		delete record;
	}

	// Moves a record to the dead list, freeing the oldest dead record nothing references if the
	// list overflows.
	static void ehDeadlistRecord(WavmEhRecord* record)
	{
		if(!record || ehVectorContains(ehTracking.deadRecords, record)) { return; }
		ehTracking.deadRecords.push_back(record);
		while(ehTracking.deadRecords.size() > maxDeadRecords)
		{
			auto it = ehTracking.deadRecords.begin();
			for(; it != ehTracking.deadRecords.end(); ++it)
			{
				if(!ehRecordIsHeld(*it) && !ehVectorContains(ehTracking.inFlightRecords, *it)) { break; }
			}
			if(it == ehTracking.deadRecords.end()) { break; }
			delete *it;
			ehTracking.deadRecords.erase(it);
		}
	}

	// Frees all records except `except`, which is already deleted by the caller (entries still
	// naming it are dropped without a second delete). Only called when an exception propagated
	// past all wasm frames, so every catch scope that held a record is dead. In-flight records
	// are not freed: they are owned by an active raise call that will clean them up itself.
	static void ehFreeAllCaught(const WavmEhRecord* except)
	{
		for(CaughtExceptionEntry& entry : ehTracking.caughtExceptions)
		{
			if(entry.record != except) { delete entry.record; }
		}
		ehTracking.caughtExceptions.clear();
		for(WavmEhRecord* record : ehTracking.deadRecords)
		{
			if(record != except) { delete record; }
		}
		ehTracking.deadRecords.clear();
		ehTracking.inFlightRecords.clear();
	}
}

#if !defined(_MSC_VER)
// Raises a wasm exception record via _Unwind_RaiseException. Does not return: a raise that finds
// no wasm or host handler returns, and is converted to a WAVM uncaughtException runtime error.
static void ehRaiseWasmRecord(wavm_eh_tag_unwind_eh* exception)
{
	if(!ehVectorContains(ehTracking.inFlightRecords, exception)) { ehTracking.inFlightRecords.push_back(exception); }
	_Unwind_Reason_Code reason = _Unwind_RaiseException(&exception->itaniumeh);
	// The exception propagated past all wasm frames, so all records held by catch scopes are
	// dead too.
	(void)reason;
	const ::std::uint_least64_t tag = exception->ehtag;
	const ::std::uint_least64_t userdata = exception->userdata;
	ehVectorErase(ehTracking.inFlightRecords, exception);
	delete exception;
	ehFreeAllCaught(exception);
	Runtime::throwException(Runtime::ExceptionTypes::uncaughtException,
							{IR::UntaggedValue(U64(tag)), IR::UntaggedValue(U64(userdata))});
}
#endif

extern "C" void wavm_throw_wasm_ehtag(::std::uint_least64_t tag, ::std::uint_least64_t value)
{
#if defined(_MSC_VER)
	auto* exception = new wavm_eh_record();
	exception->magic = exceptionclass;
	exception->ehtag = tag;
	exception->userdata = value;
	_CxxThrowException(exception, &wavmThrowInfo);
#else
	auto* exception = new wavm_eh_tag_unwind_eh();
	exception->itaniumeh.exception_class = exceptionclass;
	exception->ehtag = tag;
	exception->userdata = value;
	ehRaiseWasmRecord(exception);
#endif
}

// Re-raises the exception record that just landed on a catch dispatch but matched no clause.
// The record is still marked in flight (no clause claimed it), so this is a plain re-raise.
extern "C" void wavm_rethrow_record(void* recordPtr)
{
#if defined(_MSC_VER)
	auto* exception = reinterpret_cast<wavm_eh_record*>(recordPtr);
	if(!exception || exception->magic != exceptionclass) { std::abort(); }
	_CxxThrowException(exception, &wavmThrowInfo);
#else
	auto* exception = reinterpret_cast<wavm_eh_tag_unwind_eh*>(recordPtr);
	if(!exception || exception->itaniumeh.exception_class != exceptionclass) { std::abort(); }
	ehRaiseWasmRecord(exception);
#endif
}

// Called when a try_table catch_ref/catch_all_ref clause accepts an exception: the exnref value
// the clause pushed names this record, so it is registered as held by a catch scope. Before
// pushing the new entry, entries pushed by frames that have returned or by code in this frame
// that has finished are removed (on a downward-growing stack their bound is at or below this
// call's bound). Their records go to the dead list rather than being freed, so an exnref that
// outlived its scope can still rethrow them.
extern "C" void wavm_eh_catch_entered(void* recordPtr)
{
	auto* record = reinterpret_cast<WavmEhRecord*>(recordPtr);
	char boundMarker;
	const Uptr bound = Uptr(&boundMarker);

	// The exception has landed: it is no longer in flight, and if it was dead-listed while an
	// exnref still named it, it is live again.
	ehVectorErase(ehTracking.inFlightRecords, record);
	ehVectorErase(ehTracking.deadRecords, record);

	for(auto it = ehTracking.caughtExceptions.begin(); it != ehTracking.caughtExceptions.end();)
	{
		if(it->bound <= bound && !ehVectorContains(ehTracking.inFlightRecords, it->record))
		{
			WavmEhRecord* stale = it->record;
			it = ehTracking.caughtExceptions.erase(it);
			if(stale != record) { ehDeadlistRecord(stale); }
		}
		else { ++it; }
	}
	ehTracking.caughtExceptions.push_back({bound, record});
}

// Called when a try_table catch/catch_all clause accepts an exception: the clause produces no
// exnref, so the record is freed unless a live catch scope still holds it.
extern "C" void wavm_eh_table_caught(void* recordPtr)
{
	auto* record = reinterpret_cast<WavmEhRecord*>(recordPtr);
	ehVectorErase(ehTracking.inFlightRecords, record);
	ehReleaseRecord(record);
}

// Implements the throw_ref instruction. The exnref operand is the address of the exception
// record produced by the catch_ref/catch_all_ref clause that caught it. The reference is only
// valid while it names a record WAVM still owns: held by a live catch scope, kept on the dead
// list, or in flight. Anything else — null, a reference to a freed record, or a forged value —
// fails closed with a runtime error instead of dereferencing it.
extern "C" void wavm_throw_ref(::std::uint_least64_t exnref)
{
#if defined(_MSC_VER)
	auto* exception = reinterpret_cast<wavm_eh_record*>(Uptr(exnref));
	if(!exception || exception->magic != exceptionclass)
	{
		Runtime::throwException(Runtime::ExceptionTypes::invalidExnref, {});
	}
	_CxxThrowException(exception, &wavmThrowInfo);
#else
	auto* exception = reinterpret_cast<wavm_eh_tag_unwind_eh*>(Uptr(exnref));
	bool valid = false;
	if(exception)
	{
		valid = ehVectorContains(ehTracking.inFlightRecords, exception) || ehRecordIsHeld(exception)
				|| ehVectorErase(ehTracking.deadRecords, exception);
	}
	if(!valid) { Runtime::throwException(Runtime::ExceptionTypes::invalidExnref, {}); }
	ehRaiseWasmRecord(exception);
#endif
}

// Rethrows the exception currently being handled. Only used on MSVC, from the no-match path of a
// catch handler's dispatch.
#if defined(_MSC_VER)
extern "C" void wavm_rethrow_current() { _CxxThrowException(nullptr, nullptr); }
#endif

static llvm::Function* getWavmThrowWasmEhtagFunction(EmitModuleContext& moduleContext)
{
	if(!moduleContext.wavmThrowWasmEhtagFunction)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		::llvm::Type* ptruinttype{::llvm::Type::getInt64Ty(llvmContext)};
		moduleContext.wavmThrowWasmEhtagFunction = llvm::Function::Create(
			llvm::FunctionType::get(
				llvm::Type::getVoidTy(llvmContext), {ptruinttype, ptruinttype}, false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"wavm_throw_wasm_ehtag",
			moduleContext.llvmModule);
		moduleContext.wavmThrowWasmEhtagFunction->addFnAttr(::llvm::Attribute::AttrKind::NoReturn);
	}
	return moduleContext.wavmThrowWasmEhtagFunction;
}

static llvm::Function* getWavmRethrowWasmEhtagFunction(EmitModuleContext& moduleContext)
{
	if(!moduleContext.wavmRethrowWasmEhtagFunction)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		moduleContext.wavmRethrowWasmEhtagFunction = llvm::Function::Create(
			llvm::FunctionType::get(
				llvm::Type::getVoidTy(llvmContext), {llvmContext.i8PtrType}, false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"wavm_rethrow_record",
			moduleContext.llvmModule);
		moduleContext.wavmRethrowWasmEhtagFunction->addFnAttr(
			::llvm::Attribute::AttrKind::NoReturn);
	}
	return moduleContext.wavmRethrowWasmEhtagFunction;
}

static llvm::Function* getWavmEhCatchEnteredFunction(EmitModuleContext& moduleContext)
{
	if(!moduleContext.wavmEhCatchEnteredFunction)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		moduleContext.wavmEhCatchEnteredFunction = llvm::Function::Create(
			llvm::FunctionType::get(
				llvm::Type::getVoidTy(llvmContext), {llvmContext.i8PtrType}, false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"wavm_eh_catch_entered",
			moduleContext.llvmModule);
	}
	return moduleContext.wavmEhCatchEnteredFunction;
}

static llvm::Function* getWavmThrowRefFunction(EmitModuleContext& moduleContext)
{
	if(!moduleContext.wavmThrowRefFunction)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		moduleContext.wavmThrowRefFunction = llvm::Function::Create(
			llvm::FunctionType::get(
				llvm::Type::getVoidTy(llvmContext), {llvmContext.i64Type}, false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"wavm_throw_ref",
			moduleContext.llvmModule);
		moduleContext.wavmThrowRefFunction->addFnAttr(::llvm::Attribute::AttrKind::NoReturn);
	}
	return moduleContext.wavmThrowRefFunction;
}

static llvm::Function* getWavmEhTableCaughtFunction(EmitModuleContext& moduleContext)
{
	if(!moduleContext.wavmEhTableCaughtFunction)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		moduleContext.wavmEhTableCaughtFunction = llvm::Function::Create(
			llvm::FunctionType::get(
				llvm::Type::getVoidTy(llvmContext), {llvmContext.i8PtrType}, false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"wavm_eh_table_caught",
			moduleContext.llvmModule);
	}
	return moduleContext.wavmEhTableCaughtFunction;
}

#if defined(_MSC_VER)
static llvm::Function* getWavmRethrowCurrentFunction(EmitModuleContext& moduleContext)
{
	if(!moduleContext.wavmRethrowCurrentFunction)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		moduleContext.wavmRethrowCurrentFunction = llvm::Function::Create(
			llvm::FunctionType::get(llvm::Type::getVoidTy(llvmContext), {}, false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"wavm_rethrow_current",
			moduleContext.llvmModule);
		moduleContext.wavmRethrowCurrentFunction->addFnAttr(::llvm::Attribute::AttrKind::NoReturn);
	}
	return moduleContext.wavmRethrowCurrentFunction;
}
#endif

// Coerces an i64 value (the user data carried by a wasm exception) to the LLVM type for a
// WebAssembly value type.
static llvm::Value* coerceI64ToValueType(llvm::IRBuilder<>& irBuilder,
										 LLVMContext& llvmContext,
										 llvm::Value* i64Value,
										 IR::ValueType type)
{
	switch(type)
	{
	case IR::ValueType::i64: return i64Value;
	case IR::ValueType::i32: return irBuilder.CreateTrunc(i64Value, llvmContext.i32Type);
	case IR::ValueType::f64: return irBuilder.CreateBitCast(i64Value, llvmContext.f64Type);
	case IR::ValueType::f32:
		return irBuilder.CreateBitCast(irBuilder.CreateTrunc(i64Value, llvmContext.i32Type),
									   llvmContext.f32Type);
	case IR::ValueType::exnref: return i64Value;
	case IR::ValueType::externref:
	case IR::ValueType::funcref:
		return irBuilder.CreateBitCast(i64Value, asLLVMType(llvmContext, type));
	default: WAVM_UNREACHABLE();
	}
}

// Emits a call to a noreturn exception-raising function (wavm_throw_wasm_ehtag or wavm_throw_ref).
// The call is emitted as an invoke to the innermost enclosing landingpad so that the raised
// exception is caught by the enclosing try/try_table blocks.
void EmitFunctionContext::emitRaiseFunctionCall(llvm::Function* raiseFunction,
												llvm::ArrayRef<llvm::Value*> args)
{
	auto unwindToBlock = getInnermostUnwindToBlock();
	if(unwindToBlock)
	{
		auto returnBlock = llvm::BasicBlock::Create(llvmContext, "raiseReturn", function);
		irBuilder.CreateInvoke(
			raiseFunction->getFunctionType(), raiseFunction, returnBlock, unwindToBlock, args);
		irBuilder.SetInsertPoint(returnBlock);
		irBuilder.CreateUnreachable();
	}
	else
	{
		irBuilder.CreateCall(raiseFunction, args);
		irBuilder.CreateUnreachable();
	}
}

#if !defined(_MSC_VER)
void EmitFunctionContext::emitUnhandledExceptionDispatch(llvm::LandingPadInst* landingPadInst)
{
	// An exception that reached the end of the catch clause dispatch matched no clause. This is
	// not emitted as an LLVM 'resume' because DwarfEHPrepare prunes resumes that are not
	// reachable from a cleanup landingpad, which would fold away the tag checks above. It is
	// also not a _Unwind_Resume for wasm exceptions, because that would continue the current
	// unwind past this frame and skip an enclosing handler in the same function. Instead, wasm
	// exceptions are re-raised fresh via wavm_rethrow_record, which starts a new unwind whose
	// search can find an enclosing handler for this callsite, and reports
	// wavm.uncaughtException if none handle it. Foreign host exceptions (e.g. WAVM
	// runtime exceptions) are re-raised with _Unwind_RaiseException so that they
	// propagate unchanged to the host handler.
	auto unwindehptr = irBuilder.CreateExtractValue(landingPadInst, {0});
	auto magic = ::WAVM::LLVMJIT::wavmCreateLoad(irBuilder, llvmContext.i64Type, unwindehptr);
	auto isUserExceptionType = irBuilder.CreateICmpEQ(
		magic, ::llvm::ConstantInt::get(llvmContext.i64Type, exceptionclass));

	auto wasmRethrowBlock
		= llvm::BasicBlock::Create(llvmContext, "unhandledRethrow", function);
	auto foreignResumeBlock
		= llvm::BasicBlock::Create(llvmContext, "unhandledResume", function);
	irBuilder.CreateCondBr(isUserExceptionType, wasmRethrowBlock, foreignResumeBlock);

	irBuilder.SetInsertPoint(wasmRethrowBlock);
	auto rethrowFunc = getWavmRethrowWasmEhtagFunction(moduleContext);
	emitRaiseFunctionCall(rethrowFunc, {unwindehptr});

	irBuilder.SetInsertPoint(foreignResumeBlock);
	// A foreign exception is re-raised fresh with _Unwind_RaiseException. It cannot be
	// continued with _Unwind_Resume: the resumed phase 2 walk would reach this frame again
	// with the same SP that phase 1 recorded, re-selecting this landing pad forever. An LLVM
	// 'resume' instruction cannot be used either: DwarfEHPrepare prunes resumes that are not
	// reachable from a cleanup-only landingpad, and this landingpad must have a catch clause
	// for the phase 1 search to stop at this frame. A fresh raise evaluates this frame at the
	// raise callsite, which has no landing pad, so the exception propagates to the host.
	auto unwindRaiseFunc = moduleContext.llvmModule->getOrInsertFunction(
		"_Unwind_RaiseException",
		llvm::FunctionType::get(llvmContext.i32Type, {llvmContext.i8PtrType}, false));
	irBuilder.CreateCall(unwindRaiseFunc, {unwindehptr});
	irBuilder.CreateUnreachable();
}
#endif

llvm::BasicBlock* EmitContext::getInnermostUnwindToBlock()
{
	if(!tryStack.empty())
	{
		auto temp = tryStack.back().unwindToBlock;
		return temp;
	}
	else
	{
		return nullptr;
	}
}

void EmitFunctionContext::try_table(TryTableImm imm)
{
	FunctionType blockType = resolveBlockType(irModule, imm.type);

	// Create an end block+phi for the try_table result.
	auto endBlock = llvm::BasicBlock::Create(llvmContext, "tryTableEnd", function);
	auto endPHIs = createPHIs(endBlock, blockType.results());

	// Pop the try_table arguments.
	llvm::Value** tryTableArgs
		= (llvm::Value**)alloca(sizeof(llvm::Value*) * blockType.params().size());
	popMultiple(tryTableArgs, blockType.params().size());

	// Push a control context that ends at the end block/phi.
	pushControlStack(ControlContext::Type::tryTable, blockType.results(), endBlock, endPHIs);

	// Push a branch target for the end block/phi.
	pushBranchTarget(blockType.results(), endBlock, endPHIs);

	// Repush the try_table arguments.
	pushMultiple(tryTableArgs, blockType.params().size());

#if defined(_MSC_VER)
	// MSVC uses the funclet-based EH model. Create a catchswitch with a single catch-all catchpad
	// that catches any wasm exception thrown in the try_table body. The catchpad's handler then
	// dispatches on the exception's tag.
	auto dispatchBlock = llvm::BasicBlock::Create(llvmContext, "catchDispatch", function);
	auto catchPadBlock = llvm::BasicBlock::Create(llvmContext, "catchPad", function);
	{
		::llvm::IRBuilderBase::InsertPointGuard guard(irBuilder);

		// The catchswitch's funclet parent is the innermost enclosing funclet pad, if any.
		llvm::Value* parentPad = funcletPadStack.empty() ? llvm::ConstantTokenNone::get(llvmContext)
														 : funcletPadStack.back();

		irBuilder.SetInsertPoint(dispatchBlock);
		auto catchSwitchInst
			= irBuilder.CreateCatchSwitch(parentPad, nullptr, 1); // unwind to caller
		catchSwitchInst->addHandler(catchPadBlock);

		irBuilder.SetInsertPoint(catchPadBlock);
		auto catchPadInst
			= irBuilder.CreateCatchPad(catchSwitchInst,
									   {llvm::Constant::getNullValue(llvmContext.i8PtrType),
										llvm::ConstantInt::get(llvmContext.i32Type, 64),
										llvm::Constant::getNullValue(llvmContext.i8PtrType)});

		tryStack.push_back(TryContext{dispatchBlock});
		tryTableStack.push_back(
			TryTableContext{catchSwitchInst, catchPadInst, imm.catchTableIndex});
		funcletPadStack.push_back(catchPadInst);
	}
#else
	// Create the landingpad block that is the unwind target for any calls in the try_table body,
	// and that the catch clauses will dispatch from.
	auto landingPadBlock = llvm::BasicBlock::Create(llvmContext, "tryTableLandingPad", function);
	{
		::llvm::IRBuilderBase::InsertPointGuard guard(irBuilder);
		irBuilder.SetInsertPoint(landingPadBlock);
		auto landingPadInst = irBuilder.CreateLandingPad(
			llvm::StructType::get(llvmContext, {llvmContext.i8PtrType, llvmContext.i32Type}), 1);
		landingPadInst->setCleanup(true);
		landingPadInst->addClause(::llvm::ConstantPointerNull::get(irBuilder.getPtrTy()));

		tryStack.push_back(TryContext{landingPadBlock});
		tryTableStack.push_back(
			TryTableContext{landingPadInst, landingPadBlock, imm.catchTableIndex});
	}
#endif
}

void EmitFunctionContext::endTryTable()
{
	// Pop the try context (the unwind target for calls in the try_table body).
	WAVM_ASSERT(!tryStack.empty());
	tryStack.pop_back();

	WAVM_ASSERT(!tryTableStack.empty());
	TryTableContext& tryTableContext = tryTableStack.back();

	WAVM_ASSERT(tryTableContext.catchTableIndex < functionDef.catchClauses.size());
	const std::vector<IR::CatchClause>& catchClauses
		= functionDef.catchClauses[tryTableContext.catchTableIndex];

	// Emit the dispatch in the catch pad block.
	llvm::BasicBlock* savedInsertionPoint = irBuilder.GetInsertBlock();
#if defined(_MSC_VER)
	irBuilder.SetInsertPoint(tryTableContext.catchPadInst->getParent());

	// Get the pointer to the caught exception record via llvm.eh.exceptionpointer.
	llvm::Function* exceptionPointerFn = moduleContext.getLLVMIntrinsic(
		{llvmContext.i8PtrType}, llvm::Intrinsic::eh_exceptionpointer);
	auto unwindehptr = irBuilder.CreateCall(
		exceptionPointerFn->getFunctionType(), exceptionPointerFn, {tryTableContext.catchPadInst});
#else
	irBuilder.SetInsertPoint(tryTableContext.landingPadBlock);

	auto unwindehptr = irBuilder.CreateExtractValue(tryTableContext.landingPadInst, {0});
#endif
	auto magic = ::WAVM::LLVMJIT::wavmCreateLoad(irBuilder, llvmContext.i64Type, unwindehptr);
	auto isUserExceptionType = irBuilder.CreateICmpEQ(
		magic, ::llvm::ConstantInt::get(llvmContext.i64Type, exceptionclass));

	// Create a check block and a match block for each catch clause, and a no-match block that
	// rethrows the exception.
	auto noMatchBlock = llvm::BasicBlock::Create(llvmContext, "tryTableNoMatch", function);
	std::vector<llvm::BasicBlock*> checkBlocks;
	std::vector<llvm::BasicBlock*> matchBlocks;
	checkBlocks.reserve(catchClauses.size());
	matchBlocks.reserve(catchClauses.size());
	for(Uptr clauseIndex = 0; clauseIndex < catchClauses.size(); ++clauseIndex)
	{
		checkBlocks.push_back(llvm::BasicBlock::Create(llvmContext, "tryTableCheck", function));
		matchBlocks.push_back(llvm::BasicBlock::Create(llvmContext, "tryTableMatch", function));
	}

	// Only wasm exceptions (with the WAVM exception class) can be caught by a catch clause;
	// anything else is rethrown.
	if(checkBlocks.empty()) { irBuilder.CreateBr(noMatchBlock); }
	else
	{
		irBuilder.CreateCondBr(isUserExceptionType, checkBlocks[0], noMatchBlock);
	}

	// Emit each catch clause's check.
	for(Uptr clauseIndex = 0; clauseIndex < catchClauses.size(); ++clauseIndex)
	{
		const IR::CatchClause& catchClause = catchClauses[clauseIndex];
		irBuilder.SetInsertPoint(checkBlocks[clauseIndex]);

		llvm::BasicBlock* nextBlock
			= clauseIndex + 1 < catchClauses.size() ? checkBlocks[clauseIndex + 1] : noMatchBlock;
		if(catchClause.kind == IR::CatchClauseKind::catch_
		   || catchClause.kind == IR::CatchClauseKind::catch_ref)
		{
			auto ehtagId = ::WAVM::LLVMJIT::wavmCreateLoad(
				irBuilder,
				llvmContext.i64Type,
				irBuilder.CreateGEP(llvmContext.i8Type,
									unwindehptr,
									{::llvm::ConstantInt::get(llvmContext.i64Type, EhTagOffset)}));
			// The exception's tag identity is the runtime id of the ExceptionType object
			// created for the tag at instantiation, so tags with the same signature are
			// distinguished and imported tags match the tag they are bound to.
			auto isehtagId = irBuilder.CreateICmpEQ(
				ehtagId,
				irBuilder.CreateZExtOrTrunc(
					moduleContext.exceptionTypeIds[catchClause.exceptionTypeIndex],
					llvmContext.i64Type));
			irBuilder.CreateCondBr(isehtagId, matchBlocks[clauseIndex], nextBlock);
		}
		else
		{
			// catch_all / catch_all_ref match any wasm exception.
			irBuilder.CreateBr(matchBlocks[clauseIndex]);
		}
	}

	// Emit each catch clause's match block: branch to the clause's target label, pushing the
	// exception's payload (which is also the exnref for catch_ref/catch_all_ref) as the target's
	// arguments.
	for(Uptr clauseIndex = 0; clauseIndex < catchClauses.size(); ++clauseIndex)
	{
		const IR::CatchClause& catchClause = catchClauses[clauseIndex];
		irBuilder.SetInsertPoint(matchBlocks[clauseIndex]);

		// The catch clause label depth is relative to the labels enclosing the try_table, so add 1
		// to account for the try_table's own branch target.
		BranchTarget& target = getBranchTargetByDepth(catchClause.labelDepth + 1);
		WAVM_ASSERT(target.params.size() == target.phis.size());

		auto userData = ::WAVM::LLVMJIT::wavmCreateLoad(
			irBuilder,
			llvmContext.i64Type,
			irBuilder.CreateGEP(llvmContext.i8Type,
								unwindehptr,
								{::llvm::ConstantInt::get(llvmContext.i64Type, UserDataOffset)}));
		for(Uptr argIndex = 0; argIndex < target.params.size(); ++argIndex)
		{
			// For the exnref parameter produced by catch_ref/catch_all_ref, the exnref value is
			// the address of the exception record itself. Other parameters carry the exception's
			// payload.
			llvm::Value* payload
				= target.params[argIndex] == IR::ValueType::exnref
					  ? irBuilder.CreatePtrToInt(unwindehptr, llvmContext.i64Type)
					  : coerceI64ToValueType(
							irBuilder, llvmContext, userData, target.params[argIndex]);
			target.phis[argIndex]->addIncoming(coerceToCanonicalType(payload),
											   irBuilder.GetInsertBlock());
		}
#if defined(_MSC_VER)
		irBuilder.CreateCatchRet(tryTableContext.catchPadInst, target.block);
#else
		// A catch_ref/catch_all_ref clause keeps the record referenceable through the exnref it
		// pushed; a plain catch/catch_all clause releases it unless a live catch scope still
		// holds it.
		const bool clauseProducesExnref = catchClause.kind == IR::CatchClauseKind::catch_ref
										  || catchClause.kind == IR::CatchClauseKind::catch_all_ref;
		irBuilder.CreateCall(clauseProducesExnref
								 ? getWavmEhCatchEnteredFunction(moduleContext)
								 : getWavmEhTableCaughtFunction(moduleContext),
							 {unwindehptr});
		irBuilder.CreateBr(target.block);
#endif
	}

	// Emit the no-match block, which rethrows the exception to an outer handler.
	irBuilder.SetInsertPoint(noMatchBlock);
#if defined(_MSC_VER)
	// Rethrow the currently handled exception. The call must be marked as belonging to the
	// catchpad's funclet.
	auto rethrowFn = getWavmRethrowCurrentFunction(moduleContext);
	llvm::SmallVector<llvm::Value*, 1> funcletInputs = {tryTableContext.catchPadInst};
	llvm::OperandBundleDef funcletBundle("funclet", funcletInputs);
	irBuilder.CreateCall(rethrowFn->getFunctionType(), rethrowFn, {}, {funcletBundle});
	irBuilder.CreateUnreachable();
#else
	emitUnhandledExceptionDispatch(tryTableContext.landingPadInst);
#endif

	irBuilder.SetInsertPoint(savedInsertionPoint);
#if defined(_MSC_VER)
	WAVM_ASSERT(!funcletPadStack.empty());
	funcletPadStack.pop_back();
#endif
	tryTableStack.pop_back();
}
void EmitFunctionContext::throw_(ExceptionTypeImm imm)
{
	auto ehptr = pop();

	auto ehtagfunc = getWavmThrowWasmEhtagFunction(moduleContext);
	ehptr = irBuilder.CreateZExt(ehptr, llvmContext.i64Type);
	// The tag identity passed to the runtime is the id of the ExceptionType object
	// instantiated for the tag: it is unique per tag and shared across modules through
	// imports.
	emitRaiseFunctionCall(
		ehtagfunc,
		{irBuilder.CreateZExtOrTrunc(moduleContext.exceptionTypeIds[imm.exceptionTypeIndex],
									 llvmContext.i64Type),
		 ehptr});
	enterUnreachable();
}

void EmitFunctionContext::throw_ref(NoImm)
{
	// throw_ref re-raises the exception named by the exnref operand, invoked to the innermost
	// enclosing landingpad so that it propagates through enclosing try_table blocks just like a
	// wasm throw.
	auto exnref = pop();
	emitRaiseFunctionCall(getWavmThrowRefFunction(moduleContext), {exnref});
	enterUnreachable();
}
