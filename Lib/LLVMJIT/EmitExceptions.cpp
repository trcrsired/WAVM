#include <stddef.h>
#include <unwind.h>
#include <cstdint>
#include <cstdlib>
#include <memory>
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
#include "WAVM/Runtime/ExceptionTypeTag.h"
#include "WAVM/RuntimeABI/RuntimeABI.h"

PUSH_DISABLE_WARNINGS_FOR_LLVM_HEADERS
#include <llvm/ADT/APInt.h>
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

	inline
#ifdef __cpp_constinit
		constinit
#endif
#if defined(__GNUC__) || defined(__clang__)
		__thread
#else
		thread_local
#endif
		_Unwind_Exception unwdexceptiontable{exceptionclass};

#if defined(__SEH__) && !defined(__USING_SJLJ_EXCEPTIONS__)
	inline constexpr ::std::size_t Unwind_Exception_Private1_Offset{
		__builtin_offsetof(_Unwind_Exception, private_)};
	inline constexpr ::std::size_t Unwind_Exception_Private2_Offset{Unwind_Exception_Private1_Offset
																	+ sizeof(::std::uintptr_t)};
#else
	inline constexpr ::std::size_t Unwind_Exception_Private1_Offset{
		__builtin_offsetof(_Unwind_Exception, private_1)};
	inline constexpr ::std::size_t Unwind_Exception_Private2_Offset{
		__builtin_offsetof(_Unwind_Exception, private_2)};
#endif

}

extern "C" void wavm_throw_wasm_ehtag(::std::uint_least64_t tag, ::std::uint_least64_t value)
{
#if defined(__SEH__) && !defined(__USING_SJLJ_EXCEPTIONS__)
	if constexpr(sizeof(::std::uintptr_t) < sizeof(::std::uint_least64_t))
	{
		constexpr ::std::uint_least64_t mxval{::std::numeric_limits<::std::uintptr_t>::max()};
		if(mxval < tag || value < mxval) { ::std::abort(); }
		*unwdexceptiontable.private_ = static_cast<::std::uintptr_t>(tag);
		unwdexceptiontable.private_[1] = static_cast<::std::uintptr_t>(value);
	}
	else
	{
		*unwdexceptiontable.private_ = tag;
		unwdexceptiontable.private_[1] = value;
	}

#else
	if constexpr(sizeof(::std::uintptr_t) < sizeof(::std::uint_least64_t))
	{
		constexpr ::std::uint_least64_t mxval{::std::numeric_limits<::std::uintptr_t>::max()};
		if(mxval < tag || value < mxval) { ::std::abort(); }
		unwdexceptiontable.private_1 = static_cast<::std::uintptr_t>(tag);
		unwdexceptiontable.private_2 = static_cast<::std::uintptr_t>(value);
	}
	else
	{
		unwdexceptiontable.private_1 = tag;
		unwdexceptiontable.private_2 = value;
	}
#endif
	_Unwind_RaiseException(__builtin_addressof(unwdexceptiontable));
}

static llvm::Function* getWavmThrowWasmEhtagFunction(EmitModuleContext& moduleContext)
{
	if(!moduleContext.wavmThrowWasmEhtagFunction)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		moduleContext.wavmThrowWasmEhtagFunction = llvm::Function::Create(
			llvm::FunctionType::get(
				llvm::Type::getVoidTy(llvmContext),
				{::llvm::Type::getInt64Ty(llvmContext), ::llvm::Type::getInt64Ty(llvmContext)},
				false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"wavm_throw_wasm_ehtag",
			moduleContext.llvmModule);
	}
	return moduleContext.wavmThrowWasmEhtagFunction;
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

	exitCatch();

	// If an end instruction terminates a sequence of catch clauses, terminate the chain of
	// handler type ID tests by rethrowing the exception if its type ID didn't match any of the
	// handlers.
	llvm::BasicBlock* savedInsertionPoint = irBuilder.GetInsertBlock();
	irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);
#if 0
	emitRuntimeIntrinsic(
		"throwException",
		FunctionType(
			TypeTuple{}, TypeTuple{moduleContext.iptrValueType}, CallingConvention::intrinsic),
		{irBuilder.CreatePtrToInt(catchContext.exceptionPointer, moduleContext.iptrType)});
#endif
	irBuilder.CreateUnreachable();

	irBuilder.SetInsertPoint(savedInsertionPoint);
	catchStack.pop_back();
}

void EmitFunctionContext::exitCatch()
{
#if 0
	ControlContext& currentContext = controlStack.back();
	WAVM_ASSERT(currentContext.type == ControlContext::Type::catch_);
	WAVM_ASSERT(!catchStack.empty());
	CatchContext& catchContext = catchStack.back();

	if(currentContext.isReachable)
	{
		// Destroy the exception caught by the previous catch clause.
		emitRuntimeIntrinsic(
			"destroyException",
			FunctionType(
				TypeTuple{}, TypeTuple{moduleContext.iptrValueType}, CallingConvention::intrinsic),
			{irBuilder.CreatePtrToInt(catchContext.exceptionPointer, moduleContext.iptrType)});
	}
#endif
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

	// Push a branch target for the end block/phi.
	pushBranchTarget(blockType.results(), endBlock, endPHIs);

	// Repush the try arguments.
	pushMultiple(tryArgs, blockType.params().size());
}

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
	else { exitCatch(); }

	branchToEndOfControlContext();

	// Look up the exception type instance to be caught
	WAVM_ASSERT(imm.exceptionTypeIndex < irModule.tagSegments.size());

	auto& tagseg{irModule.tagSegments[imm.exceptionTypeIndex]};

	auto unwindehptr = irBuilder.CreateExtractValue(catchContext.landingPadInst, {0});
	auto magic = ::WAVM::LLVMJIT::wavmCreateLoad(irBuilder, llvmContext.i64Type, unwindehptr);
	auto isUserExceptionType = irBuilder.CreateICmpEQ(
		magic, ::llvm::ConstantInt::get(llvmContext.i64Type, exceptionclass));

	auto ehtagId = ::WAVM::LLVMJIT::wavmCreateLoad(
		irBuilder,
		llvmContext.i64Type,
		irBuilder.CreateGEP(
			llvmContext.i8Type,
			unwindehptr,
			{::llvm::ConstantInt::get(llvmContext.i64Type, Unwind_Exception_Private1_Offset)}));
	auto isehtagId = irBuilder.CreateICmpEQ(
		ehtagId, ::llvm::ConstantInt::get(llvmContext.i64Type, imm.exceptionTypeIndex));

	auto catchBlock = llvm::BasicBlock::Create(llvmContext, "catch", function);
	auto unhandledBlock = llvm::BasicBlock::Create(llvmContext, "unhandled", function);
	irBuilder.CreateCondBr(isehtagId, catchBlock, unhandledBlock);
	catchContext.nextHandlerBlock = unhandledBlock;

	irBuilder.SetInsertPoint(catchBlock);

	auto argument = ::WAVM::LLVMJIT::wavmCreateLoad(
		irBuilder,
		llvmContext.i64Type,
		irBuilder.CreateGEP(
			llvmContext.i8Type,
			unwindehptr,
			{::llvm::ConstantInt::get(llvmContext.i64Type, Unwind_Exception_Private2_Offset)}));
	push(argument);

	llvm::Constant* catchTypeId = ::llvm::ConstantInt::get(llvmContext.i64Type, tagseg.tagindex);

	catchContext.landingPadInst->setCleanup(false);
	catchContext.landingPadInst->addClause(::llvm::ConstantPointerNull::get(irBuilder.getPtrTy()));

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
	else { exitCatch(); }

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
	irBuilder.CreateCall(ehtagfunc,
						 {::llvm::ConstantInt::get(llvmContext.i64Type, tagseg.tagindex), ehptr});

	irBuilder.CreateUnreachable();
	enterUnreachable();
}

void EmitFunctionContext::rethrow(RethrowImm imm)
{
	WAVM_ASSERT(imm.catchDepth < catchStack.size());
	CatchContext& catchContext = catchStack[catchStack.size() - imm.catchDepth - 1];

	irBuilder.CreateResume(catchContext.landingPadInst);
	irBuilder.CreateUnreachable();
	enterUnreachable();
}

void EmitFunctionContext::delegate(BranchImm imm)
{
	CatchContext& catchContext = catchStack.back();
	{
		::llvm::IRBuilderBase::InsertPointGuard guard(irBuilder);
		//	llvm::BasicBlock* savedInsertionPoint = irBuilder.GetInsertBlock();
		irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);

		catchContext.landingPadInst->setCleanup(true);
		irBuilder.CreateResume(catchContext.landingPadInst);
		//	catchContext.nextHandlerBlock=nullptr;
	}
	this->end(NoImm{});
}