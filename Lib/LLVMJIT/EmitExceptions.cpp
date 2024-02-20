#include <stddef.h>
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

static llvm::Function* getCXABeginCatchFunction(EmitModuleContext& moduleContext)
{
	if(!moduleContext.cxaBeginCatchFunction)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		moduleContext.cxaBeginCatchFunction = llvm::Function::Create(
			llvm::FunctionType::get(llvmContext.i8PtrType, {llvmContext.i8PtrType}, false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"__cxa_begin_catch",
			moduleContext.llvmModule);
	}
	return moduleContext.cxaBeginCatchFunction;
}

static llvm::Function* getCXAEndCatchFunction(EmitModuleContext& moduleContext)
{
	if(!moduleContext.cxaEndCatchFunction)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		moduleContext.cxaEndCatchFunction = llvm::Function::Create(
			llvm::FunctionType::get(llvm::Type::getVoidTy(llvmContext), false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"__cxa_end_catch",
			moduleContext.llvmModule);
	}
	return moduleContext.cxaEndCatchFunction;
}

void EmitFunctionContext::endTryWithoutCatch()
{
	WAVM_ASSERT(!tryStack.empty());
	__builtin_printf("%s %s %d %zu\n",__FILE__,__PRETTY_FUNCTION__,__LINE__,tryStack.size());
	tryStack.pop_back();
	__builtin_printf("%s %s %d %zu\n",__FILE__,__PRETTY_FUNCTION__,__LINE__,tryStack.size());
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
	if(!tryStack.empty()) {
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
	auto& moduleContext{emitFunctionContext.moduleContext};
	auto& function{emitFunctionContext.function};
	auto& tryStack{emitFunctionContext.tryStack};
	auto& catchStack{emitFunctionContext.catchStack};
	if(moduleContext.useWindowsSEH)
	{ // Todo : MSVC ABI support after i figured out how the thing works out.
		// Insert an alloca for the exception pointer at the beginning of the function.
		irBuilder.SetInsertPoint(&function->getEntryBlock(),
								 function->getEntryBlock().getFirstInsertionPt());
		llvm::Value* exceptionPointerAlloca
			= irBuilder.CreateAlloca(llvmContext.i8PtrType, nullptr, "exceptionPointer");

		// Create a BasicBlock with a CatchSwitch instruction to use as the unwind target.
		auto catchSwitchBlock = llvm::BasicBlock::Create(llvmContext, "catchSwitch", function);
		irBuilder.SetInsertPoint(catchSwitchBlock);
		auto catchSwitchInst
			= irBuilder.CreateCatchSwitch(llvm::ConstantTokenNone::get(llvmContext), nullptr, 1);

		// Create a block+catchpad that the catchswitch will transfer control if the exception type
		// info matches a WAVM runtime exception.
		auto catchPadBlock = llvm::BasicBlock::Create(llvmContext, "catchPad", function);
		catchSwitchInst->addHandler(catchPadBlock);
		irBuilder.SetInsertPoint(catchPadBlock);
		auto catchPadInst = irBuilder.CreateCatchPad(catchSwitchInst,
													 {moduleContext.runtimeExceptionTypeInfo,
													  emitLiteral(llvmContext, I32(0)),
													  exceptionPointerAlloca});

		// Create a catchret that immediately returns from the catch "funclet" to a new non-funclet
		// basic block.
		auto catchBlock = llvm::BasicBlock::Create(llvmContext, "catch", function);
		irBuilder.CreateCatchRet(catchPadInst, catchBlock);
		irBuilder.SetInsertPoint(catchBlock);

		// Load the exception pointer from the alloca that the catchpad wrote it to.
		auto exceptionPointer
			= emitFunctionContext.loadFromUntypedPointer(exceptionPointerAlloca, llvmContext.i8PtrType);
		auto exceptionTypeId = emitFunctionContext.loadFromUntypedPointer(

		// Load the exception type ID.
			::WAVM::LLVMJIT::wavmCreateInBoundsGEP(
				irBuilder,
				llvmContext.i8Type,
				exceptionPointer,
				{emitLiteralIptr(offsetof(Exception, typeId), moduleContext.iptrType)}),
			moduleContext.iptrType);

		tryStack.push_back(TryContext{catchSwitchBlock});
		catchStack.push_back(
			CatchContext{catchSwitchInst, nullptr, exceptionPointer, catchBlock, exceptionTypeId});
	}
	else
	{
		// Create a BasicBlock with a LandingPad instruction to use as the unwind target.
		auto landingPadBlock = llvm::BasicBlock::Create(llvmContext, "landingPad", function);
		irBuilder.SetInsertPoint(landingPadBlock);
		auto landingPadInst = irBuilder.CreateLandingPad(
			llvm::StructType::get(llvmContext, {llvmContext.i8PtrType, llvmContext.i32Type}), 1);
		//landingPadInst->addClause(moduleContext.runtimeExceptionTypeInfo);

#if 0
		// Call __cxa_begin_catch to get the exception pointer.
		auto exceptionPointer
			= irBuilder.CreateCall(getCXABeginCatchFunction(moduleContext),
								   {irBuilder.CreateExtractValue(landingPadInst, {0})});
		// Load the exception type ID.
		auto ehtagId = emitFunctionContext.loadFromUntypedPointer(exceptionPointer, moduleContext.iptrType);
		// Call __cxa_end_catch immediately to free memory used to throw the exception.
		irBuilder.CreateCall(getCXAEndCatchFunction(moduleContext));
		tryStack.push_back(TryContext{landingPadBlock});
		catchStack.push_back(
			CatchContext{nullptr, landingPadInst, exceptionPointer, landingPadBlock, ehtagId});
#else
#if 0
		tryStack.push_back(TryContext{landingPadBlock});
		auto exceptionPointer = llvm::ConstantPointerNull::get(llvmContext.i8PtrType);
		auto ehtagId = llvm::ConstantInt::get(llvmContext.i8Type, 0);
		catchStack.push_back(
			CatchContext{nullptr, landingPadInst, exceptionPointer, landingPadBlock, ehtagId});
#else
		tryStack.push_back(TryContext{landingPadBlock});
		catchStack.push_back(
			CatchContext{nullptr, landingPadInst, nullptr, landingPadBlock, nullptr});
#endif
		__builtin_printf("%s %s %d\n",__FILE__,__PRETTY_FUNCTION__,__LINE__);
#endif
	}
}

void EmitFunctionContext::try_(ControlStructureImm imm)
{
	auto originalInsertBlock = irBuilder.GetInsertBlock();
#if 0
	if(moduleContext.useWindowsSEH)
	{ // Todo : MSVC ABI support after i figured out how the thing works out.
		// Insert an alloca for the exception pointer at the beginning of the function.
		irBuilder.SetInsertPoint(&function->getEntryBlock(),
								 function->getEntryBlock().getFirstInsertionPt());
		llvm::Value* exceptionPointerAlloca
			= irBuilder.CreateAlloca(llvmContext.i8PtrType, nullptr, "exceptionPointer");

		// Create a BasicBlock with a CatchSwitch instruction to use as the unwind target.
		auto catchSwitchBlock = llvm::BasicBlock::Create(llvmContext, "catchSwitch", function);
		irBuilder.SetInsertPoint(catchSwitchBlock);
		auto catchSwitchInst
			= irBuilder.CreateCatchSwitch(llvm::ConstantTokenNone::get(llvmContext), nullptr, 1);

		// Create a block+catchpad that the catchswitch will transfer control if the exception type
		// info matches a WAVM runtime exception.
		auto catchPadBlock = llvm::BasicBlock::Create(llvmContext, "catchPad", function);
		catchSwitchInst->addHandler(catchPadBlock);
		irBuilder.SetInsertPoint(catchPadBlock);
		auto catchPadInst = irBuilder.CreateCatchPad(catchSwitchInst,
													 {moduleContext.runtimeExceptionTypeInfo,
													  emitLiteral(llvmContext, I32(0)),
													  exceptionPointerAlloca});

		// Create a catchret that immediately returns from the catch "funclet" to a new non-funclet
		// basic block.
		auto catchBlock = llvm::BasicBlock::Create(llvmContext, "catch", function);
		irBuilder.CreateCatchRet(catchPadInst, catchBlock);
		irBuilder.SetInsertPoint(catchBlock);

		// Load the exception pointer from the alloca that the catchpad wrote it to.
		auto exceptionPointer
			= loadFromUntypedPointer(exceptionPointerAlloca, llvmContext.i8PtrType);

		// Load the exception type ID.
		auto exceptionTypeId = loadFromUntypedPointer(
			::WAVM::LLVMJIT::wavmCreateInBoundsGEP(
				irBuilder,
				llvmContext.i8Type,
				exceptionPointer,
				{emitLiteralIptr(offsetof(Exception, typeId), moduleContext.iptrType)}),
			moduleContext.iptrType);

		tryStack.push_back(TryContext{catchSwitchBlock});
		catchStack.push_back(
			CatchContext{catchSwitchInst, nullptr, exceptionPointer, catchBlock, exceptionTypeId});
	}
	else
	{
		// Create a BasicBlock with a LandingPad instruction to use as the unwind target.
		auto landingPadBlock = llvm::BasicBlock::Create(llvmContext, "landingPad", function);
		irBuilder.SetInsertPoint(landingPadBlock);
		auto landingPadInst = irBuilder.CreateLandingPad(
			llvm::StructType::get(llvmContext, {llvmContext.i8PtrType, llvmContext.i32Type}), 1);
		landingPadInst->addClause(moduleContext.runtimeExceptionTypeInfo);

		// Call __cxa_begin_catch to get the exception pointer.
		auto exceptionPointer
			= irBuilder.CreateCall(getCXABeginCatchFunction(moduleContext),
								   {irBuilder.CreateExtractValue(landingPadInst, {0})});
		// Load the exception type ID.
		auto ehtagId = loadFromUntypedPointer(exceptionPointer, moduleContext.iptrType);
		// Call __cxa_end_catch immediately to free memory used to throw the exception.
		irBuilder.CreateCall(getCXAEndCatchFunction(moduleContext));
		tryStack.push_back(TryContext{landingPadBlock});
		catchStack.push_back(
			CatchContext{nullptr, landingPadInst, exceptionPointer, landingPadBlock, ehtagId});
	}
#else
	generate_catch_common(*this);
#endif
	irBuilder.SetInsertPoint(originalInsertBlock);
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
#if 0
	const IR::ExceptionType catchType = irModule.exceptionTypes.getType(imm.exceptionTypeIndex);
	llvm::Constant* catchTypeId = moduleContext.exceptionTypeIds[imm.exceptionTypeIndex];

	irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);
	auto isExceptionType = irBuilder.CreateICmpEQ(catchContext.exceptionTypeId, catchTypeId);

	auto catchBlock = llvm::BasicBlock::Create(llvmContext, "catch", function);
	auto unhandledBlock = llvm::BasicBlock::Create(llvmContext, "unhandled", function);
	irBuilder.CreateCondBr(isExceptionType, catchBlock, unhandledBlock);
	catchContext.nextHandlerBlock = unhandledBlock;
	irBuilder.SetInsertPoint(catchBlock);

	// Push the exception arguments on the stack.
	for(Uptr argumentIndex = 0; argumentIndex < catchType.params.size(); ++argumentIndex)
	{
		const ValueType parameters = catchType.params[argumentIndex];
		const Uptr argOffset
			= offsetof(Exception, arguments)
			  + (catchType.params.size() - argumentIndex - 1) * sizeof(Exception::arguments[0]);
		auto argument = loadFromUntypedPointer(
			::WAVM::LLVMJIT::wavmCreateInBoundsGEP(irBuilder,
												   llvmContext.i8Type,
												   catchContext.exceptionPointer,
												   {emitLiteral(llvmContext, argOffset)}),
			asLLVMType(llvmContext, parameters),
			sizeof(Exception::arguments[0]));
		push(argument);
	}
#else
	auto& tagseg{irModule.tagSegments[imm.exceptionTypeIndex]};
	llvm::Constant* catchTypeId = ::llvm::ConstantInt::get(llvmContext.i64Type, tagseg.tagindex);

	catchContext.landingPadInst->addClause(moduleContext.runtimeExceptionTypeInfo);

	irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);

	auto exceptionPointer
		= irBuilder.CreateCall(getCXABeginCatchFunction(moduleContext),
								{irBuilder.CreateExtractValue(catchContext.landingPadInst, {0})});
	// Load the exception type ID.
	auto ehtagId = loadFromUntypedPointer(exceptionPointer, moduleContext.iptrType);
	// Call __cxa_end_catch immediately to free memory used to throw the exception.
	irBuilder.CreateCall(getCXAEndCatchFunction(moduleContext));
	auto isExceptionType = irBuilder.CreateICmpEQ(ehtagId, catchTypeId);

	auto catchBlock = llvm::BasicBlock::Create(llvmContext, "catch", function);
	auto unhandledBlock = llvm::BasicBlock::Create(llvmContext, "unhandled", function);
	irBuilder.CreateCondBr(isExceptionType, catchBlock, unhandledBlock);
	catchContext.nextHandlerBlock = unhandledBlock;
	irBuilder.SetInsertPoint(catchBlock);
	auto argument = loadFromUntypedPointer(
		::WAVM::LLVMJIT::wavmCreateInBoundsGEP(
			irBuilder,
			llvmContext.i8Type,
			exceptionPointer,
			{emitLiteralIptr(__builtin_offsetof(::WAVM::Runtime::ExceptionTypeTag, ehptr),
							 moduleContext.iptrType)}),
		moduleContext.iptrType);
	push(argument);
#endif

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
#if 0
	irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);
#if 0
	auto isUserExceptionType = irBuilder.CreateICmpNE(
		loadFromUntypedPointer(
			::WAVM::LLVMJIT::wavmCreateInBoundsGEP(
				irBuilder,
				llvmContext.i8Type,
				catchContext.exceptionPointer,
				{emitLiteralIptr(offsetof(Exception, isUserException), moduleContext.iptrType)}),
			llvmContext.i8Type),
		llvm::ConstantInt::get(llvmContext.i8Type, llvm::APInt(8, 0, false)));
#elif 1
	auto isUserExceptionType = irBuilder.CreateICmpNE(
		loadFromUntypedPointer(catchContext.exceptionPointer, llvmContext.i8Type),
		);
#endif
	auto catchBlock = llvm::BasicBlock::Create(llvmContext, "catch", function);
	auto unhandledBlock = llvm::BasicBlock::Create(llvmContext, "unhandled", function);
	irBuilder.CreateCondBr(isUserExceptionType, catchBlock, unhandledBlock);
	catchContext.nextHandlerBlock = unhandledBlock;
	irBuilder.SetInsertPoint(catchBlock);
#else
#endif

	// Change the top of the control stack to a catch clause.
	controlContext.type = ControlContext::Type::catch_;
	controlContext.isReachable = true;
}

void EmitFunctionContext::throw_(ExceptionTypeImm imm)
{
	auto ehptr = pop();
	auto& tagseg{irModule.tagSegments[imm.exceptionTypeIndex]};

	emitRuntimeIntrinsic("throwExceptionTag",
						 FunctionType(TypeTuple{},
									  TypeTuple{ValueType::i64, moduleContext.iptrValueType},
									  IR::CallingConvention::intrinsic),
						 {::llvm::ConstantInt::get(llvmContext.i64Type, tagseg.tagindex), ehptr});
	__builtin_printf("%s %s %d\n",__FILE__,__PRETTY_FUNCTION__,__LINE__);
	irBuilder.CreateUnreachable();
	enterUnreachable();
}

void EmitFunctionContext::rethrow(RethrowImm imm)
{
	WAVM_ASSERT(imm.catchDepth < catchStack.size());
	CatchContext& catchContext = catchStack[catchStack.size() - imm.catchDepth - 1];
#if 0
	emitRuntimeIntrinsic(
		"throwException",
		FunctionType(
			TypeTuple{}, TypeTuple{moduleContext.iptrValueType}, IR::CallingConvention::intrinsic),
		{irBuilder.CreatePtrToInt(catchContext.exceptionPointer, moduleContext.iptrType)});
#elif 0
	auto exceptionPointer = catchContext.exceptionPointer;
	auto ehtagId = loadFromUntypedPointer(exceptionPointer, moduleContext.iptrType);
	auto argument = loadFromUntypedPointer(
		::WAVM::LLVMJIT::wavmCreateInBoundsGEP(
			irBuilder,
			llvmContext.i8Type,
			catchContext.exceptionPointer,
			{emitLiteralIptr(__builtin_offsetof(::WAVM::Runtime::ExceptionTypeTag, ehptr),
							 moduleContext.iptrType)}),
		moduleContext.iptrType);
	emitRuntimeIntrinsic("throwExceptionTag",
						 FunctionType(TypeTuple{},
									  TypeTuple{ValueType::i64, moduleContext.iptrValueType},
									  IR::CallingConvention::intrinsic),
						 {ehtagId, argument});
#endif
	__builtin_printf("%s %s %d\n",__FILE__,__PRETTY_FUNCTION__,__LINE__);
	irBuilder.CreateUnreachable();
	enterUnreachable();
}

void EmitFunctionContext::delegate(BranchImm imm)
{
#if 0
	__builtin_printf("%s %s %d catchStack.size():%zu imm.targetDepth=%zu\n",__FILE__,__PRETTY_FUNCTION__,__LINE__,catchStack.size(),imm.targetDepth);
#endif
	CatchContext& catchContext = catchStack.back();

	llvm::BasicBlock* savedInsertionPoint = irBuilder.GetInsertBlock();
	irBuilder.SetInsertPoint(catchContext.nextHandlerBlock);
#if 0
//	irBuilder.CreateCatchSwitch(catchContext.landingPadInst,nullptr,imm.targetDepth);
#endif
	irBuilder.CreateResume(catchContext.landingPadInst);

	irBuilder.SetInsertPoint(savedInsertionPoint);

	this->end(NoImm{});

}