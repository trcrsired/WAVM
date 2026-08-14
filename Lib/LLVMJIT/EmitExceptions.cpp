#include <stddef.h>
#include <unwind.h>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <vector>
#include "EmitFunctionContext.h"
#include "EmitModuleContext.h"
#include "LLVMJITPrivate.h"
#include "WAVM/IR/Module.h"
#include "WAVM/IR/Operators.h"
#include "WAVM/IR/Types.h"
#include "WAVM/IR/Value.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Platform/Signal.h"
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
		::std::uint_least64_t magic;   // = exceptionclass
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

	// Maps a wasm exception's payload value (the address of the exception object in wasm memory,
	// which is what an exnref refers to) to the host exception object that was raised for it. This
	// is used by throw_ref to re-raise a caught exception.
	thread_local std::unordered_map<::std::uint_least64_t, void*> exnrefToUnwindExceptionMap;

#if defined(_MSC_VER)
	// Stores the host exception record structs allocated for each thrown exception. They must stay
	// alive for as long as the exception may be caught/rethrown, so they are never freed.
	thread_local std::vector<std::unique_ptr<wavm_eh_record>> exceptionPool;
#else
	// Stores the host _Unwind_Exception structs allocated for each thrown exception. They must stay
	// alive for as long as the exception may be caught/rethrown, so they are never freed.
	thread_local std::vector<std::unique_ptr<wavm_eh_tag_unwind_eh>> exceptionPool;
#endif

}

extern "C" void wavm_throw_wasm_ehtag(::std::uint_least64_t tag, ::std::uint_least64_t value)
{
#if defined(_MSC_VER)
	auto exception = std::make_unique<wavm_eh_record>();
	exception->magic = exceptionclass;
	exception->ehtag = tag;
	exception->userdata = value;
	exnrefToUnwindExceptionMap[value] = exception.get();
	exceptionPool.push_back(std::move(exception));
	_CxxThrowException(exceptionPool.back().get(), &wavmThrowInfo);
#else
	auto exception = std::make_unique<wavm_eh_tag_unwind_eh>();
	exception->itaniumeh.exception_class = exceptionclass;
	exception->ehtag = tag;
	exception->userdata = value;
	exnrefToUnwindExceptionMap[value] = __builtin_addressof(exception->itaniumeh);
	_Unwind_Exception* exceptionObject = __builtin_addressof(exception->itaniumeh);
	exceptionPool.push_back(std::move(exception));
	_Unwind_RaiseException(exceptionObject);
#endif
}

extern "C" void wavm_throw_ref(::std::uint_least64_t exnref)
{
	auto mapIt = exnrefToUnwindExceptionMap.find(exnref);
	if(mapIt != exnrefToUnwindExceptionMap.end())
	{
#if defined(_MSC_VER)
		_CxxThrowException(mapIt->second, &wavmThrowInfo);
#else
		_Unwind_RaiseException(static_cast<_Unwind_Exception*>(mapIt->second));
#endif
	}
	// If the exnref doesn't map to a live exception, abort.
	std::abort();
}

// Rethrows the exception currently being handled. Only used on MSVC, from the no-match path of a
// catch handler's dispatch.
#if defined(_MSC_VER)
extern "C" void wavm_rethrow_current()
{
	_CxxThrowException(nullptr, nullptr);
}
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
				llvm::Type::getVoidTy(llvmContext), {llvmContext.i64Type}, false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"wavm_throw_ref",
			moduleContext.llvmModule);
		moduleContext.wavmRethrowWasmEhtagFunction->addFnAttr(::llvm::Attribute::AttrKind::NoReturn);
	}
	return moduleContext.wavmRethrowWasmEhtagFunction;
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
		return irBuilder.CreateBitCast(
			irBuilder.CreateTrunc(i64Value, llvmContext.i32Type), llvmContext.f32Type);
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
		irBuilder.CreateInvoke(raiseFunction->getFunctionType(),
							   raiseFunction,
							   returnBlock,
							   unwindToBlock,
							   args);
		irBuilder.SetInsertPoint(returnBlock);
		irBuilder.CreateUnreachable();
	}
	else
	{
		irBuilder.CreateCall(raiseFunction, args);
		irBuilder.CreateUnreachable();
	}
}

void EmitFunctionContext::endTryWithoutCatch()
{
	WAVM_ASSERT(!tryStack.empty());
	tryStack.pop_back();
	endTryCatch();
}

void EmitFunctionContext::endTryCatch()
{
	WAVM_ASSERT(!catchStack.empty());
	CatchContext& catchContext = catchStack.back();

	// If an end instruction terminates a sequence of catch clauses, terminate the chain of
	// handler type ID tests by rethrowing the exception if its type ID didn't match any of the
	// handlers.
	llvm::BasicBlock* savedInsertionPoint = irBuilder.GetInsertBlock();
	irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);

	irBuilder.CreateResume(catchContext.landingPadInst);

	irBuilder.SetInsertPoint(savedInsertionPoint);
	catchStack.pop_back();
}

llvm::BasicBlock* EmitContext::getInnermostUnwindToBlock()
{
	if(!tryStack.empty())
	{
		auto temp = tryStack.back().unwindToBlock;
		return temp;
	}
	else { return nullptr; }
}

static inline void generate_catch_common(EmitFunctionContext& emitFunctionContext)
{
	using TryContext = typename EmitFunctionContext::TryContext;
	using CatchContext = typename EmitFunctionContext::CatchContext;
	auto& llvmContext{emitFunctionContext.llvmContext};
	auto& irBuilder{emitFunctionContext.irBuilder};
	auto& function{emitFunctionContext.function};
	auto& tryStack{emitFunctionContext.tryStack};
	auto& catchStack{emitFunctionContext.catchStack};

	// Create a BasicBlock with a LandingPad instruction to use as the unwind target.
	auto landingPadBlock = llvm::BasicBlock::Create(llvmContext, "landingPad", function);
	irBuilder.SetInsertPoint(landingPadBlock);
	auto landingPadInst = irBuilder.CreateLandingPad(
		llvm::StructType::get(llvmContext, {llvmContext.i8PtrType, llvmContext.i32Type}), 1);

	tryStack.push_back(TryContext{landingPadBlock});
	catchStack.push_back(CatchContext{nullptr, landingPadInst, nullptr, landingPadBlock, nullptr});
}

void EmitFunctionContext::try_(ControlStructureImm imm)
{
	{
		::llvm::IRBuilderBase::InsertPointGuard guard(irBuilder);
		generate_catch_common(*this);
	}

	// Create an end try+phi for the try result.
	FunctionType blockType = resolveBlockType(irModule, imm.type);
	auto endBlock = llvm::BasicBlock::Create(llvmContext, "tryEnd", function);
	auto endPHIs = createPHIs(endBlock, blockType.results());

	// Pop the try arguments.
	llvm::Value** tryArgs = (llvm::Value**)alloca(sizeof(llvm::Value*) * blockType.params().size());
	popMultiple(tryArgs, blockType.params().size());

	// Push a control context that ends at the end block/phi.
	pushControlStack(ControlContext::Type::try_, blockType.results(), endBlock, endPHIs);

	// Remember the landingpad on the control context, so that the 'rethrow' instruction can find
	// the landingpad of the catch at the requested depth.
	controlStack.back().landingPadInst = catchStack.back().landingPadInst;

	// Push a branch target for the end block/phi.
	pushBranchTarget(blockType.results(), endBlock, endPHIs);

	// Repush the try arguments.
	pushMultiple(tryArgs, blockType.params().size());
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
		llvm::Value* parentPad = funcletPadStack.empty()
									 ? llvm::ConstantTokenNone::get(llvmContext)
									 : funcletPadStack.back();

		irBuilder.SetInsertPoint(dispatchBlock);
		auto catchSwitchInst
			= irBuilder.CreateCatchSwitch(parentPad, nullptr, 1); // unwind to caller
		catchSwitchInst->addHandler(catchPadBlock);

		irBuilder.SetInsertPoint(catchPadBlock);
		auto catchPadInst = irBuilder.CreateCatchPad(catchSwitchInst,
													 {llvm::Constant::getNullValue(llvmContext.i8PtrType),
													  llvm::ConstantInt::get(llvmContext.i32Type, 64),
													  llvm::Constant::getNullValue(llvmContext.i8PtrType)});

		tryStack.push_back(TryContext{dispatchBlock});
		tryTableStack.push_back(TryTableContext{catchSwitchInst, catchPadInst, imm.catchTableIndex});
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
		landingPadInst->addClause(::llvm::ConstantPointerNull::get(irBuilder.getPtrTy()));

		tryStack.push_back(TryContext{landingPadBlock});
		tryTableStack.push_back(TryTableContext{landingPadInst, landingPadBlock, imm.catchTableIndex});
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
	else { irBuilder.CreateCondBr(isUserExceptionType, checkBlocks[0], noMatchBlock); }

	// Emit each catch clause's check.
	for(Uptr clauseIndex = 0; clauseIndex < catchClauses.size(); ++clauseIndex)
	{
		const IR::CatchClause& catchClause = catchClauses[clauseIndex];
		irBuilder.SetInsertPoint(checkBlocks[clauseIndex]);

		llvm::BasicBlock* nextBlock = clauseIndex + 1 < catchClauses.size()
										  ? checkBlocks[clauseIndex + 1]
										  : noMatchBlock;
		if(catchClause.kind == IR::CatchClauseKind::catch_
		   || catchClause.kind == IR::CatchClauseKind::catch_ref)
		{
			auto ehtagId = ::WAVM::LLVMJIT::wavmCreateLoad(
				irBuilder,
				llvmContext.i64Type,
				irBuilder.CreateGEP(llvmContext.i8Type,
									unwindehptr,
									{::llvm::ConstantInt::get(llvmContext.i64Type, EhTagOffset)}));
			auto isehtagId = irBuilder.CreateICmpEQ(
				ehtagId,
				::llvm::ConstantInt::get(
					llvmContext.i64Type, irModule.tagSegments[catchClause.exceptionTypeIndex].tagindex));
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
			llvm::Value* payload
				= coerceI64ToValueType(irBuilder, llvmContext, userData, target.params[argIndex]);
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
	irBuilder.CreateResume(tryTableContext.landingPadInst);
#endif

	irBuilder.SetInsertPoint(savedInsertionPoint);
#if defined(_MSC_VER)
	WAVM_ASSERT(!funcletPadStack.empty());
	funcletPadStack.pop_back();
#endif
	tryTableStack.pop_back();
}
#if 1
[[maybe_unused]]
static inline void foodebugging(EmitFunctionContext& functionContext, ::llvm::Value* memaddress)
{
	functionContext.emitRuntimeIntrinsic(
		"wavmdebuggingprint",
		FunctionType(
			TypeTuple{ValueType::i64}, TypeTuple{ValueType::i64}, IR::CallingConvention::intrinsic),
		{memaddress});
}
#endif
void EmitFunctionContext::catch_(ExceptionTypeImm imm)
{
	WAVM_ASSERT(!controlStack.empty());
	WAVM_ASSERT(!catchStack.empty());
	ControlContext& controlContext = controlStack.back();
	CatchContext& catchContext = catchStack.back();
	WAVM_ASSERT(controlContext.type == ControlContext::Type::try_
				|| controlContext.type == ControlContext::Type::catch_);
	if(controlContext.type == ControlContext::Type::try_)
	{
		WAVM_ASSERT(!tryStack.empty());
		tryStack.pop_back();
	}

	branchToEndOfControlContext();

	// Look up the exception type instance to be caught
	WAVM_ASSERT(imm.exceptionTypeIndex < irModule.tagSegments.size());

	auto& tagseg{irModule.tagSegments[imm.exceptionTypeIndex]};

	catchContext.landingPadInst->addClause(::llvm::ConstantPointerNull::get(irBuilder.getPtrTy()));
	irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);
	auto catchBlock = llvm::BasicBlock::Create(llvmContext, "catchtag", function);
	auto unhandledBlock = llvm::BasicBlock::Create(llvmContext, "unhandledtag", function);
	auto unwindehptr = irBuilder.CreateExtractValue(catchContext.landingPadInst, {0});
	auto magic = ::WAVM::LLVMJIT::wavmCreateLoad(irBuilder, llvmContext.i64Type, unwindehptr);
	auto isUserExceptionType = irBuilder.CreateICmpEQ(
		magic, ::llvm::ConstantInt::get(llvmContext.i64Type, exceptionclass));

	auto catchchecktagBlock = llvm::BasicBlock::Create(llvmContext, "catchchecktag", function);
	irBuilder.CreateCondBr(isUserExceptionType, catchchecktagBlock, unhandledBlock);
	irBuilder.SetInsertPoint(catchchecktagBlock);

	auto ehtagId = ::WAVM::LLVMJIT::wavmCreateLoad(
		irBuilder,
		llvmContext.i64Type,
		irBuilder.CreateGEP(llvmContext.i8Type,
							unwindehptr,
							{::llvm::ConstantInt::get(llvmContext.i64Type, EhTagOffset)}));
	auto isehtagId = irBuilder.CreateICmpEQ(
		ehtagId, ::llvm::ConstantInt::get(llvmContext.i64Type, tagseg.tagindex));

	irBuilder.CreateCondBr(isehtagId, catchBlock, unhandledBlock);
	catchContext.nextHandlerBlock = unhandledBlock;
	irBuilder.SetInsertPoint(catchBlock);

	auto argument = ::WAVM::LLVMJIT::wavmCreateLoad(
		irBuilder,
		llvmContext.i64Type,
		irBuilder.CreateGEP(llvmContext.i8Type,
							unwindehptr,
							{::llvm::ConstantInt::get(llvmContext.i64Type, UserDataOffset)}));
	push(argument);

	// Change the top of the control stack to a catch clause.
	controlContext.type = ControlContext::Type::catch_;
	controlContext.isReachable = true;
}

void EmitFunctionContext::catch_all(NoImm)
{
	WAVM_ASSERT(!controlStack.empty());
	WAVM_ASSERT(!catchStack.empty());
	ControlContext& controlContext = controlStack.back();
	CatchContext& catchContext = catchStack.back();
	WAVM_ASSERT(controlContext.type == ControlContext::Type::try_
				|| controlContext.type == ControlContext::Type::catch_);
	if(controlContext.type == ControlContext::Type::try_)
	{
		WAVM_ASSERT(!tryStack.empty());
		tryStack.pop_back();
	}

	branchToEndOfControlContext();
	catchContext.landingPadInst->addClause(::llvm::ConstantPointerNull::get(irBuilder.getPtrTy()));
	irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);
	auto catchBlock = llvm::BasicBlock::Create(llvmContext, "catchall", function);
	auto unhandledBlock = llvm::BasicBlock::Create(llvmContext, "unhandledall", function);

	auto unwindehptr = irBuilder.CreateExtractValue(catchContext.landingPadInst, {0});
	auto magic = ::WAVM::LLVMJIT::wavmCreateLoad(irBuilder, llvmContext.i64Type, unwindehptr);
	auto isUserExceptionType = irBuilder.CreateICmpEQ(
		magic, ::llvm::ConstantInt::get(llvmContext.i64Type, exceptionclass));
	irBuilder.CreateCondBr(isUserExceptionType, catchBlock, unhandledBlock);
	catchContext.nextHandlerBlock = unhandledBlock;
	irBuilder.SetInsertPoint(catchBlock);

	// Change the top of the control stack to a catch clause.
	controlContext.type = ControlContext::Type::catch_;
	controlContext.isReachable = true;
}

void EmitFunctionContext::throw_(ExceptionTypeImm imm)
{
	auto ehptr = pop();
	auto& tagseg{irModule.tagSegments[imm.exceptionTypeIndex]};

	auto ehtagfunc = getWavmThrowWasmEhtagFunction(moduleContext);
	emitRaiseFunctionCall(ehtagfunc,
						  {::llvm::ConstantInt::get(llvmContext.i64Type, tagseg.tagindex), ehptr});
	enterUnreachable();
}

void EmitFunctionContext::throw_ref(NoImm)
{
	auto exnref = pop();

	auto rethrowFunc = getWavmRethrowWasmEhtagFunction(moduleContext);
	emitRaiseFunctionCall(rethrowFunc, {exnref});
	enterUnreachable();
}

void EmitFunctionContext::rethrow(RethrowImm imm)
{
	// 'rethrow $depth' rethrows the exception caught by the catch clause at the given label depth,
	// which is validated to be a catch handler. Resuming its landingpad continues unwinding that
	// exception to the enclosing handlers.
	WAVM_ASSERT(imm.catchDepth < controlStack.size());
	ControlContext& catchContext = controlStack[controlStack.size() - imm.catchDepth - 1];
	WAVM_ASSERT(catchContext.type == ControlContext::Type::catch_);
	WAVM_ASSERT(catchContext.landingPadInst);
	irBuilder.CreateResume(catchContext.landingPadInst);
	enterUnreachable();
}

void EmitFunctionContext::delegate(BranchImm)
{
	CatchContext& catchContext = catchStack.back();
	{
		::llvm::IRBuilderBase::InsertPointGuard guard(irBuilder);
		irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);
		catchContext.landingPadInst->setCleanup(true);
	}
	this->end(NoImm{});
}
