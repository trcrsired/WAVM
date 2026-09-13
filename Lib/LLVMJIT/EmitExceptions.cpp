#include <stddef.h>
#include <unwind.h>
#include <cstdint>
#include <cstdlib>
#include <deque>
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

	// Index into EhTrackingState::recordPool identifying a record slot. Records are named by slot
	// rather than pointer so that references are bounds-checked and paired with a generation.
	typedef U32 EhSlot;
	inline constexpr EhSlot invalidEhSlot{~EhSlot(0)};

	// The data an exnref value carries: the exception's tag identity and payload. An exnref is a
	// copyable value, so catching an exception copies this out and releases the record; throw_ref
	// builds a fresh record from the stored value. Because the payload is a single word, an
	// exnref can never contain another exnref, so the value is always self-contained.
	struct ExnrefValue
	{
		::std::uint_least64_t ehtag;
		::std::uint_least64_t userdata;
	};

	// All tracking of exception records and exnref values. Records live in recordPool, a deque so
	// that their addresses never move: a record in flight is referenced by the unwind machinery
	// for the whole raise. Freed slots are recycled through freeSlots, so the pool only grows to
	// the largest number of simultaneously in-flight records.
	struct EhTrackingState
	{
		std::deque<WavmEhRecord> recordPool;

		// Generation per pool slot, bumped on free; not user-visible, but kept so the pool can
		// safely recycle slots.
		std::vector<U32> slotGenerations;

		// Pool slots that are free for reuse.
		std::vector<EhSlot> freeSlots;

		// Slots of records currently inside a raise call: marked before
		// _Unwind_RaiseException/_CxxThrowException and unmarked when a handler claims the record
		// (wavm_eh_catch_entered/wavm_eh_table_caught) or the raise returns uncaught.
		std::vector<EhSlot> inFlightRecords;

		// The values exnref handles name, kept as a bounded ring: when it fills, the oldest value
		// is evicted and its slot's generation is bumped so stale exnrefs fail closed. Exnref
		// liveness cannot be tracked without GC, so a bounded table is the compromise.
		std::vector<ExnrefValue> exnrefValues;
		std::vector<U32> exnrefGenerations;
		Uptr exnrefNext{0};
	};
	thread_local EhTrackingState ehTracking;

	inline constexpr Uptr exnrefValueCapacity{1024};

	static bool ehVectorContains(const std::vector<EhSlot>& vec, EhSlot slot)
	{
		for(EhSlot element : vec)
		{
			if(element == slot) { return true; }
		}
		return false;
	}

	// Removes the first occurrence of `slot` from `vec`, returning whether it was present.
	static bool ehVectorErase(std::vector<EhSlot>& vec, EhSlot slot)
	{
		for(auto it = vec.begin(); it != vec.end(); ++it)
		{
			if(*it == slot)
			{
				vec.erase(it);
				return true;
			}
		}
		return false;
	}

	// Returns the pool slot holding `record`, or invalidEhSlot if the pointer does not name a
	// pooled record.
	static EhSlot ehSlotOfRecord(const WavmEhRecord* record)
	{
		for(Uptr slot = 0; slot < ehTracking.recordPool.size(); ++slot)
		{
			if(&ehTracking.recordPool[slot] == record) { return EhSlot(slot); }
		}
		return invalidEhSlot;
	}

	static WavmEhRecord* ehRecordAt(EhSlot slot) { return &ehTracking.recordPool[slot]; }

	// Allocates a pool slot, reusing a freed one if available, and returns it.
	static EhSlot ehAllocateRecord()
	{
		if(!ehTracking.freeSlots.empty())
		{
			const EhSlot slot = ehTracking.freeSlots.back();
			ehTracking.freeSlots.pop_back();
			return slot;
		}
		const EhSlot slot = EhSlot(ehTracking.recordPool.size());
		ehTracking.recordPool.push_back(WavmEhRecord{});
		ehTracking.slotGenerations.push_back(0);
		return slot;
	}

	// Frees a pool slot: bumps its generation so stale references naming it fail, and recycles
	// the slot. Idempotent: a slot may be named by more than one tracking list.
	static void ehFreeRecord(EhSlot slot)
	{
		if(ehVectorContains(ehTracking.freeSlots, slot)) { return; }
		++ehTracking.slotGenerations[slot];
		ehTracking.freeSlots.push_back(slot);
	}

	// Stores an exnref value and returns its handle: generation in the high bits and index+1 in
	// the low bits, so that 0 remains the null exnref.
	static U64 ehMakeExnref(const ExnrefValue& value)
	{
		Uptr index;
		if(ehTracking.exnrefValues.size() < exnrefValueCapacity)
		{
			index = ehTracking.exnrefValues.size();
			ehTracking.exnrefValues.push_back(value);
			ehTracking.exnrefGenerations.push_back(0);
		}
		else
		{
			index = ehTracking.exnrefNext;
			ehTracking.exnrefValues[index] = value;
			++ehTracking.exnrefGenerations[index];
			ehTracking.exnrefNext = (ehTracking.exnrefNext + 1) % exnrefValueCapacity;
		}
		return (U64(ehTracking.exnrefGenerations[index]) << 32) | U64(index + 1);
	}

	// Decodes an exnref to its value, or returns false for null, out-of-range, or evicted
	// values.
	static bool ehExnrefToValue(U64 exnref, ExnrefValue& value)
	{
		const U64 indexField = exnref & 0xffffffff;
		if(!indexField) { return false; }
		const Uptr index = Uptr(indexField - 1);
		if(index >= ehTracking.exnrefValues.size()
		   || ehTracking.exnrefGenerations[index] != U32(exnref >> 32))
		{
			return false;
		}
		value = ehTracking.exnrefValues[index];
		return true;
	}

	// Frees all records except `exceptSlot`, whose slot the caller has already freed. Only
	// called when an exception propagated past all wasm frames, so every in-flight record
	// belongs to a raise frame the resulting host exception unwinds through.
	static void ehFreeAllInFlight(EhSlot exceptSlot)
	{
		for(EhSlot slot : ehTracking.inFlightRecords)
		{
			if(slot != exceptSlot) { ehFreeRecord(slot); }
		}
		ehTracking.inFlightRecords.clear();
	}
}

#if !defined(_MSC_VER)
// Raises the wasm exception record in `slot` via _Unwind_RaiseException. Does not return: a
// raise that finds no wasm or host handler returns, and is converted to a WAVM
// uncaughtException runtime error.
static void ehRaiseWasmRecord(EhSlot slot)
{
	WavmEhRecord* exception = ehRecordAt(slot);
	if(!ehVectorContains(ehTracking.inFlightRecords, slot)) { ehTracking.inFlightRecords.push_back(slot); }
	_Unwind_Reason_Code reason = _Unwind_RaiseException(&exception->itaniumeh);
	// The exception propagated past all wasm frames, so every other in-flight record is dead
	// too.
	(void)reason;
	const ::std::uint_least64_t tag = exception->ehtag;
	const ::std::uint_least64_t userdata = exception->userdata;
	ehVectorErase(ehTracking.inFlightRecords, slot);
	ehFreeRecord(slot);
	ehFreeAllInFlight(slot);
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
	const EhSlot slot = ehAllocateRecord();
	auto* exception = ehRecordAt(slot);
	*exception = wavm_eh_tag_unwind_eh{};
	exception->itaniumeh.exception_class = exceptionclass;
	exception->ehtag = tag;
	exception->userdata = value;
	ehRaiseWasmRecord(slot);
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
	const EhSlot slot = ehSlotOfRecord(exception);
	if(slot == invalidEhSlot) { std::abort(); }
	ehRaiseWasmRecord(slot);
#endif
}

// Called when a try_table catch_ref/catch_all_ref clause accepts an exception: copies the
// record's tag and payload out as an exnref value, releases the record, and returns the handle
// the clause pushes. The value is self-contained, so the exnref stays valid for as long as the
// bounded value table retains it regardless of which catch scopes are still live.
extern "C" ::std::uint_least64_t wavm_eh_catch_entered(void* recordPtr)
{
	auto* record = reinterpret_cast<WavmEhRecord*>(recordPtr);
	const EhSlot slot = ehSlotOfRecord(record);
	if(slot == invalidEhSlot) { std::abort(); }

	// The exception has landed: claim it out of the in-flight set, copy its data out as the
	// exnref value, and free the record.
	ehVectorErase(ehTracking.inFlightRecords, slot);
	const U64 exnref = ehMakeExnref(ExnrefValue{record->ehtag, record->userdata});
	ehFreeRecord(slot);
	return exnref;
}

// Called when a try_table catch/catch_all clause accepts an exception: the clause produces no
// exnref, so the record is simply released.
extern "C" void wavm_eh_table_caught(void* recordPtr)
{
	auto* record = reinterpret_cast<WavmEhRecord*>(recordPtr);
	const EhSlot slot = ehSlotOfRecord(record);
	if(slot == invalidEhSlot) { std::abort(); }
	ehVectorErase(ehTracking.inFlightRecords, slot);
	ehFreeRecord(slot);
}

// Implements the throw_ref instruction. The exnref operand is the handle produced by a
// catch_ref/catch_all_ref clause and names a stored {tag, payload} value. The reference is
// only valid while the bounded value table retains it; anything else — null, an evicted
// entry, or a forged value — fails closed with a runtime error instead of dereferencing it.
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
	ExnrefValue value;
	if(!ehExnrefToValue(exnref, value))
	{
		Runtime::throwException(Runtime::ExceptionTypes::invalidExnref, {});
	}
	const EhSlot slot = ehAllocateRecord();
	auto* exception = ehRecordAt(slot);
	*exception = wavm_eh_tag_unwind_eh{};
	exception->itaniumeh.exception_class = exceptionclass;
	exception->ehtag = value.ehtag;
	exception->userdata = value.userdata;
	ehRaiseWasmRecord(slot);
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
			llvm::FunctionType::get(llvmContext.i64Type, {llvmContext.i8PtrType}, false),
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
#if defined(_MSC_VER)
		// For the exnref parameter produced by catch_ref/catch_all_ref, the exnref value is the
		// address of the exception record itself.
		llvm::Value* exnrefValue = irBuilder.CreatePtrToInt(unwindehptr, llvmContext.i64Type);
#else
		// A catch_ref/catch_all_ref clause keeps the record referenceable through the exnref it
		// pushes; a plain catch/catch_all clause releases it unless a live catch scope still
		// holds it. wavm_eh_catch_entered registers the record and returns the exnref handle.
		const bool clauseProducesExnref = catchClause.kind == IR::CatchClauseKind::catch_ref
										  || catchClause.kind == IR::CatchClauseKind::catch_all_ref;
		llvm::Value* exnrefValue = nullptr;
		if(clauseProducesExnref)
		{
			exnrefValue = irBuilder.CreateCall(
				getWavmEhCatchEnteredFunction(moduleContext), {unwindehptr});
		}
		else
		{
			irBuilder.CreateCall(getWavmEhTableCaughtFunction(moduleContext), {unwindehptr});
		}
#endif
		for(Uptr argIndex = 0; argIndex < target.params.size(); ++argIndex)
		{
			// The exnref parameter produced by catch_ref/catch_all_ref gets the record's handle.
			// Other parameters carry the exception's payload.
			llvm::Value* payload
				= target.params[argIndex] == IR::ValueType::exnref
					  ? exnrefValue
					  : coerceI64ToValueType(
							irBuilder, llvmContext, userData, target.params[argIndex]);
			target.phis[argIndex]->addIncoming(coerceToCanonicalType(payload),
											   irBuilder.GetInsertBlock());
		}
#if defined(_MSC_VER)
		irBuilder.CreateCatchRet(tryTableContext.catchPadInst, target.block);
#else
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
