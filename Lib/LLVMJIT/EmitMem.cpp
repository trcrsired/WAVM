#include "EmitContext.h"
#include "EmitFunctionContext.h"
#include "EmitModuleContext.h"
#include "LLVMJITPrivate.h"
#include "WAVM/IR/IR.h"
#include "WAVM/IR/Operators.h"
#include "WAVM/IR/Types.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"

PUSH_DISABLE_WARNINGS_FOR_LLVM_HEADERS
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/AtomicOrdering.h>

#if LLVM_VERSION_MAJOR >= 10
#include <llvm/IR/IntrinsicsAArch64.h>
#endif
POP_DISABLE_WARNINGS_FOR_LLVM_HEADERS

using namespace WAVM;
using namespace WAVM::IR;
using namespace WAVM::LLVMJIT;

enum class BoundsCheckOp
{
	clampToGuardRegion,
	trapOnOutOfBounds
};

static llvm::Value* getMemoryNumPages(EmitFunctionContext& functionContext, Uptr memoryIndex)
{
	llvm::Constant* memoryOffset = functionContext.moduleContext.memoryOffsets[memoryIndex];

	// Load the number of memory pages from the compartment runtime data.
	llvm::LoadInst* memoryNumPagesLoad = functionContext.loadFromUntypedPointer(
		::WAVM::LLVMJIT::wavmCreateInBoundsGEP(
			functionContext.irBuilder,
			functionContext.irBuilder.getInt8Ty(),
			functionContext.getCompartmentAddress(),
			{llvm::ConstantExpr::getAdd(
				memoryOffset,
				emitLiteralIptr(offsetof(Runtime::MemoryRuntimeData, numPages),
								functionContext.moduleContext.iptrType))}),
		functionContext.moduleContext.iptrType,
		functionContext.moduleContext.iptrAlignment);
	memoryNumPagesLoad->setAtomic(llvm::AtomicOrdering::Acquire);

	return memoryNumPagesLoad;
}

static llvm::Value* getMemoryNumBytes(EmitFunctionContext& functionContext, Uptr memoryIndex)
{
	return functionContext.irBuilder.CreateMul(
		getMemoryNumPages(functionContext, memoryIndex),
		emitLiteralIptr(IR::numBytesPerPage, functionContext.moduleContext.iptrType));
}

#if 0
[[maybe_unused]]
static inline void foomemorytagdebugging(EmitFunctionContext& functionContext,
										 ::llvm::Value* memaddress)
{
	functionContext.emitRuntimeIntrinsic(
		"wavmdebuggingprint",
		FunctionType(
			TypeTuple{ValueType::i64}, TypeTuple{ValueType::i64}, IR::CallingConvention::intrinsic),
		{functionContext.irBuilder.CreateZExt(memaddress, functionContext.llvmContext.i64Type)});
}

static void createconditionaltrapcond(EmitFunctionContext& functionContext, ::llvm::Value* cmpres, ::llvm::Value* addressrshift)
{
	llvm::IRBuilder<>& irBuilder = functionContext.irBuilder;
	::llvm::BasicBlock* trapBlock
		= ::llvm::BasicBlock::Create(functionContext.moduleContext.llvmContext, "");
	::llvm::BasicBlock* normalBlock
		= ::llvm::BasicBlock::Create(functionContext.moduleContext.llvmContext, "");
	irBuilder.CreateCondBr(cmpres, trapBlock, normalBlock);
	// irBuilder.CreateBr(trapBlock);
	irBuilder.SetInsertPoint(trapBlock);
	foomemorytagdebugging(functionContext,addressrshift);
	irBuilder.CreateIntrinsic(::llvm::Intrinsic::trap, {}, {});
	irBuilder.CreateUnreachable();
	// irBuilder.CreateBr(normalBlock);
	irBuilder.SetInsertPoint(normalBlock);
}
#endif
static void createconditionaltrap(EmitFunctionContext& functionContext, ::llvm::Value* cmpres)
{
	llvm::IRBuilder<>& irBuilder = functionContext.irBuilder;
	::llvm::BasicBlock* trapBlock{functionContext.TrapBlock};
	auto* function = functionContext.function;
	if(trapBlock != nullptr)
	{
		::llvm::BasicBlock* normalBlock
			= ::llvm::BasicBlock::Create(functionContext.moduleContext.llvmContext, "", function);
		irBuilder.CreateCondBr(cmpres, trapBlock, normalBlock);
		// irBuilder.CreateBr(normalBlock);
		irBuilder.SetInsertPoint(normalBlock);
	}
	else
	{
		trapBlock
			= ::llvm::BasicBlock::Create(functionContext.moduleContext.llvmContext, "", function);
		::llvm::BasicBlock* normalBlock
			= ::llvm::BasicBlock::Create(functionContext.moduleContext.llvmContext, "", function);
		irBuilder.CreateCondBr(cmpres, trapBlock, normalBlock);
		// irBuilder.CreateBr(trapBlock);
		irBuilder.SetInsertPoint(trapBlock);
		irBuilder.CreateIntrinsic(::llvm::Intrinsic::trap, {}, {});
		irBuilder.CreateUnreachable();
		// irBuilder.CreateBr(normalBlock);
		irBuilder.SetInsertPoint(normalBlock);
		functionContext.TrapBlock = trapBlock;
	}
}

static llvm::Value* armmte32_to_64ptr_value(EmitFunctionContext& functionContext,
											Uptr memoryIndex,
											llvm::Value* memaddress)
{
	auto& irBuilder = functionContext.irBuilder;
	const MemoryType& memoryType
		= functionContext.moduleContext.irModule.memories.getType(memoryIndex);
	if(memoryType.indexType == IndexType::i32)
	{
		memaddress = irBuilder.CreateIntToPtr(
			irBuilder.CreateZExt(memaddress, functionContext.llvmContext.i64Type),
			functionContext.llvmContext.i8PtrType);
	}
	return memaddress;
}

static llvm::Value* armmte64_to_32_value(EmitFunctionContext& functionContext,
										 Uptr memoryIndex,
										 llvm::Value* memaddress64)
{
	auto& irBuilder = functionContext.irBuilder;
	const MemoryType& memoryType
		= functionContext.moduleContext.irModule.memories.getType(memoryIndex);
	if(memoryType.indexType == IndexType::i32)
	{
		constexpr ::std::uint_least64_t mask{0x0F00000000000000};
		constexpr ::std::uint_least64_t mask1{0x000000000FFFFFFF};
		memaddress64 = irBuilder.CreatePtrToInt(memaddress64, functionContext.llvmContext.i64Type);
		memaddress64 = irBuilder.CreateTrunc(
			irBuilder.CreateOr(irBuilder.CreateLShr(irBuilder.CreateAnd(memaddress64, mask), 28),
							   irBuilder.CreateAnd(memaddress64, mask1)),
			functionContext.llvmContext.i32Type);
	}
	return memaddress64;
}

static llvm::Value* armmte64_to_32_old_value(EmitFunctionContext& functionContext,
											 Uptr memoryIndex,
											 llvm::Value* olduntaggedmemaddress,
											 llvm::Value* memaddress64)
{
	auto& irBuilder = functionContext.irBuilder;
	const MemoryType& memoryType
		= functionContext.moduleContext.irModule.memories.getType(memoryIndex);
	constexpr ::std::uint_least64_t mask{0x0F00000000000000};
	if(memoryType.indexType == IndexType::i32)
	{
		memaddress64 = irBuilder.CreatePtrToInt(memaddress64, functionContext.llvmContext.i64Type);
		memaddress64 = irBuilder.CreateOr(
			irBuilder.CreateTrunc(irBuilder.CreateLShr(irBuilder.CreateAnd(memaddress64, mask), 28),
								  functionContext.llvmContext.i32Type),
			olduntaggedmemaddress);
	}
	else
	{
		memaddress64 = irBuilder.CreatePtrToInt(memaddress64, functionContext.llvmContext.i64Type);
		memaddress64
			= irBuilder.CreateOr(irBuilder.CreateAnd(memaddress64, mask), olduntaggedmemaddress);
	}
	return memaddress64;
}

static llvm::Function* getWavmMemtagTrapFunction(EmitFunctionContext& functionContext)
{
	auto& moduleContext{functionContext.moduleContext};
	if(!moduleContext.wavmMemtagTrapFunction)
	{
		LLVMContext& llvmContext = moduleContext.llvmContext;
		auto mtgfunc = llvm::Function::Create(
			llvm::FunctionType::get(llvm::Type::getVoidTy(llvmContext), {}, false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"wavm_memtag_trap_function",
			moduleContext.llvmModule);
		mtgfunc->addFnAttr(::llvm::Attribute::AttrKind::NoReturn);
		moduleContext.wavmMemtagTrapFunction = mtgfunc;
	}
	return moduleContext.wavmMemtagTrapFunction;
}

static void createmtgconditionaltrap(EmitFunctionContext& functionContext, ::llvm::Value* cmpres)
{
	llvm::IRBuilder<>& irBuilder = functionContext.irBuilder;
	::llvm::BasicBlock* trapBlock{functionContext.MtgTrapBlock};
	auto* function = functionContext.function;
	if(trapBlock != nullptr)
	{
		::llvm::BasicBlock* normalBlock
			= ::llvm::BasicBlock::Create(functionContext.moduleContext.llvmContext, "", function);
		irBuilder.CreateCondBr(cmpres, trapBlock, normalBlock);
		// irBuilder.CreateBr(normalBlock);
		irBuilder.SetInsertPoint(normalBlock);
	}
	else
	{
		trapBlock
			= ::llvm::BasicBlock::Create(functionContext.moduleContext.llvmContext, "", function);
		::llvm::BasicBlock* normalBlock
			= ::llvm::BasicBlock::Create(functionContext.moduleContext.llvmContext, "", function);
		irBuilder.CreateCondBr(cmpres, trapBlock, normalBlock);
		// irBuilder.CreateBr(trapBlock);
		irBuilder.SetInsertPoint(trapBlock);
		irBuilder.CreateCall(getWavmMemtagTrapFunction(functionContext));
		irBuilder.CreateUnreachable();
		// irBuilder.CreateBr(normalBlock);
		irBuilder.SetInsertPoint(normalBlock);
		functionContext.MtgTrapBlock = trapBlock;
	}
}

// Bounds checks a sandboxed memory address + offset, and returns an offset relative to the memory
// base address that is guaranteed to be within the virtual address space allocated for the linear
// memory object.
static llvm::Value* getOffsetAndBoundedAddress(EmitFunctionContext& functionContext,
											   Uptr memoryIndex,
											   llvm::Value* address,
											   uint32_t knownNumBytes,
											   U64 offset,
											   BoundsCheckOp boundsCheckOp,
											   llvm::Value* numBytes = nullptr,
											   bool istagging = false)
{
	if(numBytes == nullptr)
	{
		numBytes = llvm::ConstantInt::get(address->getType(), knownNumBytes);
	}
	else { knownNumBytes = 0; }
	const MemoryType& memoryType
		= functionContext.moduleContext.irModule.memories.getType(memoryIndex);
	const bool is32bitMemoryOn64bitHost
		= memoryType.indexType == IndexType::i32
		  && functionContext.moduleContext.iptrValueType == ValueType::i64;

	llvm::IRBuilder<>& irBuilder = functionContext.irBuilder;
	auto& meminfo{functionContext.memoryInfos[memoryIndex]};

	bool fullcheck{functionContext.isMemTagged == ::WAVM::LLVMJIT::memtagStatus::full};

	auto memtagBasePointerVariable = meminfo.memtagBasePointerVariable;
	::llvm::Value* taggedval{};
	if(memtagBasePointerVariable) // memtag needs to ignore upper 8 bits
	{
		uint_least64_t shifter, mask;
		if(memoryType.indexType == IndexType::i64)
		{
			using constanttype = ::WAVM::IR::memtag64constants;
			shifter = constanttype::shifter;
			mask = constanttype::mask;
		}
		else
		{
			using constanttype = ::WAVM::IR::memtag32constants;
			shifter = constanttype::shifter;
			mask = constanttype::mask;
		}
		if(fullcheck && 1 < knownNumBytes && knownNumBytes <= 16u)
		{
			taggedval = irBuilder.CreateTrunc(irBuilder.CreateLShr(address, shifter),
											  functionContext.llvmContext.i16Type);
			taggedval = irBuilder.CreateAdd(irBuilder.CreateShl(taggedval, 8), taggedval);
		}
		else
		{
			taggedval = irBuilder.CreateTrunc(irBuilder.CreateLShr(address, shifter),
											  functionContext.llvmContext.i8Type);
		}
		address = irBuilder.CreateAnd(address, mask);
	}
	numBytes = irBuilder.CreateZExt(numBytes, address->getType());
	WAVM_ASSERT(numBytes->getType() == address->getType());

	if(memoryType.indexType == IndexType::i32)
	{
		// zext a 32-bit address and number of bytes to the target machine pointer width.
		// This is crucial for security, as LLVM will otherwise implicitly sign extend it to the
		// target machine pointer width in the GEP below, interpreting it as a signed offset and
		// allowing access to memory outside the sandboxed memory range.
		address = irBuilder.CreateZExt(address, functionContext.moduleContext.iptrType);
		numBytes = irBuilder.CreateZExt(numBytes, functionContext.moduleContext.iptrType);
	}
	// If the offset is greater than the size of the guard region, add it before bounds checking,
	// and check for overflow.
	if(offset && offset >= Runtime::memoryNumGuardBytes)
	{
		llvm::Constant* offsetConstant
			= emitLiteralIptr(offset, functionContext.moduleContext.iptrType);

		if(is32bitMemoryOn64bitHost || memtagBasePointerVariable)
		{
			// This is a 64-bit add of two numbers zero-extended from 32-bit, so it can't overflow.
			// Memtag will reserve upper 8 bits so it won't overflow either
			address = irBuilder.CreateAdd(address, offsetConstant);
		}
		else
		{
			llvm::Value* addressPlusOffsetAndOverflow
				= functionContext.callLLVMIntrinsic({functionContext.moduleContext.iptrType},
													llvm::Intrinsic::uadd_with_overflow,
													{address, offsetConstant});

			llvm::Value* addressPlusOffset
				= irBuilder.CreateExtractValue(addressPlusOffsetAndOverflow, {0});
			llvm::Value* addressPlusOffsetOverflowed
				= irBuilder.CreateExtractValue(addressPlusOffsetAndOverflow, {1});

			address
				= irBuilder.CreateOr(addressPlusOffset,
									 irBuilder.CreateSExt(addressPlusOffsetOverflowed,
														  functionContext.moduleContext.iptrType));
		}
	}
	bool memtagneedmaskoffset{true};
	if(boundsCheckOp == BoundsCheckOp::trapOnOutOfBounds)
	{
		// If the caller requires a trap, test whether the addressed bytes are within the bounds of
		// the memory, and if not call a trap intrinsic.
		llvm::Value* memoryNumBytes = getMemoryNumBytes(functionContext, memoryIndex);
		llvm::Value* memoryNumBytesMinusNumBytes = irBuilder.CreateSub(memoryNumBytes, numBytes);
		llvm::Value* numBytesWasGreaterThanMemoryNumBytes
			= irBuilder.CreateICmpUGT(memoryNumBytesMinusNumBytes, memoryNumBytes);
		auto cmpres{
			irBuilder.CreateOr(numBytesWasGreaterThanMemoryNumBytes,
							   irBuilder.CreateICmpUGT(address, memoryNumBytesMinusNumBytes))};
		createconditionaltrap(functionContext, cmpres);
	}
	else if(is32bitMemoryOn64bitHost)
	{
		// For 32-bit addresses on 64-bit targets, the runtime will reserve the full range of
		// addresses that can be generated by this function, so accessing those addresses has
		// well-defined behavior.
		memtagneedmaskoffset = false;
	}
	else
	{
		// For all other cases (e.g. 64-bit addresses on 64-bit targets), it's not possible for the
		// runtime to reserve the full range of addresses, so this function must clamp addresses to
		// the guard region.
		if(!memtagBasePointerVariable)
		{
			llvm::Value* endAddress = ::WAVM::LLVMJIT::wavmCreateLoad(
				irBuilder,
				functionContext.moduleContext.iptrType,
				functionContext.memoryInfos[memoryIndex].endAddressVariable);

#if 0
			createconditionaltrap(functionContext,
									irBuilder.CreateICmpUGE(address, endAddress));
#else
			address = irBuilder.CreateSelect(
				irBuilder.CreateICmpULT(address, endAddress), address, endAddress);
#endif
		}
	}
	if(offset && offset < Runtime::memoryNumGuardBytes)
	{
		llvm::Constant* offsetConstant
			= emitLiteralIptr(offset, functionContext.moduleContext.iptrType);

		address = irBuilder.CreateAdd(address, offsetConstant);
	}
	if(memtagBasePointerVariable && memtagneedmaskoffset)
	{
		// Memtagging incorporates its own checking logic, hence it is advisable to confidently
		// disregard the bounds checking logic.
		address = irBuilder.CreateAnd(address, ::IR::maxMemory64WASMMask);
	}
	// If the offset is less than the size of the guard region, then add it after bounds checking.
	// This avoids the need to check the addition for overflow, and allows it to be used as the
	// displacement in x86 addresses. Additionally, it allows the LLVM optimizer to reuse the bounds
	// checking code for consecutive loads/stores to the same address.
	if(!istagging && memtagBasePointerVariable)
	{
		if((fullcheck || knownNumBytes == 0) && knownNumBytes != 1)
		{
			if(1 < knownNumBytes && knownNumBytes <= 16u)
			{
				::llvm::Value* addressrshift{irBuilder.CreateLShr(address, 4)};
				::llvm::Value* tagbaseptrval = ::WAVM::LLVMJIT::wavmCreateLoad(
					irBuilder, functionContext.llvmContext.i8PtrType, memtagBasePointerVariable);
				::llvm::Value* tagbytePointer = ::WAVM::LLVMJIT::wavmCreateInBoundsGEP(
					irBuilder, functionContext.llvmContext.i8Type, tagbaseptrval, addressrshift);
				::llvm::Value* taginmem = ::WAVM::LLVMJIT::wavmCreateLoad(
					irBuilder, functionContext.llvmContext.i16Type, tagbytePointer);
				::llvm::Value* addresslast = irBuilder.CreateAdd(
					address, llvm::ConstantInt::get(address->getType(), knownNumBytes - 1u));
				::llvm::Value* addresslastrshift{irBuilder.CreateLShr(addresslast, 4)};
				// need littleendian
				auto addresssamebucket = irBuilder.CreateICmpEQ(addressrshift, addresslastrshift);
#if 0
				auto c = irBuilder.CreateLShr(
					llvm::ConstantInt::get(functionContext.llvmContext.i16Type, 0xFFFF),
					irBuilder.CreateShl(
						irBuilder.CreateZExt(addresssamebucket, functionContext.llvmContext.i8Type),
						3));
#else
				auto c = irBuilder.CreateSelect(
					addresssamebucket,
					llvm::ConstantInt::get(functionContext.llvmContext.i16Type, 0xFF),
					llvm::ConstantInt::get(functionContext.llvmContext.i16Type, 0xFFFF));
#endif
				taggedval = irBuilder.CreateAnd(taggedval, c);
				taginmem = irBuilder.CreateAnd(taginmem, c);
				createmtgconditionaltrap(functionContext,
										 irBuilder.CreateICmpNE(taggedval, taginmem));
			}
			else
			{
				::llvm::BasicBlock* checkBlock = ::llvm::BasicBlock::Create(
					functionContext.moduleContext.llvmContext, "", functionContext.function);
				::llvm::BasicBlock* mergeblock = ::llvm::BasicBlock::Create(
					functionContext.moduleContext.llvmContext, "", functionContext.function);
				irBuilder.CreateCondBr(
					irBuilder.CreateICmpNE(numBytes,
										   llvm::ConstantInt::get(numBytes->getType(), 0)),
					checkBlock,
					mergeblock);
				irBuilder.SetInsertPoint(checkBlock);
				::llvm::Value* addressrshift{irBuilder.CreateLShr(address, 4)};
				::llvm::Value* tagbaseptrval = ::WAVM::LLVMJIT::wavmCreateLoad(
					irBuilder, functionContext.llvmContext.i8PtrType, memtagBasePointerVariable);
				::llvm::Value* tagbytePointer = ::WAVM::LLVMJIT::wavmCreateInBoundsGEP(
					irBuilder, functionContext.llvmContext.i8Type, tagbaseptrval, addressrshift);
				auto firstcmpres = irBuilder.CreateICmpNE(
					taggedval,
					::WAVM::LLVMJIT::wavmCreateLoad(
						irBuilder, functionContext.llvmContext.i8Type, tagbytePointer));

				::llvm::Value* addresslastrshift{irBuilder.CreateLShr(
					irBuilder.CreateAdd(
						address,
						irBuilder.CreateSub(numBytes,
											llvm::ConstantInt::get(numBytes->getType(), 1))),
					4)};
				::llvm::Value* lasttagbytePointer
					= ::WAVM::LLVMJIT::wavmCreateInBoundsGEP(irBuilder,
															 functionContext.llvmContext.i8Type,
															 tagbaseptrval,
															 {addresslastrshift});
				auto lastcmpres = irBuilder.CreateICmpNE(
					taggedval,
					::WAVM::LLVMJIT::wavmCreateLoad(
						irBuilder, functionContext.llvmContext.i8Type, lasttagbytePointer));
				createmtgconditionaltrap(functionContext,
										 irBuilder.CreateLogicalOr(firstcmpres, lastcmpres));
				irBuilder.CreateBr(mergeblock);
				irBuilder.SetInsertPoint(mergeblock);
			}
		}
		else
		{
			::llvm::Value* addressrshift{irBuilder.CreateLShr(address, 4)};
			::llvm::Value* tagbaseptrval = ::WAVM::LLVMJIT::wavmCreateLoad(
				irBuilder, functionContext.llvmContext.i8PtrType, memtagBasePointerVariable);
			::llvm::Value* tagbytePointer = ::WAVM::LLVMJIT::wavmCreateInBoundsGEP(
				irBuilder, functionContext.llvmContext.i8Type, tagbaseptrval, addressrshift);
			createmtgconditionaltrap(
				functionContext,
				irBuilder.CreateICmpNE(
					taggedval,
					::WAVM::LLVMJIT::wavmCreateLoad(
						irBuilder, functionContext.llvmContext.i8Type, tagbytePointer)));
		}
	}
	return address;
}

llvm::Value* EmitFunctionContext::coerceAddressToPointer(llvm::Value* boundedAddress,
														 llvm::Type* memoryType,
														 Uptr memoryIndex)
{
	auto& meminfo{memoryInfos[memoryIndex]};
	llvm::Value* memoryBasePointer = ::WAVM::LLVMJIT::wavmCreateLoad(
		irBuilder, llvmContext.i8PtrType, meminfo.basePointerVariable);
	llvm::Value* bytePointer = ::WAVM::LLVMJIT::wavmCreateInBoundsGEP(
		irBuilder, llvmContext.i8Type, memoryBasePointer, boundedAddress);

	// Cast the pointer to the appropriate type.
#if LLVM_VERSION_MAJOR > 14
	return bytePointer;
#else
	return irBuilder.CreatePointerCast(bytePointer, memoryType->getPointerTo());
#endif
}

//
// Memory size operators
// These just call out to wavmIntrinsics.growMemory/currentMemory, passing a pointer to the default
// memory for the module.
//

void EmitFunctionContext::memory_grow(MemoryImm imm)
{
	llvm::Value* deltaNumPages = pop();
	ValueVector resultTuple = emitRuntimeIntrinsic(
		"memory.grow",
		FunctionType(TypeTuple(moduleContext.iptrValueType),
					 TypeTuple({moduleContext.iptrValueType, moduleContext.iptrValueType}),
					 IR::CallingConvention::intrinsic),
		{zext(deltaNumPages, moduleContext.iptrType),
		 getMemoryIdFromOffset(irBuilder, moduleContext.memoryOffsets[imm.memoryIndex])});
	WAVM_ASSERT(resultTuple.size() == 1);
	const MemoryType& memoryType = moduleContext.irModule.memories.getType(imm.memoryIndex);
	push(coerceIptrToIndex(memoryType.indexType, resultTuple[0]));
}
void EmitFunctionContext::memory_size(MemoryImm imm)
{
	const MemoryType& memoryType = moduleContext.irModule.memories.getType(imm.memoryIndex);
	push(coerceIptrToIndex(memoryType.indexType, getMemoryNumPages(*this, imm.memoryIndex)));
}

//
// Memory bulk operators.
//

void EmitFunctionContext::memory_init(DataSegmentAndMemImm imm)
{
	auto numBytes = pop();
	auto sourceOffset = pop();
	auto destAddress = pop();
	emitRuntimeIntrinsic(
		"memory.init",
		FunctionType({},
					 TypeTuple({moduleContext.iptrValueType,
								moduleContext.iptrValueType,
								moduleContext.iptrValueType,
								moduleContext.iptrValueType,
								moduleContext.iptrValueType,
								moduleContext.iptrValueType}),
					 IR::CallingConvention::intrinsic),
		{zext(destAddress, moduleContext.iptrType),
		 zext(sourceOffset, moduleContext.iptrType),
		 zext(numBytes, moduleContext.iptrType),
		 moduleContext.instanceId,
		 getMemoryIdFromOffset(irBuilder, moduleContext.memoryOffsets[imm.memoryIndex]),
		 emitLiteral(llvmContext, imm.dataSegmentIndex)});
}

void EmitFunctionContext::data_drop(DataSegmentImm imm)
{
	emitRuntimeIntrinsic(
		"data.drop",
		FunctionType({},
					 TypeTuple({moduleContext.iptrValueType, moduleContext.iptrValueType}),
					 IR::CallingConvention::intrinsic),
		{moduleContext.instanceId, emitLiteralIptr(imm.dataSegmentIndex, moduleContext.iptrType)});
}

void EmitFunctionContext::memory_copy(MemoryCopyImm imm)
{
	llvm::Value* numBytes = pop();
	llvm::Value* sourceAddress = pop();
	llvm::Value* destAddress = pop();

	llvm::Value* sourceBoundedAddress = getOffsetAndBoundedAddress(*this,
																   imm.sourceMemoryIndex,
																   sourceAddress,
																   0,
																   0,
																   BoundsCheckOp::trapOnOutOfBounds,
																   numBytes);
	llvm::Value* destBoundedAddress = getOffsetAndBoundedAddress(
		*this, imm.destMemoryIndex, destAddress, 0, 0, BoundsCheckOp::trapOnOutOfBounds, numBytes);

	llvm::Value* sourcePointer
		= coerceAddressToPointer(sourceBoundedAddress, llvmContext.i8Type, imm.sourceMemoryIndex);
	llvm::Value* destPointer
		= coerceAddressToPointer(destBoundedAddress, llvmContext.i8Type, imm.destMemoryIndex);

	llvm::Value* numBytesUptr = irBuilder.CreateZExt(numBytes, moduleContext.iptrType);

	// Use the LLVM memmove instruction to do the copy.
#if LLVM_VERSION_MAJOR < 7
	irBuilder.CreateMemMove(destPointer, sourcePointer, numBytesUptr, 1, true);
#else
	irBuilder.CreateMemMove(
		destPointer, LLVM_ALIGNMENT(1), sourcePointer, LLVM_ALIGNMENT(1), numBytesUptr, true);
#endif
}

void EmitFunctionContext::memory_fill(MemoryImm imm)
{
	llvm::Value* numBytes = pop();
	llvm::Value* value = pop();
	llvm::Value* destAddress = pop();

	llvm::Value* destBoundedAddress = getOffsetAndBoundedAddress(
		*this, imm.memoryIndex, destAddress, 0, 0, BoundsCheckOp::trapOnOutOfBounds, numBytes);
	llvm::Value* destPointer
		= coerceAddressToPointer(destBoundedAddress, llvmContext.i8Type, imm.memoryIndex);

	llvm::Value* numBytesUptr = irBuilder.CreateZExt(numBytes, moduleContext.iptrType);

	// Use the LLVM memset instruction to do the fill.
	irBuilder.CreateMemSet(destPointer,
						   irBuilder.CreateTrunc(value, llvmContext.i8Type),
						   numBytesUptr,
						   LLVM_ALIGNMENT(1),
						   true);
}

static inline bool isMemTaggedEnabled(EmitFunctionContext& functionContext)
{
	return functionContext.isMemTagged != ::WAVM::LLVMJIT::memtagStatus::none;
}

static inline ::llvm::Value* generateMemRandomTagByte(EmitFunctionContext& functionContext,
													  Uptr memoryIndex)
{
	auto& irBuilder = functionContext.irBuilder;
	auto& meminfo = functionContext.memoryInfos[memoryIndex];
	::llvm::Value* memtagrandombufferptr = ::WAVM::LLVMJIT::wavmCreateLoad(
		irBuilder, functionContext.llvmContext.i8PtrType, meminfo.memtagRandomBufferVariable);
	auto* function = functionContext.function;
	::llvm::BasicBlock* rdtagentryBlock
		= ::llvm::BasicBlock::Create(functionContext.moduleContext.llvmContext, "", function);
	irBuilder.CreateBr(rdtagentryBlock);
	irBuilder.SetInsertPoint(rdtagentryBlock);
	::llvm::Value* arg0 = memtagrandombufferptr;

	::llvm::Value* currptraddr
		= irBuilder.CreateGEP(functionContext.llvmContext.i8PtrType, arg0, {irBuilder.getInt32(1)});
	::llvm::Value* currptr = ::WAVM::LLVMJIT::wavmCreateLoad(
		irBuilder, functionContext.llvmContext.i8PtrType, currptraddr);
	::llvm::Value* endptraddr
		= irBuilder.CreateGEP(functionContext.llvmContext.i8PtrType, arg0, {irBuilder.getInt32(2)});
	::llvm::Value* endptr = ::WAVM::LLVMJIT::wavmCreateLoad(
		irBuilder, functionContext.llvmContext.i8PtrType, endptraddr);
	::llvm::Value* cmpres = irBuilder.CreateICmpEQ(currptr, endptr);
	::llvm::BasicBlock* trueBlock
		= ::llvm::BasicBlock::Create(functionContext.moduleContext.llvmContext, "", function);
	::llvm::BasicBlock* mergeBlock
		= ::llvm::BasicBlock::Create(functionContext.moduleContext.llvmContext, "", function);
	irBuilder.CreateCondBr(cmpres, trueBlock, mergeBlock);
	irBuilder.SetInsertPoint(trueBlock);
	::llvm::Value* begptr
		= ::WAVM::LLVMJIT::wavmCreateLoad(irBuilder, functionContext.llvmContext.i8PtrType, arg0);
	irBuilder.CreateBr(mergeBlock);
	irBuilder.SetInsertPoint(mergeBlock);
	auto currphiNode = irBuilder.CreatePHI(functionContext.llvmContext.i8PtrType, 2);
	currphiNode->addIncoming(begptr, trueBlock);
	currphiNode->addIncoming(currptr, rdtagentryBlock);
	::llvm::Value* rettag = ::WAVM::LLVMJIT::wavmCreateLoad(
		irBuilder, functionContext.llvmContext.i8Type, currphiNode);
	currptr = irBuilder.CreateGEP(
		functionContext.llvmContext.i8Type, currphiNode, {irBuilder.getInt32(1)});
	irBuilder.CreateStore(currptr, currptraddr);
	return rettag;
}

static inline ::llvm::Value* TagMemPointer(EmitFunctionContext& functionContext,
										   Uptr memoryIndex,
										   ::llvm::Value* address,
										   ::llvm::Value* color,
										   bool addressuntagged)
{
	const MemoryType& memoryType
		= functionContext.moduleContext.irModule.memories.getType(memoryIndex);

	llvm::IRBuilder<>& irBuilder = functionContext.irBuilder;

	uint_least64_t shifter, mask;
	::llvm::Type* extendtype;
	if(memoryType.indexType == IndexType::i64)
	{
		using constanttype = ::WAVM::IR::memtag64constants;
		shifter = constanttype::shifter;
		mask = constanttype::mask;
		extendtype = functionContext.llvmContext.i64Type;
	}
	else
	{
		using constanttype = ::WAVM::IR::memtag32constants;
		shifter = constanttype::shifter;
		mask = constanttype::mask;
		extendtype = functionContext.llvmContext.i32Type;
	}
	color = irBuilder.CreateZExt(color, extendtype);
	color = irBuilder.CreateShl(color, shifter);
	if(!addressuntagged) { address = irBuilder.CreateAnd(address, mask); }
	address = irBuilder.CreateOr(address, color);
	return address;
}

static inline ::llvm::Value* UntagAddress(EmitFunctionContext& functionContext,
										  Uptr memoryIndex,
										  ::llvm::Value* address)
{
	MemoryType const& memoryType
		= functionContext.moduleContext.irModule.memories.getType(memoryIndex);
	llvm::IRBuilder<>& irBuilder = functionContext.irBuilder;
	uint_least64_t mask;
	if(memoryType.indexType == IndexType::i64) { mask = ::WAVM::IR::memtag64constants::mask; }
	else { mask = ::WAVM::IR::memtag32constants::mask; }
	address = irBuilder.CreateAnd(address, mask);
	return address;
}

static inline ::llvm::Value* ComputeMemTagIndex(EmitFunctionContext& functionContext,
												Uptr memoryIndex,
												::llvm::Value* address,
												bool addressuntagged)
{
	if(!addressuntagged) { address = UntagAddress(functionContext, memoryIndex, address); }
	llvm::IRBuilder<>& irBuilder = functionContext.irBuilder;
	auto* memtagBasePointerVariable
		= functionContext.memoryInfos[memoryIndex].memtagBasePointerVariable;
	auto* memtagbase = ::WAVM::LLVMJIT::wavmCreateLoad(
		irBuilder, functionContext.llvmContext.i8PtrType, memtagBasePointerVariable);
	address = irBuilder.CreateAnd(address, ::IR::maxMemory64WASMMask);
	auto* realtagaddress = irBuilder.CreateGEP(
		functionContext.llvmContext.i8Type, memtagbase, {irBuilder.CreateLShr(address, 4)});
	return realtagaddress;
}

static void memtag_zero_memory(EmitFunctionContext& functionContext,
							   Uptr memoryIndex,
							   ::llvm::Value* untaggedmemaddress,
							   ::llvm::Value* taggedbytes)
{
	auto realaddr = functionContext.coerceAddressToPointer(
		untaggedmemaddress, functionContext.llvmContext.i8Type, memoryIndex);
	llvm::IRBuilder<>& irBuilder = functionContext.irBuilder;
	::llvm::Value* zeroconstant = irBuilder.getInt8(0);
	irBuilder.CreateMemSet(realaddr, zeroconstant, taggedbytes, LLVM_ALIGNMENT(1), false);
}

static inline ::llvm::Value* StoreTagIntoMemAndZeroing(EmitFunctionContext& functionContext,
													   Uptr memoryIndex,
													   ::llvm::Value* address,
													   ::llvm::Value* taggedbytes,
													   ::llvm::Value* color,
													   bool zeroing,
													   bool addressisuntagged)
{
	MemoryType const& memoryType
		= functionContext.moduleContext.irModule.memories.getType(memoryIndex);
	llvm::IRBuilder<>& irBuilder = functionContext.irBuilder;
	uint_least64_t shifter, mask;
	if(memoryType.indexType == IndexType::i64)
	{
		using constanttype = ::WAVM::IR::memtag64constants;
		shifter = constanttype::shifter;
		mask = constanttype::mask;
	}
	else
	{
		using constanttype = ::WAVM::IR::memtag32constants;
		shifter = constanttype::shifter;
		mask = constanttype::mask;
	}
	if(color == nullptr)
	{
		color = irBuilder.CreateTrunc(irBuilder.CreateLShr(address, shifter),
									  functionContext.llvmContext.i8Type);
	}
	if(!addressisuntagged) { address = irBuilder.CreateAnd(address, mask); }

	if(functionContext.isMemTagged == ::WAVM::LLVMJIT::memtagStatus::armmte)
	{
		if(functionContext.moduleContext.targetArch == ::llvm::Triple::aarch64)
		{
			auto decl = ::llvm::Intrinsic::aarch64_stgp;
			if(zeroing) { decl = ::llvm::Intrinsic::aarch64_settag_zero; }
			auto SetTagFn = ::llvm::Intrinsic::getOrInsertDeclaration(
				functionContext.moduleContext.llvmModule,
				decl,
				{functionContext.llvmContext.i64Type});
			// todo bounds checking
			llvm::Value* sourcePointer = functionContext.coerceAddressToPointer(
				getOffsetAndBoundedAddress(functionContext,
										   memoryIndex,
										   address,
										   0,
										   0,
										   BoundsCheckOp::clampToGuardRegion,
										   nullptr,
										   true),
				functionContext.llvmContext.i8Type,
				memoryIndex);
			irBuilder.CreateCall(SetTagFn, {sourcePointer, taggedbytes});
		}
		else if(zeroing) { memtag_zero_memory(functionContext, memoryIndex, address, taggedbytes); }
	}
	else
	{
		auto* memtagBasePointerVariable
			= functionContext.memoryInfos[memoryIndex].memtagBasePointerVariable;
		auto* memtagbase = ::WAVM::LLVMJIT::wavmCreateLoad(
			irBuilder, functionContext.llvmContext.i8PtrType, memtagBasePointerVariable);
		auto* realtagaddress = irBuilder.CreateGEP(
			functionContext.llvmContext.i8Type, memtagbase, {irBuilder.CreateLShr(address, 4)});
		if(memoryType.indexType == IndexType::i64)
		{
			auto* cndpageprotect = irBuilder.CreateICmpUGE(
				taggedbytes,
				::llvm::ConstantInt::get(functionContext.llvmContext.i64Type,
										 ::WAVM::Runtime::memoryNumGuardBytes));
			llvm::Value* addressPlusOffsetAndOverflow
				= functionContext.callLLVMIntrinsic({functionContext.moduleContext.iptrType},
													llvm::Intrinsic::uadd_with_overflow,
													{address, taggedbytes});
			llvm::Value* addressPlusOffset
				= irBuilder.CreateExtractValue(addressPlusOffsetAndOverflow, {0});
			llvm::Value* addressPlusOffsetOverflowed
				= irBuilder.CreateExtractValue(addressPlusOffsetAndOverflow, {1});
			auto* cndbig = irBuilder.CreateICmpUGE(
				addressPlusOffset,
				::llvm::ConstantInt::get(functionContext.llvmContext.i64Type,
										 ::WAVM::IR::maxMemory64WASMBytes));
			createconditionaltrap(
				functionContext,
				irBuilder.CreateAnd(cndpageprotect,
									irBuilder.CreateAnd(cndbig, addressPlusOffsetOverflowed)));
		}
		irBuilder.CreateMemSet(
			realtagaddress, color, irBuilder.CreateLShr(taggedbytes, 4), LLVM_ALIGNMENT(1), false);
		if(zeroing) { memtag_zero_memory(functionContext, memoryIndex, address, taggedbytes); }
	}
	return address;
}

static inline ::llvm::Value* StoreTagIntoMem(EmitFunctionContext& functionContext,
											 Uptr memoryIndex,
											 ::llvm::Value* address,
											 ::llvm::Value* taggedbytes,
											 ::llvm::Value* color,
											 bool addressisuntagged = false)
{
	return StoreTagIntoMemAndZeroing(
		functionContext, memoryIndex, address, taggedbytes, color, false, addressisuntagged);
}

static inline ::llvm::Value* StoreZTagIntoMem(EmitFunctionContext& functionContext,
											  Uptr memoryIndex,
											  ::llvm::Value* address,
											  ::llvm::Value* taggedbytes,
											  ::llvm::Value* color,
											  bool addressisuntagged = false)
{
	return StoreTagIntoMemAndZeroing(
		functionContext, memoryIndex, address, taggedbytes, color, true, addressisuntagged);
}

void EmitFunctionContext::memtag_status(MemoryImm imm)
{
	MemoryType const& memoryType = this->moduleContext.irModule.memories.getType(imm.memoryIndex);
	if(memoryType.indexType == IndexType::i64)
	{
		push(this->irBuilder.getInt64(static_cast<::std::uint64_t>(this->isMemTagged)));
	}
	else { push(this->irBuilder.getInt32(static_cast<::std::uint64_t>(this->isMemTagged))); }
}

void EmitFunctionContext::memtag_tagbits(MemoryImm imm)
{
	MemoryType const& memoryType = this->moduleContext.irModule.memories.getType(imm.memoryIndex);
	if(isMemTaggedEnabled(*this))
	{
		if(memoryType.indexType == IndexType::i64)
		{
			if(this->isMemTagged == ::WAVM::LLVMJIT::memtagStatus::armmte)
			{
				push(this->irBuilder.getInt64(4));
			}
			else { push(this->irBuilder.getInt64(::WAVM::IR::memtag64constants::bits)); }
		}
		else
		{
			if constexpr(::WAVM::IR::memtag32constants::bits != 4)
			{
				if(this->isMemTagged == ::WAVM::LLVMJIT::memtagStatus::armmte)
				{
					push(this->irBuilder.getInt32(4));
					return;
				}
			}
			push(this->irBuilder.getInt32(::WAVM::IR::memtag32constants::bits));
		}
	}
	else
	{
		if(memoryType.indexType == IndexType::i64) { push(this->irBuilder.getInt64(0)); }
		else { push(this->irBuilder.getInt32(0)); }
	}
}

void EmitFunctionContext::memtag_startbit(MemoryImm imm)
{
	MemoryType const& memoryType = this->moduleContext.irModule.memories.getType(imm.memoryIndex);
	if(isMemTaggedEnabled(*this))
	{
		if(memoryType.indexType == IndexType::i64)
		{
			if(this->isMemTagged == ::WAVM::LLVMJIT::memtagStatus::armmte)
			{
				push(this->irBuilder.getInt64(56));
			}
			else { push(this->irBuilder.getInt64(::WAVM::IR::memtag32constants::shifter)); }
		}
		else
		{
			if(this->isMemTagged == ::WAVM::LLVMJIT::memtagStatus::armmte)
			{
				push(this->irBuilder.getInt64(28));
			}
			else { push(this->irBuilder.getInt32(::WAVM::IR::memtag32constants::shifter)); }
		}
	}
	else
	{
		if(memoryType.indexType == IndexType::i64) { push(this->irBuilder.getInt64(64)); }
		else { push(this->irBuilder.getInt32(32)); }
	}
}

static ::llvm::Value* memtag_store_tag_common(EmitFunctionContext& functionContext,
											  Uptr memoryIndex,
											  ::llvm::Value* memaddress,
											  ::llvm::Value* mask,
											  ::llvm::Value* taggedbytes,
											  bool zeroing)
{
	if(isMemTaggedEnabled(functionContext))
	{
		MemoryType const& memoryType
			= functionContext.moduleContext.irModule.memories.getType(memoryIndex);
		llvm::IRBuilder<>& irBuilder = functionContext.irBuilder;
		if(memoryType.indexType == IndexType::i64)
		{
			if(functionContext.isMemTagged == ::WAVM::LLVMJIT::memtagStatus::armmte)
			{
				auto olduntaggedmemaddress{UntagAddress(functionContext, memoryIndex, memaddress)};
				memaddress = functionContext.coerceAddressToPointer(
					getOffsetAndBoundedAddress(functionContext,
											   memoryIndex,
											   memaddress,
											   0,
											   0,
											   BoundsCheckOp::clampToGuardRegion,
											   taggedbytes,
											   true),
					functionContext.llvmContext.i8Type,
					memoryIndex);
				memaddress
					= irBuilder.CreateIntrinsic(zeroing ? (::llvm::Intrinsic::aarch64_settag_zero)
														: (::llvm::Intrinsic::aarch64_settag),
												{},
												{memaddress, taggedbytes});
				memaddress = armmte64_to_32_old_value(
					functionContext, memoryIndex, olduntaggedmemaddress, memaddress);
			}
		}
		else
		{
			auto color = generateMemRandomTagByte(functionContext, memoryIndex);
			if(zeroing)
			{
				memaddress = StoreZTagIntoMem(
					functionContext, memoryIndex, memaddress, taggedbytes, color);
			}
			else
			{
				memaddress
					= StoreTagIntoMem(functionContext, memoryIndex, memaddress, taggedbytes, color);
			}
			memaddress = TagMemPointer(functionContext, memoryIndex, memaddress, color, true);
		}
	}
	return memaddress;
}

void EmitFunctionContext::memtag_randomstore(MemoryImm imm)
{
	::llvm::Value* taggedbytes = pop();
	::llvm::Value* memaddress = pop();
	push(memtag_store_tag_common(*this, imm.memoryIndex, memaddress, nullptr, taggedbytes, false));
}

void EmitFunctionContext::memtag_randomstorez(MemoryImm imm)
{
	::llvm::Value* taggedbytes = pop();
	::llvm::Value* memaddress = pop();
	push(memtag_store_tag_common(*this, imm.memoryIndex, memaddress, nullptr, taggedbytes, true));
}

void EmitFunctionContext::memtag_randommaskstore(MemoryImm imm)
{
	::llvm::Value* mask = pop();
	::llvm::Value* taggedbytes = pop();
	::llvm::Value* memaddress = pop();
	push(memtag_store_tag_common(*this, imm.memoryIndex, memaddress, mask, taggedbytes, false));
}

void EmitFunctionContext::memtag_randommaskstorez(MemoryImm imm)
{
	::llvm::Value* mask = pop();
	::llvm::Value* taggedbytes = pop();
	::llvm::Value* memaddress = pop();
	push(memtag_store_tag_common(*this, imm.memoryIndex, memaddress, mask, taggedbytes, true));
}

void EmitFunctionContext::memtag_extract(MemoryImm imm)
{
	::llvm::Value* memaddress = pop();
	if(isMemTaggedEnabled(*this))
	{
		MemoryType const& memoryType
			= this->moduleContext.irModule.memories.getType(imm.memoryIndex);
		llvm::IRBuilder<>& irBuilder = this->irBuilder;
		uint_least64_t shifter;
		if(memoryType.indexType == IndexType::i64)
		{
			using constanttype = ::WAVM::IR::memtag64constants;
			shifter = constanttype::shifter;
		}
		else
		{
			using constanttype = ::WAVM::IR::memtag32constants;
			shifter = constanttype::shifter;
		}
		push(irBuilder.CreateLShr(memaddress, shifter));
	}
	else
	{
		MemoryType const& memoryType
			= this->moduleContext.irModule.memories.getType(imm.memoryIndex);
		if(memoryType.indexType == IndexType::i64) { push(this->irBuilder.getInt64(0)); }
		else { push(this->irBuilder.getInt32(0)); }
	}
}

void EmitFunctionContext::memtag_insert(MemoryImm imm)
{
	::llvm::Value* newcolor = pop();
	::llvm::Value* memaddress = pop();
	if(isMemTaggedEnabled(*this))
	{
		MemoryType const& memoryType
			= this->moduleContext.irModule.memories.getType(imm.memoryIndex);
		llvm::IRBuilder<>& irBuilder = this->irBuilder;
		uint_least64_t shifter, mask, tagindexmask;
		::llvm::Type* vtype;
		if(memoryType.indexType == IndexType::i64)
		{
			using constanttype = ::WAVM::IR::memtag64constants;
			shifter = constanttype::shifter;
			mask = constanttype::mask;
			tagindexmask = constanttype::index_mask;
			vtype = irBuilder.getInt64Ty();
		}
		else
		{
			using constanttype = ::WAVM::IR::memtag32constants;
			shifter = constanttype::shifter;
			mask = constanttype::mask;
			tagindexmask = constanttype::index_mask;
			vtype = irBuilder.getInt32Ty();
		}
		memaddress = irBuilder.CreateOr(
			irBuilder.CreateAnd(memaddress, ::llvm::ConstantInt::get(vtype, mask)),
			irBuilder.CreateShl(
				irBuilder.CreateAnd(::llvm::ConstantInt::get(vtype, tagindexmask), newcolor),
				shifter)); // Todo: Fix it
	}
	push(memaddress);
}

void EmitFunctionContext::memtag_untag(MemoryImm imm)
{
	::llvm::Value* memaddress = pop();
	if(isMemTaggedEnabled(*this)) { memaddress = UntagAddress(*this, imm.memoryIndex, memaddress); }
	push(memaddress);
}

void EmitFunctionContext::memtag_untagstore(MemoryImm imm)
{
	::llvm::Value* taggedbytes = pop();
	::llvm::Value* memaddress = pop();
	if(isMemTaggedEnabled(*this))
	{
		memaddress = UntagAddress(*this, imm.memoryIndex, memaddress);
		StoreTagIntoMem(*this,
						imm.memoryIndex,
						memaddress,
						taggedbytes,
						llvm::ConstantInt::get(this->llvmContext.i8Type, 0));
	}
	push(memaddress);
}

void EmitFunctionContext::memtag_untagstorez(MemoryImm imm)
{
	::llvm::Value* taggedbytes = pop();
	::llvm::Value* memaddress = pop();
	if(isMemTaggedEnabled(*this))
	{
		memaddress = UntagAddress(*this, imm.memoryIndex, memaddress);
		StoreZTagIntoMem(*this,
						 imm.memoryIndex,
						 memaddress,
						 taggedbytes,
						 llvm::ConstantInt::get(this->llvmContext.i8Type, 0));
	}
	push(memaddress);
}

void EmitFunctionContext::memtag_store(MemoryImm imm)
{
	::llvm::Value* taggedbytes = pop();
	::llvm::Value* memaddress = pop();
	if(isMemTaggedEnabled(*this))
	{
		StoreTagIntoMem(*this, imm.memoryIndex, memaddress, taggedbytes, nullptr);
	}
}

void EmitFunctionContext::memtag_storez(MemoryImm imm)
{
	::llvm::Value* taggedbytes = pop();
	::llvm::Value* memaddress = pop();
	if(isMemTaggedEnabled(*this))
	{
		StoreZTagIntoMem(*this, imm.memoryIndex, memaddress, taggedbytes, nullptr);
	}
	else { memtag_zero_memory(*this, imm.memoryIndex, memaddress, taggedbytes); }
}

void EmitFunctionContext::memtag_random(MemoryImm imm)
{
	::llvm::Value* memaddress = pop();
	if(isMemTaggedEnabled(*this))
	{
		if(this->isMemTagged == ::WAVM::LLVMJIT::memtagStatus::armmte)
		{
			memaddress = armmte32_to_64ptr_value(*this, imm.memoryIndex, memaddress);
			memaddress = irBuilder.CreateIntrinsic(
				::llvm::Intrinsic::aarch64_irg,
				{},
				{memaddress, ::llvm::ConstantInt::get(this->llvmContext.i64Type, 0)});
			memaddress = armmte64_to_32_value(*this, imm.memoryIndex, memaddress);
		}
		else
		{
			auto color = generateMemRandomTagByte(*this, imm.memoryIndex);
			memaddress = TagMemPointer(*this, imm.memoryIndex, memaddress, color, false);
		}
	}
	push(memaddress);
}

void EmitFunctionContext::memtag_randommask(MemoryImm imm) // Todo
{
	::llvm::Value* mask = pop();
	::llvm::Value* memaddress = pop();
	if(isMemTaggedEnabled(*this))
	{
		if(this->isMemTagged == ::WAVM::LLVMJIT::memtagStatus::armmte)
		{
			memaddress = armmte32_to_64ptr_value(*this, imm.memoryIndex, memaddress);
			memaddress
				= irBuilder.CreateIntrinsic(::llvm::Intrinsic::aarch64_irg, {}, {memaddress, mask});
			memaddress = armmte64_to_32_value(*this, imm.memoryIndex, memaddress);
		}
		else
		{
			auto color = generateMemRandomTagByte(*this, imm.memoryIndex);
			memaddress = TagMemPointer(*this, imm.memoryIndex, memaddress, color, false);
		}
	}
	push(memaddress);
}

struct hint_addr_result
{
	::llvm::Value* color;
	::llvm::Value* untaggedmemaddress;
	::llvm::Value* taggedmemaddress;
};

static hint_addr_result compute_hint_addr_seperate(EmitFunctionContext& functionContext,
												   Uptr memoryIndex,
												   ::llvm::Value* memaddress,
												   ::llvm::Value* hintptr,
												   ::llvm::Value* hintindex)
{
	MemoryType const& memoryType
		= functionContext.moduleContext.irModule.memories.getType(memoryIndex);
	llvm::IRBuilder<>& irBuilder = functionContext.irBuilder;
	uint_least32_t bits;
	uint_least64_t shifter, mask, tagindexmask;
	if(memoryType.indexType == IndexType::i64)
	{
		using constanttype = ::WAVM::IR::memtag64constants;
		bits = constanttype::bits;
		shifter = constanttype::shifter;
		mask = constanttype::mask;
		tagindexmask = constanttype::index_mask;
	}
	else
	{
		using constanttype = ::WAVM::IR::memtag32constants;
		bits = constanttype::bits;
		shifter = constanttype::shifter;
		mask = constanttype::mask;
		tagindexmask = constanttype::index_mask;
	}
	hintindex = irBuilder.CreateAnd(hintindex, tagindexmask);
	hintptr = irBuilder.CreateLShr(hintptr, shifter);
	hintptr = irBuilder.CreateAdd(hintptr, hintindex);
	memaddress = irBuilder.CreateAnd(memaddress, mask);
	if(bits != 8) { hintptr = irBuilder.CreateAnd(hintptr, tagindexmask); }
	return {irBuilder.CreateTrunc(hintptr, functionContext.llvmContext.i8Type),
			memaddress,
			irBuilder.CreateAdd(irBuilder.CreateShl(hintptr, shifter), memaddress)};
}

static ::llvm::Value* compute_hint_addr(EmitFunctionContext& functionContext,
										Uptr memoryIndex,
										::llvm::Value* memaddress,
										::llvm::Value* hintptr,
										::llvm::Value* hintindex)
{
	MemoryType const& memoryType
		= functionContext.moduleContext.irModule.memories.getType(memoryIndex);
	llvm::IRBuilder<>& irBuilder = functionContext.irBuilder;
	uint_least64_t shifter, mask, hintmask, tagindexmask;
	if(memoryType.indexType == IndexType::i64)
	{
		using constanttype = ::WAVM::IR::memtag64constants;
		shifter = constanttype::shifter;
		mask = constanttype::mask;
		hintmask = constanttype::hint_mask;
		tagindexmask = constanttype::index_mask;
	}
	else
	{
		using constanttype = ::WAVM::IR::memtag32constants;
		shifter = constanttype::shifter;
		mask = constanttype::mask;
		hintmask = constanttype::hint_mask;
		tagindexmask = constanttype::index_mask;
	}
	hintindex = irBuilder.CreateAnd(hintindex, tagindexmask);
	hintindex = irBuilder.CreateShl(hintindex, shifter);
	hintptr = irBuilder.CreateAnd(hintptr, hintmask);
	hintptr = irBuilder.CreateAdd(hintptr, hintindex);
	memaddress = irBuilder.CreateAnd(memaddress, mask);
	return irBuilder.CreateAdd(memaddress, hintptr);
}

void EmitFunctionContext::memtag_hint(MemoryImm imm)
{
	::llvm::Value* hintindex = pop();
	::llvm::Value* hintptr = pop();
	::llvm::Value* memaddress = pop();
	if(isMemTaggedEnabled(*this))
	{
		memaddress = compute_hint_addr(*this, imm.memoryIndex, memaddress, hintptr, hintindex);
	}
	push(memaddress);
}

void EmitFunctionContext::memtag_hintstore(MemoryImm imm)
{
	::llvm::Value* hintindex = pop();
	::llvm::Value* hintptr = pop();
	::llvm::Value* taggedbytes = pop();
	::llvm::Value* memaddress = pop();
	if(isMemTaggedEnabled(*this))
	{
		auto hintres
			= compute_hint_addr_seperate(*this, imm.memoryIndex, memaddress, hintptr, hintindex);
		StoreTagIntoMem(
			*this, imm.memoryIndex, hintres.untaggedmemaddress, taggedbytes, hintres.color, true);
		memaddress = hintres.taggedmemaddress;
	}
	push(memaddress);
}

void EmitFunctionContext::memtag_hintstorez(MemoryImm imm)
{
	::llvm::Value* hintindex = pop();
	::llvm::Value* hintptr = pop();
	::llvm::Value* taggedbytes = pop();
	::llvm::Value* memaddress = pop();
	if(isMemTaggedEnabled(*this))
	{
		auto hintres
			= compute_hint_addr_seperate(*this, imm.memoryIndex, memaddress, hintptr, hintindex);
		StoreZTagIntoMem(
			*this, imm.memoryIndex, hintres.untaggedmemaddress, taggedbytes, hintres.color, true);
		memaddress = hintres.taggedmemaddress;
	}
	else { memtag_zero_memory(*this, imm.memoryIndex, memaddress, taggedbytes); }
	push(memaddress);
}

void EmitFunctionContext::memtag_sub(MemoryImm imm)
{
	::llvm::Value* ptrb = pop();
	::llvm::Value* ptra = pop();
	llvm::IRBuilder<>& irBuilder = this->irBuilder;

	if(isMemTaggedEnabled(*this))
	{
		const MemoryType& memoryType
			= this->moduleContext.irModule.memories.getType(imm.memoryIndex);
		uint_least64_t mask;
		if(memoryType.indexType == IndexType::i64) { mask = ::WAVM::IR::memtag64constants::mask; }
		else { mask = ::WAVM::IR::memtag32constants::mask; }
		ptra = irBuilder.CreateAnd(ptra, mask);
		ptrb = irBuilder.CreateAnd(ptrb, mask);
	}
	push(irBuilder.CreateSub(ptra, ptrb));
}
void EmitFunctionContext::memtag_copy(MemoryImm imm)
{
	::llvm::Value* memaddress2 = pop();
	::llvm::Value* memaddress1 = pop();
	if(isMemTaggedEnabled(*this))
	{
		llvm::IRBuilder<>& irBuilder = this->irBuilder;
		const MemoryType& memoryType
			= this->moduleContext.irModule.memories.getType(imm.memoryIndex);
		uint_least64_t maskcolor, mask;
		if(memoryType.indexType == IndexType::i64)
		{
			using constanttype = ::WAVM::IR::memtag64constants;
			maskcolor = constanttype::hint_mask;
			mask = constanttype::mask;
		}
		else
		{
			using constanttype = ::WAVM::IR::memtag32constants;
			maskcolor = constanttype::hint_mask;
			mask = constanttype::mask;
		}
		memaddress1 = irBuilder.CreateOr(irBuilder.CreateAnd(memaddress2, maskcolor),
										 irBuilder.CreateAnd(memaddress1, mask));
	}
	push(memaddress1);
}
void EmitFunctionContext::memtag_load(MemoryImm imm)
{
	::llvm::Value* memaddress = pop();
	if(isMemTaggedEnabled(*this))
	{
		memaddress = UntagAddress(*this, imm.memoryIndex, memaddress);
		if(this->isMemTagged == ::WAVM::LLVMJIT::memtagStatus::armmte)
		{
			if(this->moduleContext.targetArch == ::llvm::Triple::aarch64)
			{
				auto olduntaggedmemaddress{memaddress};
				memaddress = coerceAddressToPointer(
					getOffsetAndBoundedAddress(*this,
											   imm.memoryIndex,
											   memaddress,
											   0,
											   0,
											   BoundsCheckOp::clampToGuardRegion,
											   nullptr,
											   true),
					this->llvmContext.i8Type,
					imm.memoryIndex);
				memaddress = irBuilder.CreateIntrinsic(
					::llvm::Intrinsic::aarch64_ldg, {}, {memaddress, memaddress});
				memaddress = armmte64_to_32_old_value(
					*this, imm.memoryIndex, olduntaggedmemaddress, memaddress);
			}
		}
		else
		{
			auto realaddr = ComputeMemTagIndex(*this, imm.memoryIndex, memaddress, true);
			llvm::IRBuilder<>& irBuilder = this->irBuilder;

			const MemoryType& memoryType
				= this->moduleContext.irModule.memories.getType(imm.memoryIndex);
			::llvm::Value* color
				= ::WAVM::LLVMJIT::wavmCreateLoad(irBuilder, this->llvmContext.i8Type, realaddr);

			uint32_t shiftval{};
			::llvm::Type* extendtype;
			if(memoryType.indexType == IndexType::i64)
			{
				extendtype = this->llvmContext.i64Type;
				shiftval = ::WAVM::IR::memtag64constants::shifter;
			}
			else
			{
				extendtype = this->llvmContext.i32Type;
				shiftval = ::WAVM::IR::memtag32constants::shifter;
			}
			color = irBuilder.CreateZExt(color, extendtype);
			color = irBuilder.CreateShl(color, shiftval);
			memaddress = irBuilder.CreateOr(memaddress, color);
		}
	}
	push(memaddress);
}

//
// Load/store operators
//

#define EMIT_LOAD_OP(destType, name, llvmMemoryType, numBytesLog2, conversionOp)                   \
	void EmitFunctionContext::name(LoadOrStoreImm<numBytesLog2> imm)                               \
	{                                                                                              \
		auto address = pop();                                                                      \
		auto boundedAddress = getOffsetAndBoundedAddress(*this,                                    \
														 imm.memoryIndex,                          \
														 address,                                  \
														 U64(1 << numBytesLog2),                   \
														 imm.offset,                               \
														 BoundsCheckOp::clampToGuardRegion);       \
		auto pointer = coerceAddressToPointer(boundedAddress, llvmMemoryType, imm.memoryIndex);    \
		auto load = ::WAVM::LLVMJIT::wavmCreateLoad(irBuilder, llvmMemoryType, pointer);           \
		/* Don't trust the alignment hint provided by the WebAssembly code, since the load can't   \
		 * trap if it's wrong. */                                                                  \
		load->setAlignment(LLVM_ALIGNMENT(1));                                                     \
		load->setVolatile(true);                                                                   \
		push(conversionOp(load, destType));                                                        \
	}
#define EMIT_STORE_OP(name, llvmMemoryType, numBytesLog2, conversionOp)                            \
	void EmitFunctionContext::name(LoadOrStoreImm<numBytesLog2> imm)                               \
	{                                                                                              \
		auto value = pop();                                                                        \
		auto address = pop();                                                                      \
		auto boundedAddress = getOffsetAndBoundedAddress(*this,                                    \
														 imm.memoryIndex,                          \
														 address,                                  \
														 U64(1 << numBytesLog2),                   \
														 imm.offset,                               \
														 BoundsCheckOp::clampToGuardRegion);       \
		auto pointer = coerceAddressToPointer(boundedAddress, llvmMemoryType, imm.memoryIndex);    \
		auto memoryValue = conversionOp(value, llvmMemoryType);                                    \
		auto store = irBuilder.CreateStore(memoryValue, pointer);                                  \
		store->setVolatile(true);                                                                  \
		/* Don't trust the alignment hint provided by the WebAssembly code, since the store can't  \
		 * trap if it's wrong. */                                                                  \
		store->setAlignment(LLVM_ALIGNMENT(1));                                                    \
	}

EMIT_LOAD_OP(llvmContext.i32Type, i32_load8_s, llvmContext.i8Type, 0, sext)
EMIT_LOAD_OP(llvmContext.i32Type, i32_load8_u, llvmContext.i8Type, 0, zext)
EMIT_LOAD_OP(llvmContext.i32Type, i32_load16_s, llvmContext.i16Type, 1, sext)
EMIT_LOAD_OP(llvmContext.i32Type, i32_load16_u, llvmContext.i16Type, 1, zext)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load8_s, llvmContext.i8Type, 0, sext)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load8_u, llvmContext.i8Type, 0, zext)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load16_s, llvmContext.i16Type, 1, sext)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load16_u, llvmContext.i16Type, 1, zext)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load32_s, llvmContext.i32Type, 2, sext)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load32_u, llvmContext.i32Type, 2, zext)

EMIT_LOAD_OP(llvmContext.i32Type, i32_load, llvmContext.i32Type, 2, identity)
EMIT_LOAD_OP(llvmContext.i64Type, i64_load, llvmContext.i64Type, 3, identity)
EMIT_LOAD_OP(llvmContext.f32Type, f32_load, llvmContext.f32Type, 2, identity)
EMIT_LOAD_OP(llvmContext.f64Type, f64_load, llvmContext.f64Type, 3, identity)

EMIT_STORE_OP(i32_store8, llvmContext.i8Type, 0, trunc)
EMIT_STORE_OP(i64_store8, llvmContext.i8Type, 0, trunc)
EMIT_STORE_OP(i32_store16, llvmContext.i16Type, 1, trunc)
EMIT_STORE_OP(i64_store16, llvmContext.i16Type, 1, trunc)
EMIT_STORE_OP(i32_store, llvmContext.i32Type, 2, trunc)
EMIT_STORE_OP(i64_store32, llvmContext.i32Type, 2, trunc)
EMIT_STORE_OP(i64_store, llvmContext.i64Type, 3, identity)
EMIT_STORE_OP(f32_store, llvmContext.f32Type, 2, identity)
EMIT_STORE_OP(f64_store, llvmContext.f64Type, 3, identity)

EMIT_STORE_OP(v128_store, value->getType(), 4, identity)
EMIT_LOAD_OP(llvmContext.i64x2Type, v128_load, llvmContext.i64x2Type, 4, identity)

EMIT_LOAD_OP(llvmContext.i8x16Type, v128_load8_splat, llvmContext.i8Type, 0, splat<16>)
EMIT_LOAD_OP(llvmContext.i16x8Type, v128_load16_splat, llvmContext.i16Type, 1, splat<8>)
EMIT_LOAD_OP(llvmContext.i32x4Type, v128_load32_splat, llvmContext.i32Type, 2, splat<4>)
EMIT_LOAD_OP(llvmContext.i64x2Type, v128_load64_splat, llvmContext.i64Type, 3, splat<2>)

EMIT_LOAD_OP(llvmContext.i16x8Type, v128_load8x8_s, llvmContext.i8x8Type, 3, sext)
EMIT_LOAD_OP(llvmContext.i16x8Type, v128_load8x8_u, llvmContext.i8x8Type, 3, zext)
EMIT_LOAD_OP(llvmContext.i32x4Type, v128_load16x4_s, llvmContext.i16x4Type, 3, sext)
EMIT_LOAD_OP(llvmContext.i32x4Type, v128_load16x4_u, llvmContext.i16x4Type, 3, zext)
EMIT_LOAD_OP(llvmContext.i64x2Type, v128_load32x2_s, llvmContext.i32x2Type, 3, sext)
EMIT_LOAD_OP(llvmContext.i64x2Type, v128_load32x2_u, llvmContext.i32x2Type, 3, zext)

EMIT_LOAD_OP(llvmContext.i32x4Type,
			 v128_load32_zero,
			 llvmContext.i32Type,
			 2,
			 insertInZeroedVector<4>)
EMIT_LOAD_OP(llvmContext.i64x2Type,
			 v128_load64_zero,
			 llvmContext.i64Type,
			 3,
			 insertInZeroedVector<2>)

static void emitLoadLane(EmitFunctionContext& functionContext,
						 llvm::Type* llvmVectorType,
						 const BaseLoadOrStoreImm& loadOrStoreImm,
						 Uptr laneIndex,
						 Uptr numBytesLog2)
{
	llvm::Value* vector = functionContext.pop();
	vector = functionContext.irBuilder.CreateBitCast(vector, llvmVectorType);

	llvm::Value* address = functionContext.pop();
	llvm::Value* boundedAddress = getOffsetAndBoundedAddress(functionContext,
															 loadOrStoreImm.memoryIndex,
															 address,
															 U64(1) << numBytesLog2,
															 loadOrStoreImm.offset,
															 BoundsCheckOp::clampToGuardRegion);
	llvm::Value* pointer = functionContext.coerceAddressToPointer(
		boundedAddress, llvmVectorType->getScalarType(), loadOrStoreImm.memoryIndex);
	llvm::LoadInst* load = ::WAVM::LLVMJIT::wavmCreateLoad(
		functionContext.irBuilder, llvmVectorType->getScalarType(), pointer);
	// Don't trust the alignment hint provided by the WebAssembly code, since the load can't trap if
	// it's wrong.
	load->setAlignment(LLVM_ALIGNMENT(1));
	load->setVolatile(true);

	vector = functionContext.irBuilder.CreateInsertElement(vector, load, laneIndex);
	functionContext.push(vector);
}

static void emitStoreLane(EmitFunctionContext& functionContext,
						  llvm::Type* llvmVectorType,
						  const BaseLoadOrStoreImm& loadOrStoreImm,
						  Uptr laneIndex,
						  Uptr numBytesLog2)
{
	llvm::Value* vector = functionContext.pop();
	vector = functionContext.irBuilder.CreateBitCast(vector, llvmVectorType);

	auto lane = functionContext.irBuilder.CreateExtractElement(vector, laneIndex);

	llvm::Value* address = functionContext.pop();
	llvm::Value* boundedAddress = getOffsetAndBoundedAddress(functionContext,
															 loadOrStoreImm.memoryIndex,
															 address,
															 U64(1) << numBytesLog2,
															 loadOrStoreImm.offset,
															 BoundsCheckOp::clampToGuardRegion);
	llvm::Value* pointer = functionContext.coerceAddressToPointer(
		boundedAddress, lane->getType(), loadOrStoreImm.memoryIndex);
	llvm::StoreInst* store = functionContext.irBuilder.CreateStore(lane, pointer);
	// Don't trust the alignment hint provided by the WebAssembly code, since the load can't trap if
	// it's wrong.
	store->setAlignment(LLVM_ALIGNMENT(1));
	store->setVolatile(true);
}

#define EMIT_LOAD_LANE_OP(name, llvmVectorType, naturalAlignmentLog2, numLanes)                    \
	void EmitFunctionContext::name(LoadOrStoreLaneImm<naturalAlignmentLog2, numLanes> imm)         \
	{                                                                                              \
		emitLoadLane(*this, llvmVectorType, imm, imm.laneIndex, U64(1) << naturalAlignmentLog2);   \
	}
#define EMIT_STORE_LANE_OP(name, llvmVectorType, naturalAlignmentLog2, numLanes)                   \
	void EmitFunctionContext::name(LoadOrStoreLaneImm<naturalAlignmentLog2, numLanes> imm)         \
	{                                                                                              \
		emitStoreLane(*this, llvmVectorType, imm, imm.laneIndex, U64(1) << naturalAlignmentLog2);  \
	}
EMIT_LOAD_LANE_OP(v128_load8_lane, llvmContext.i8x16Type, 0, 16)
EMIT_LOAD_LANE_OP(v128_load16_lane, llvmContext.i16x8Type, 1, 8)
EMIT_LOAD_LANE_OP(v128_load32_lane, llvmContext.i32x4Type, 2, 4)
EMIT_LOAD_LANE_OP(v128_load64_lane, llvmContext.i64x2Type, 3, 2)
EMIT_STORE_LANE_OP(v128_store8_lane, llvmContext.i8x16Type, 0, 16)
EMIT_STORE_LANE_OP(v128_store16_lane, llvmContext.i16x8Type, 1, 8)
EMIT_STORE_LANE_OP(v128_store32_lane, llvmContext.i32x4Type, 2, 4)
EMIT_STORE_LANE_OP(v128_store64_lane, llvmContext.i64x2Type, 3, 2)

static void emitLoadInterleaved(EmitFunctionContext& functionContext,
								llvm::Type* llvmValueType,
								llvm::Intrinsic::ID aarch64IntrinsicID,
								const BaseLoadOrStoreImm& imm,
								U32 numVectors,
								U32 numLanes,
								U64 numBytes)
{
	static constexpr U32 maxVectors = 4;
	static constexpr U32 maxLanes = 16;
	WAVM_ASSERT(numVectors <= maxVectors);
	WAVM_ASSERT(numLanes <= maxLanes);

	auto address = functionContext.pop();
	auto boundedAddress = getOffsetAndBoundedAddress(functionContext,
													 imm.memoryIndex,
													 address,
													 numBytes,
													 imm.offset,
													 BoundsCheckOp::clampToGuardRegion);
	auto pointer
		= functionContext.coerceAddressToPointer(boundedAddress, llvmValueType, imm.memoryIndex);
	if(functionContext.moduleContext.targetArch == llvm::Triple::aarch64)
	{
		auto results = functionContext.callLLVMIntrinsic(
			{llvmValueType,
#if LLVM_VERSION_MAJOR > 14
			 ::llvm::PointerType::get(functionContext.llvmContext, 0)
#else
			 llvmValueType->getPointerTo()
#endif
			},
			aarch64IntrinsicID,
			{pointer});
		for(U32 vectorIndex = 0; vectorIndex < numVectors; ++vectorIndex)
		{
			functionContext.push(
				functionContext.irBuilder.CreateExtractValue(results, vectorIndex));
		}
	}
	else
	{
		llvm::Value* loads[maxVectors];
		for(U32 vectorIndex = 0; vectorIndex < numVectors; ++vectorIndex)
		{
			auto load = ::WAVM::LLVMJIT::wavmCreateLoad(
				functionContext.irBuilder,
				llvmValueType,
				::WAVM::LLVMJIT::wavmCreateInBoundsGEP(
					functionContext.irBuilder,
					functionContext.llvmContext.i8Type,
					pointer,
					{emitLiteral(functionContext.llvmContext, U32(vectorIndex))}));
			/* Don't trust the alignment hint provided by the WebAssembly code, since the load
			 * can't trap if it's wrong. */
			load->setAlignment(LLVM_ALIGNMENT(1));
			load->setVolatile(true);
			loads[vectorIndex] = load;
		}
		for(U32 vectorIndex = 0; vectorIndex < numVectors; ++vectorIndex)
		{
			llvm::Value* deinterleavedVector = llvm::UndefValue::get(llvmValueType);
			for(U32 laneIndex = 0; laneIndex < numLanes; ++laneIndex)
			{
				const Uptr interleavedElementIndex = laneIndex * numVectors + vectorIndex;
				deinterleavedVector = functionContext.irBuilder.CreateInsertElement(
					deinterleavedVector,
					functionContext.irBuilder.CreateExtractElement(
						loads[interleavedElementIndex / numLanes],
						interleavedElementIndex % numLanes),
					laneIndex);
			}
			functionContext.push(deinterleavedVector);
		}
	}
}

static void emitStoreInterleaved(EmitFunctionContext& functionContext,
								 llvm::Type* llvmValueType,
								 llvm::Intrinsic::ID aarch64IntrinsicID,
								 const IR::BaseLoadOrStoreImm& imm,
								 U32 numVectors,
								 U32 numLanes,
								 U64 numBytes)
{
	static constexpr U32 maxVectors = 4;
	WAVM_ASSERT(numVectors <= 4);

	llvm::Value* values[maxVectors];
	for(U32 vectorIndex = 0; vectorIndex < numVectors; ++vectorIndex)
	{
		values[numVectors - vectorIndex - 1]
			= functionContext.irBuilder.CreateBitCast(functionContext.pop(), llvmValueType);
	}
	auto address = functionContext.pop();
	auto boundedAddress = getOffsetAndBoundedAddress(functionContext,
													 imm.memoryIndex,
													 address,
													 numBytes,
													 imm.offset,
													 BoundsCheckOp::clampToGuardRegion);
	auto pointer
		= functionContext.coerceAddressToPointer(boundedAddress, llvmValueType, imm.memoryIndex);
	if(functionContext.moduleContext.targetArch == llvm::Triple::aarch64)
	{
		llvm::Value* args[maxVectors + 1];
		for(U32 vectorIndex = 0; vectorIndex < numVectors; ++vectorIndex)
		{
			args[vectorIndex] = values[vectorIndex];
			args[numVectors] = pointer;
		}
		functionContext.callLLVMIntrinsic({llvmValueType,
#if LLVM_VERSION_MAJOR > 14
										   ::llvm::PointerType::get(functionContext.llvmContext, 0)
#else
										   llvmValueType->getPointerTo()
#endif
										  },
										  aarch64IntrinsicID,
										  llvm::ArrayRef<llvm::Value*>(args, numVectors + 1));
	}
	else
	{
		for(U32 vectorIndex = 0; vectorIndex < numVectors; ++vectorIndex)
		{
			llvm::Value* interleavedVector = llvm::UndefValue::get(llvmValueType);
			for(U32 laneIndex = 0; laneIndex < numLanes; ++laneIndex)
			{
				const Uptr interleavedElementIndex = vectorIndex * numLanes + laneIndex;
				const Uptr deinterleavedVectorIndex = interleavedElementIndex % numVectors;
				const Uptr deinterleavedLaneIndex = interleavedElementIndex / numVectors;
				interleavedVector = functionContext.irBuilder.CreateInsertElement(
					interleavedVector,
					functionContext.irBuilder.CreateExtractElement(values[deinterleavedVectorIndex],
																   deinterleavedLaneIndex),
					laneIndex);
			}
			auto store = functionContext.irBuilder.CreateStore(
				interleavedVector,
				::WAVM::LLVMJIT::wavmCreateInBoundsGEP(
					functionContext.irBuilder,
					functionContext.llvmContext.i8Type,
					pointer,
					{emitLiteral(functionContext.llvmContext, U32(vectorIndex))}));
			store->setVolatile(true);
			store->setAlignment(LLVM_ALIGNMENT(1));
		}
	}
}

#define EMIT_LOAD_INTERLEAVED_OP(name, llvmValueType, naturalAlignmentLog2, numVectors, numLanes)  \
	void EmitFunctionContext::name(LoadOrStoreImm<naturalAlignmentLog2> imm)                       \
	{                                                                                              \
		emitLoadInterleaved(*this,                                                                 \
							llvmValueType,                                                         \
							llvm::Intrinsic::aarch64_neon_ld##numVectors,                          \
							imm,                                                                   \
							numVectors,                                                            \
							numLanes,                                                              \
							U64(1 << naturalAlignmentLog2) * numVectors);                          \
	}

#define EMIT_STORE_INTERLEAVED_OP(name, llvmValueType, naturalAlignmentLog2, numVectors, numLanes) \
	void EmitFunctionContext::name(LoadOrStoreImm<naturalAlignmentLog2> imm)                       \
	{                                                                                              \
		emitStoreInterleaved(*this,                                                                \
							 llvmValueType,                                                        \
							 llvm::Intrinsic::aarch64_neon_st##numVectors,                         \
							 imm,                                                                  \
							 numVectors,                                                           \
							 numLanes,                                                             \
							 U64(1 << naturalAlignmentLog2) * numVectors);                         \
	}

EMIT_LOAD_INTERLEAVED_OP(v8x16_load_interleaved_2, llvmContext.i8x16Type, 4, 2, 16)
EMIT_LOAD_INTERLEAVED_OP(v8x16_load_interleaved_3, llvmContext.i8x16Type, 4, 3, 16)
EMIT_LOAD_INTERLEAVED_OP(v8x16_load_interleaved_4, llvmContext.i8x16Type, 4, 4, 16)
EMIT_LOAD_INTERLEAVED_OP(v16x8_load_interleaved_2, llvmContext.i16x8Type, 4, 2, 8)
EMIT_LOAD_INTERLEAVED_OP(v16x8_load_interleaved_3, llvmContext.i16x8Type, 4, 3, 8)
EMIT_LOAD_INTERLEAVED_OP(v16x8_load_interleaved_4, llvmContext.i16x8Type, 4, 4, 8)
EMIT_LOAD_INTERLEAVED_OP(v32x4_load_interleaved_2, llvmContext.i32x4Type, 4, 2, 4)
EMIT_LOAD_INTERLEAVED_OP(v32x4_load_interleaved_3, llvmContext.i32x4Type, 4, 3, 4)
EMIT_LOAD_INTERLEAVED_OP(v32x4_load_interleaved_4, llvmContext.i32x4Type, 4, 4, 4)
EMIT_LOAD_INTERLEAVED_OP(v64x2_load_interleaved_2, llvmContext.i64x2Type, 4, 2, 2)
EMIT_LOAD_INTERLEAVED_OP(v64x2_load_interleaved_3, llvmContext.i64x2Type, 4, 3, 2)
EMIT_LOAD_INTERLEAVED_OP(v64x2_load_interleaved_4, llvmContext.i64x2Type, 4, 4, 2)

EMIT_STORE_INTERLEAVED_OP(v8x16_store_interleaved_2, llvmContext.i8x16Type, 4, 2, 16)
EMIT_STORE_INTERLEAVED_OP(v8x16_store_interleaved_3, llvmContext.i8x16Type, 4, 3, 16)
EMIT_STORE_INTERLEAVED_OP(v8x16_store_interleaved_4, llvmContext.i8x16Type, 4, 4, 16)
EMIT_STORE_INTERLEAVED_OP(v16x8_store_interleaved_2, llvmContext.i16x8Type, 4, 2, 8)
EMIT_STORE_INTERLEAVED_OP(v16x8_store_interleaved_3, llvmContext.i16x8Type, 4, 3, 8)
EMIT_STORE_INTERLEAVED_OP(v16x8_store_interleaved_4, llvmContext.i16x8Type, 4, 4, 8)
EMIT_STORE_INTERLEAVED_OP(v32x4_store_interleaved_2, llvmContext.i32x4Type, 4, 2, 4)
EMIT_STORE_INTERLEAVED_OP(v32x4_store_interleaved_3, llvmContext.i32x4Type, 4, 3, 4)
EMIT_STORE_INTERLEAVED_OP(v32x4_store_interleaved_4, llvmContext.i32x4Type, 4, 4, 4)
EMIT_STORE_INTERLEAVED_OP(v64x2_store_interleaved_2, llvmContext.i64x2Type, 4, 2, 2)
EMIT_STORE_INTERLEAVED_OP(v64x2_store_interleaved_3, llvmContext.i64x2Type, 4, 3, 2)
EMIT_STORE_INTERLEAVED_OP(v64x2_store_interleaved_4, llvmContext.i64x2Type, 4, 4, 2)

void EmitFunctionContext::trapIfMisalignedAtomic(llvm::Value* address, U32 alignmentLog2)
{
	if(alignmentLog2 > 0)
	{
		emitConditionalTrapIntrinsic(
			irBuilder.CreateICmpNE(
				llvmContext.typedZeroConstants[(Uptr)ValueType::i64],
				irBuilder.CreateAnd(
					address, emitLiteralIptr((U64(1) << alignmentLog2) - 1, address->getType()))),
			"misalignedAtomicTrap",
			FunctionType(TypeTuple{}, TypeTuple{ValueType::i64}, IR::CallingConvention::intrinsic),
			{address});
	}
}

void EmitFunctionContext::memory_atomic_notify(AtomicLoadOrStoreImm<2> imm)
{
	llvm::Value* numWaiters = pop();
	llvm::Value* address = pop();
	llvm::Value* boundedAddress = getOffsetAndBoundedAddress(
		*this, imm.memoryIndex, address, U64(4), imm.offset, BoundsCheckOp::clampToGuardRegion);
	trapIfMisalignedAtomic(boundedAddress, imm.alignmentLog2);
	push(emitRuntimeIntrinsic(
		"memory.atomic.notify",
		FunctionType(
			TypeTuple{ValueType::i32},
			TypeTuple{moduleContext.iptrValueType, ValueType::i32, moduleContext.iptrValueType},
			IR::CallingConvention::intrinsic),
		{boundedAddress,
		 numWaiters,
		 getMemoryIdFromOffset(irBuilder, moduleContext.memoryOffsets[imm.memoryIndex])})[0]);
}
void EmitFunctionContext::memory_atomic_wait32(AtomicLoadOrStoreImm<2> imm)
{
	llvm::Value* timeout = pop();
	llvm::Value* expectedValue = pop();
	llvm::Value* address = pop();
	llvm::Value* boundedAddress = getOffsetAndBoundedAddress(
		*this, imm.memoryIndex, address, U64(4), imm.offset, BoundsCheckOp::clampToGuardRegion);
	trapIfMisalignedAtomic(boundedAddress, imm.alignmentLog2);
	push(emitRuntimeIntrinsic(
		"memory.atomic.wait32",
		FunctionType(TypeTuple{ValueType::i32},
					 TypeTuple{moduleContext.iptrValueType,
							   ValueType::i32,
							   ValueType::i64,
							   moduleContext.iptrValueType},
					 IR::CallingConvention::intrinsic),
		{boundedAddress,
		 expectedValue,
		 timeout,
		 getMemoryIdFromOffset(irBuilder, moduleContext.memoryOffsets[imm.memoryIndex])})[0]);
}
void EmitFunctionContext::memory_atomic_wait64(AtomicLoadOrStoreImm<3> imm)
{
	llvm::Value* timeout = pop();
	llvm::Value* expectedValue = pop();
	llvm::Value* address = pop();
	llvm::Value* boundedAddress = getOffsetAndBoundedAddress(
		*this, imm.memoryIndex, address, U64(8), imm.offset, BoundsCheckOp::clampToGuardRegion);
	trapIfMisalignedAtomic(boundedAddress, imm.alignmentLog2);
	push(emitRuntimeIntrinsic(
		"memory.atomic.wait64",
		FunctionType(TypeTuple{ValueType::i32},
					 TypeTuple{moduleContext.iptrValueType,
							   ValueType::i64,
							   ValueType::i64,
							   moduleContext.iptrValueType},
					 IR::CallingConvention::intrinsic),
		{boundedAddress,
		 expectedValue,
		 timeout,
		 getMemoryIdFromOffset(irBuilder, moduleContext.memoryOffsets[imm.memoryIndex])})[0]);
}

void EmitFunctionContext::atomic_fence(AtomicFenceImm imm)
{
	switch(imm.order)
	{
	case MemoryOrder::sequentiallyConsistent:
		irBuilder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
		break;
	default: WAVM_UNREACHABLE();
	};
}

#define EMIT_ATOMIC_LOAD_OP(valueTypeId, name, llvmMemoryType, numBytesLog2, memToValue)           \
	void EmitFunctionContext::valueTypeId##_##name(AtomicLoadOrStoreImm<numBytesLog2> imm)         \
	{                                                                                              \
		auto address = pop();                                                                      \
		auto boundedAddress = getOffsetAndBoundedAddress(*this,                                    \
														 imm.memoryIndex,                          \
														 address,                                  \
														 U64(1 << numBytesLog2),                   \
														 imm.offset,                               \
														 BoundsCheckOp::clampToGuardRegion);       \
		trapIfMisalignedAtomic(boundedAddress, numBytesLog2);                                      \
		auto pointer = coerceAddressToPointer(boundedAddress, llvmMemoryType, imm.memoryIndex);    \
		auto load = ::WAVM::LLVMJIT::wavmCreateLoad(irBuilder, address->getType(), pointer);       \
		load->setAlignment(LLVM_ALIGNMENT(U64(1) << imm.alignmentLog2));                           \
		load->setVolatile(true);                                                                   \
		load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);                             \
		push(memToValue(load, asLLVMType(llvmContext, ValueType::valueTypeId)));                   \
	}
#define EMIT_ATOMIC_STORE_OP(valueTypeId, name, llvmMemoryType, numBytesLog2, valueToMem)          \
	void EmitFunctionContext::valueTypeId##_##name(AtomicLoadOrStoreImm<numBytesLog2> imm)         \
	{                                                                                              \
		auto value = pop();                                                                        \
		auto address = pop();                                                                      \
		auto boundedAddress = getOffsetAndBoundedAddress(*this,                                    \
														 imm.memoryIndex,                          \
														 address,                                  \
														 U64(1 << numBytesLog2),                   \
														 imm.offset,                               \
														 BoundsCheckOp::clampToGuardRegion);       \
		trapIfMisalignedAtomic(boundedAddress, numBytesLog2);                                      \
		auto pointer = coerceAddressToPointer(boundedAddress, llvmMemoryType, imm.memoryIndex);    \
		auto memoryValue = valueToMem(value, llvmMemoryType);                                      \
		auto store = irBuilder.CreateStore(memoryValue, pointer);                                  \
		store->setVolatile(true);                                                                  \
		store->setAlignment(LLVM_ALIGNMENT(U64(1) << imm.alignmentLog2));                          \
		store->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);                            \
	}
EMIT_ATOMIC_LOAD_OP(i32, atomic_load, llvmContext.i32Type, 2, identity)
EMIT_ATOMIC_LOAD_OP(i64, atomic_load, llvmContext.i64Type, 3, identity)

EMIT_ATOMIC_LOAD_OP(i32, atomic_load8_u, llvmContext.i8Type, 0, zext)
EMIT_ATOMIC_LOAD_OP(i32, atomic_load16_u, llvmContext.i16Type, 1, zext)
EMIT_ATOMIC_LOAD_OP(i64, atomic_load8_u, llvmContext.i8Type, 0, zext)
EMIT_ATOMIC_LOAD_OP(i64, atomic_load16_u, llvmContext.i16Type, 1, zext)
EMIT_ATOMIC_LOAD_OP(i64, atomic_load32_u, llvmContext.i32Type, 2, zext)

EMIT_ATOMIC_STORE_OP(i32, atomic_store, llvmContext.i32Type, 2, identity)
EMIT_ATOMIC_STORE_OP(i64, atomic_store, llvmContext.i64Type, 3, identity)

EMIT_ATOMIC_STORE_OP(i32, atomic_store8, llvmContext.i8Type, 0, trunc)
EMIT_ATOMIC_STORE_OP(i32, atomic_store16, llvmContext.i16Type, 1, trunc)
EMIT_ATOMIC_STORE_OP(i64, atomic_store8, llvmContext.i8Type, 0, trunc)
EMIT_ATOMIC_STORE_OP(i64, atomic_store16, llvmContext.i16Type, 1, trunc)
EMIT_ATOMIC_STORE_OP(i64, atomic_store32, llvmContext.i32Type, 2, trunc)

#define EMIT_ATOMIC_CMPXCHG(                                                                       \
	valueTypeId, name, llvmMemoryType, numBytesLog2, memToValue, valueToMem)                       \
	void EmitFunctionContext::valueTypeId##_##name(AtomicLoadOrStoreImm<numBytesLog2> imm)         \
	{                                                                                              \
		auto replacementValue = valueToMem(pop(), llvmMemoryType);                                 \
		auto expectedValue = valueToMem(pop(), llvmMemoryType);                                    \
		auto address = pop();                                                                      \
		auto boundedAddress = getOffsetAndBoundedAddress(*this,                                    \
														 imm.memoryIndex,                          \
														 address,                                  \
														 U64(1 << numBytesLog2),                   \
														 imm.offset,                               \
														 BoundsCheckOp::clampToGuardRegion);       \
		trapIfMisalignedAtomic(boundedAddress, numBytesLog2);                                      \
		auto pointer = coerceAddressToPointer(boundedAddress, llvmMemoryType, imm.memoryIndex);    \
		auto atomicCmpXchg = ::WAVM::LLVMJIT::wavmCreateAtomicCmpXchg(                             \
			irBuilder,                                                                             \
			pointer,                                                                               \
			expectedValue,                                                                         \
			replacementValue,                                                                      \
			llvm::AtomicOrdering::SequentiallyConsistent,                                          \
			llvm::AtomicOrdering::SequentiallyConsistent);                                         \
		atomicCmpXchg->setVolatile(true);                                                          \
		auto previousValue = irBuilder.CreateExtractValue(atomicCmpXchg, {0});                     \
		push(memToValue(previousValue, asLLVMType(llvmContext, ValueType::valueTypeId)));          \
	}

EMIT_ATOMIC_CMPXCHG(i32, atomic_rmw8_cmpxchg_u, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_CMPXCHG(i32, atomic_rmw16_cmpxchg_u, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_CMPXCHG(i32, atomic_rmw_cmpxchg, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_CMPXCHG(i64, atomic_rmw8_cmpxchg_u, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_CMPXCHG(i64, atomic_rmw16_cmpxchg_u, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_CMPXCHG(i64, atomic_rmw32_cmpxchg_u, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_CMPXCHG(i64, atomic_rmw_cmpxchg, llvmContext.i64Type, 3, identity, identity)

#define EMIT_ATOMIC_RMW(                                                                           \
	valueTypeId, name, rmwOpId, llvmMemoryType, numBytesLog2, memToValue, valueToMem)              \
	void EmitFunctionContext::valueTypeId##_##name(AtomicLoadOrStoreImm<numBytesLog2> imm)         \
	{                                                                                              \
		auto value = valueToMem(pop(), llvmMemoryType);                                            \
		auto address = pop();                                                                      \
		auto boundedAddress = getOffsetAndBoundedAddress(*this,                                    \
														 imm.memoryIndex,                          \
														 address,                                  \
														 U64(1 << numBytesLog2),                   \
														 imm.offset,                               \
														 BoundsCheckOp::clampToGuardRegion);       \
		trapIfMisalignedAtomic(boundedAddress, numBytesLog2);                                      \
		auto pointer = coerceAddressToPointer(boundedAddress, llvmMemoryType, imm.memoryIndex);    \
		auto atomicRMW                                                                             \
			= ::WAVM::LLVMJIT::wavmCreateAtomicRMW(irBuilder,                                      \
												   llvm::AtomicRMWInst::BinOp::rmwOpId,            \
												   pointer,                                        \
												   value,                                          \
												   llvm::AtomicOrdering::SequentiallyConsistent);  \
		atomicRMW->setVolatile(true);                                                              \
		push(memToValue(atomicRMW, asLLVMType(llvmContext, ValueType::valueTypeId)));              \
	}

EMIT_ATOMIC_RMW(i32, atomic_rmw8_xchg_u, Xchg, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw16_xchg_u, Xchg, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw_xchg, Xchg, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_RMW(i64, atomic_rmw8_xchg_u, Xchg, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw16_xchg_u, Xchg, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw32_xchg_u, Xchg, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw_xchg, Xchg, llvmContext.i64Type, 3, identity, identity)

EMIT_ATOMIC_RMW(i32, atomic_rmw8_add_u, Add, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw16_add_u, Add, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw_add, Add, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_RMW(i64, atomic_rmw8_add_u, Add, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw16_add_u, Add, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw32_add_u, Add, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw_add, Add, llvmContext.i64Type, 3, identity, identity)

EMIT_ATOMIC_RMW(i32, atomic_rmw8_sub_u, Sub, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw16_sub_u, Sub, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw_sub, Sub, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_RMW(i64, atomic_rmw8_sub_u, Sub, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw16_sub_u, Sub, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw32_sub_u, Sub, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw_sub, Sub, llvmContext.i64Type, 3, identity, identity)

EMIT_ATOMIC_RMW(i32, atomic_rmw8_and_u, And, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw16_and_u, And, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw_and, And, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_RMW(i64, atomic_rmw8_and_u, And, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw16_and_u, And, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw32_and_u, And, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw_and, And, llvmContext.i64Type, 3, identity, identity)

EMIT_ATOMIC_RMW(i32, atomic_rmw8_or_u, Or, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw16_or_u, Or, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw_or, Or, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_RMW(i64, atomic_rmw8_or_u, Or, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw16_or_u, Or, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw32_or_u, Or, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw_or, Or, llvmContext.i64Type, 3, identity, identity)

EMIT_ATOMIC_RMW(i32, atomic_rmw8_xor_u, Xor, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw16_xor_u, Xor, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i32, atomic_rmw_xor, Xor, llvmContext.i32Type, 2, identity, identity)

EMIT_ATOMIC_RMW(i64, atomic_rmw8_xor_u, Xor, llvmContext.i8Type, 0, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw16_xor_u, Xor, llvmContext.i16Type, 1, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw32_xor_u, Xor, llvmContext.i32Type, 2, zext, trunc)
EMIT_ATOMIC_RMW(i64, atomic_rmw_xor, Xor, llvmContext.i64Type, 3, identity, identity)
