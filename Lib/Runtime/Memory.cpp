#include "WAVM/Platform/Memory.h"
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <random>
#include <vector>
#include "RuntimePrivate.h"
#include "WAVM/IR/IR.h"
#include "WAVM/IR/Types.h"
#include "WAVM/IR/Value.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Platform/Intrinsic.h"
#include "WAVM/Platform/RWMutex.h"
#include "WAVM/Platform/Random.h"
#include "WAVM/Runtime/Runtime.h"
#include "WAVM/RuntimeABI/RuntimeABI.h"

using namespace WAVM;
using namespace WAVM::Runtime;

namespace WAVM { namespace Runtime {
	WAVM_DEFINE_INTRINSIC_MODULE(wavmIntrinsicsMemory)
}}

// Global lists of memories; used to query whether an address is reserved by one of them.
static Platform::RWMutex memoriesMutex;
static std::vector<Memory*> memories;

static inline constexpr U64 maxMemory64WASMPages = IR::maxMemory64WASMPages;

inline constexpr auto maxMemory64WASMTagsPages
	= maxMemory64WASMPages / 16u + (maxMemory64WASMPages % 16u != 0);

static inline Uptr getPlatformPagesPerWebAssemblyPageLog2()
{
	auto v{Platform::getBytesPerPageLog2()};
	WAVM_ERROR_UNLESS(4u <= v && v <= IR::numBytesPerPageLog2);
	return IR::numBytesPerPageLog2 - v;
}

static inline void wavm_random_tag_fill_buffer_function(U8* base, U8* ed) noexcept
{
	try
	{
		::WAVM::Platform::getCryptographicRNG(reinterpret_cast<U8*>(base),
											  static_cast<::std::size_t>(ed - base));
	}
	catch(...)
	{
		::std::abort();
	}
}

static MemtagRandomBufferInMemory createMemoryTagRandomBufferImpl(U8 mask) noexcept
{
	::std::random_device eng;
	::std::uniform_int_distribution<::std::size_t> dis(4096, 16384);
	::std::size_t buffersize{dis(eng)};
	U8* ptr = reinterpret_cast<U8*>(::std::malloc(buffersize));
	if(ptr == nullptr) { ::std::abort(); }
	U8* ed = ptr + buffersize;
	wavm_random_tag_fill_buffer_function(ptr, ed);
	constexpr U8 u8mx{::std::numeric_limits<U8>::max()};
	if(mask != u8mx)
	{
		for(auto it{ptr}; it != ed; ++it) { *it &= mask; }
	}
	return {ptr, ed};
}

static inline constexpr U8 computeMemtagBufferMask(IR::IndexType idx) noexcept
{
	return static_cast<U8>(idx == IR::IndexType::i32 ? ::WAVM::IR::memtag32constants::index_mask
													 : ::WAVM::IR::memtag64constants::index_mask);
}

static Memory* createMemoryImpl(Compartment* compartment,
								IR::MemoryType type,
								std::string&& debugName,
								::WAVM::LLVMJIT::memtagStatus isMemTagged,
								ResourceQuotaRefParam resourceQuota)
{
	::std::unique_ptr<Memory> memoryuptr(
		new Memory(compartment, type, std::move(debugName), resourceQuota, isMemTagged));

	Memory* memory = memoryuptr.get();

	const Uptr pageBytesLog2 = Platform::getBytesPerPageLog2();

	Uptr memoryMaxPages;
	if(type.indexType == IR::IndexType::i32)
	{
		static_assert(sizeof(Uptr) == 8, "WAVM's runtime requires a 64-bit host");

		// For 32-bit memories on a 64-bit runtime, allocate 8GB of address space for the memory.
		// This allows eliding bounds checks on memory accesses, since a 32-bit index + 32-bit
		// offset will always be within the reserved address-space.
		memoryMaxPages = (Uptr(8) * 1024 * 1024 * 1024) >> pageBytesLog2;
	}
	else
	{
		// Clamp the maximum size of 64-bit memories to maxMemory64Bytes.
		memoryMaxPages = std::min(type.size.max, maxMemory64WASMPages);

		// Convert maxMemoryPages from fixed size WASM pages (64KB) to platform-specific pages.
		memoryMaxPages <<= getPlatformPagesPerWebAssemblyPageLog2();
	}

	const Uptr numGuardPages = memoryNumGuardBytes >> pageBytesLog2;
	auto totalpages = memoryMaxPages + numGuardPages;
	memory->baseAddress = Platform::allocateVirtualPages(totalpages);
	if(isMemTagged == ::WAVM::LLVMJIT::memtagStatus::basic
	   || isMemTagged == ::WAVM::LLVMJIT::memtagStatus::full)
	{
		auto totaltaggedpages = (totalpages >> 4u) + (totalpages & 15u);
		auto baseAddressTags = Platform::allocateVirtualPages(totaltaggedpages);
		if(!baseAddressTags) { return nullptr; }
		memory->baseAddressTags = baseAddressTags;
		memory->memtagRandomBuffer
			= createMemoryTagRandomBufferImpl(computeMemtagBufferMask(type.indexType));
	}
	else if(isMemTagged == ::WAVM::LLVMJIT::memtagStatus::armmte)
	{
		memory->memtagRandomBuffer = createMemoryTagRandomBufferImpl(
			static_cast<U8>(::WAVM::IR::memtagarmmteconstants::index_mask));
	}
	memory->numReservedBytes = memoryMaxPages << pageBytesLog2;
	if(!memory->baseAddress) { return nullptr; }

	// Grow the memory to the type's minimum size.
	if(growMemory(memory, type.size.min) != GrowResult::success) { return nullptr; }

	// Add the memory to the global array.
	{
		Platform::RWMutex::ExclusiveLock memoriesLock(memoriesMutex);
		memories.push_back(memory);
	}

	return memoryuptr.release();
}

Memory* Runtime::createMemory(Compartment* compartment,
							  IR::MemoryType type,
							  std::string&& debugName,
							  ::WAVM::LLVMJIT::memtagStatus isMemTagged,
							  ResourceQuotaRefParam resourceQuota)
{
	WAVM_ASSERT(type.size.min <= UINTPTR_MAX);
	Memory* memory
		= createMemoryImpl(compartment, type, std::move(debugName), isMemTagged, resourceQuota);
	if(!memory) { return nullptr; }
	::std::unique_ptr<Memory> memoryuptr(memory);
	// Add the memory to the compartment's memories IndexMap.
	{
		Platform::RWMutex::ExclusiveLock compartmentLock(compartment->mutex);

		memory->id = compartment->memories.add(UINTPTR_MAX, memory);
		if(memory->id == UINTPTR_MAX) { return nullptr; }
		MemoryRuntimeData& runtimeData = compartment->runtimeData->memories[memory->id];
		runtimeData.base = memory->baseAddress;
		runtimeData.endAddress = memory->numReservedBytes;
		runtimeData.memtagBase = memory->baseAddressTags;
		memoryuptr->memtagstatus = isMemTagged;
		if(isMemTagged == ::WAVM::LLVMJIT::memtagStatus::basic
		   || isMemTagged == ::WAVM::LLVMJIT::memtagStatus::full)
		{
			auto memtagrdbf{memory->memtagRandomBuffer};
			runtimeData.memtagRandomBuffer = {memtagrdbf.Base, memtagrdbf.End, memtagrdbf.End};
		}
		else { runtimeData.memtagRandomBuffer = {}; }
		runtimeData.numPages.store(memory->numPages.load(std::memory_order_acquire),
								   std::memory_order_release);
	}

	return memoryuptr.release();
}

Memory* Runtime::cloneMemory(Memory* memory, Compartment* newCompartment)
{
	Platform::RWMutex::ExclusiveLock resizingLock(memory->resizingMutex);
	const IR::MemoryType memoryType = getMemoryType(memory);
	std::string debugName = memory->debugName;
	bool const ismemtagged{memory->baseAddressTags != nullptr};
	Memory* newMemory = createMemoryImpl(newCompartment,
										 memoryType,
										 std::move(debugName),
										 memory->memtagstatus,
										 memory->resourceQuota);
	if(!newMemory) { return nullptr; }

	// Copy the memory contents to the new memory.
	memcpy(newMemory->baseAddress, memory->baseAddress, memoryType.size.min * IR::numBytesPerPage);
	if(ismemtagged)
	{
		memcpy(newMemory->baseAddressTags,
			   memory->baseAddressTags,
			   memoryType.size.min * (IR::numBytesPerPage >> 4u));
		newMemory->memtagRandomBuffer
			= createMemoryTagRandomBufferImpl(computeMemtagBufferMask(memoryType.indexType));
	}
	resizingLock.unlock();

	// Insert the memory in the new compartment's memories array with the same index as it had in
	// the original compartment's memories IndexMap.
	{
		Platform::RWMutex::ExclusiveLock compartmentLock(newCompartment->mutex);

		newMemory->id = memory->id;
		newCompartment->memories.insertOrFail(newMemory->id, newMemory);

		MemoryRuntimeData& runtimeData = newCompartment->runtimeData->memories[newMemory->id];
		runtimeData.base = newMemory->baseAddress;
		runtimeData.numPages.store(newMemory->numPages, std::memory_order_release);
		runtimeData.endAddress = newMemory->numReservedBytes;
		runtimeData.memtagBase = newMemory->baseAddressTags;
		auto memtagrdbf{newMemory->memtagRandomBuffer};
		if(memtagrdbf.Base)
		{
			runtimeData.memtagRandomBuffer = {memtagrdbf.Base, memtagrdbf.End, memtagrdbf.End};
		}
		else { runtimeData.memtagRandomBuffer = {}; }
	}

	return newMemory;
}

Runtime::Memory::~Memory()
{
	if(id != UINTPTR_MAX)
	{
		WAVM_ASSERT_RWMUTEX_IS_EXCLUSIVELY_LOCKED_BY_CURRENT_THREAD(compartment->mutex);

		WAVM_ASSERT(compartment->memories[id] == this);
		compartment->memories.removeOrFail(id);

		MemoryRuntimeData& runtimeData = compartment->runtimeData->memories[id];
		WAVM_ASSERT(runtimeData.base == baseAddress);
		runtimeData.base = nullptr;
		runtimeData.numPages.store(0, std::memory_order_release);
		runtimeData.endAddress = 0;
		runtimeData.memtagBase = nullptr;
		runtimeData.memtagRandomBuffer = {};
	}

	// Remove the memory from the global array.
	{
		Platform::RWMutex::ExclusiveLock memoriesLock(memoriesMutex);
		for(Uptr memoryIndex = 0; memoryIndex < memories.size(); ++memoryIndex)
		{
			if(memories[memoryIndex] == this)
			{
				memories.erase(memories.begin() + memoryIndex);
				break;
			}
		}
	}

	// Free the virtual address space.
	const Uptr pageBytesLog2 = Platform::getBytesPerPageLog2();
	if(baseAddress && numReservedBytes > 0)
	{
		Platform::freeVirtualPages(baseAddress,
								   (numReservedBytes + memoryNumGuardBytes) >> pageBytesLog2);

		Platform::deregisterVirtualAllocation(numPages >> pageBytesLog2);
	}
	if(baseAddressTags && numReservedBytes > 0)
	{
		auto wasmlog2m4 = pageBytesLog2 + 4u;
		Platform::freeVirtualPages(baseAddressTags,
								   (numReservedBytes + memoryNumGuardBytes) >> wasmlog2m4);
		Platform::deregisterVirtualAllocation(numPages >> wasmlog2m4);
	}

	// Free the allocated quota.
	if(resourceQuota) { resourceQuota->memoryPages.free(numPages); }

	if(memtagRandomBuffer.Base)
	{
		::WAVM::Utils::secure_clear(
			memtagRandomBuffer.Base,
			static_cast<::std::size_t>(memtagRandomBuffer.End - memtagRandomBuffer.Base));
		free(memtagRandomBuffer.Base);
	}
}

bool Runtime::isAddressOwnedByMemory(U8* address, Memory*& outMemory, Uptr& outMemoryAddress)
{
	// Iterate over all memories and check if the address is within the reserved address space for
	// each.
	Platform::RWMutex::ShareableLock memoriesLock(memoriesMutex);
	for(auto memory : memories)
	{
		U8* startAddress = memory->baseAddress;
		U8* endAddress = memory->baseAddress + memory->numReservedBytes + memoryNumGuardBytes;
		if(address >= startAddress && address < endAddress)
		{
			outMemory = memory;
			outMemoryAddress = address - startAddress;
			return true;
		}
	}
	return false;
}

Uptr Runtime::getMemoryNumPages(const Memory* memory)
{
	return memory->numPages.load(std::memory_order_seq_cst);
}
IR::MemoryType Runtime::getMemoryType(const Memory* memory)
{
	return IR::MemoryType{memory->isShared,
						  memory->indexType,
						  IR::SizeConstraints{getMemoryNumPages(memory), memory->maxPages}};
}

#if defined(__aarch64__) && (!defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__))

namespace {
	inline constexpr bool platform_support_arm_mte{true};
	inline void* wavm_arm_mte_irg(void* ptr, uintptr_t mask) noexcept
	{
#if __has_builtin(__builtin_arm_irg)
		return __builtin_arm_irg(ptr, mask);
#else
		uintptr_t val;
		__asm__(
			".arch_extension memtag\n"
			"irg %0, %1, %2"
			: "=r"(val)
			: "r"(reinterpret_cast<uintptr_t>(ptr)), "r"(mask));
		return reinterpret_cast<void*>(val);
#endif
	}

	inline void wavm_arm_mte_stg(void* tagged_addr) noexcept
	{
#if __has_builtin(__builtin_arm_stg)
		__builtin_arm_stg(tagged_addr);
#else
		__asm__ __volatile__(
			".arch_extension memtag\n"
			"stg %0, [%0]"
			:
			: "r"(reinterpret_cast<uintptr_t>(tagged_addr))
			: "memory");
#endif
	}

	inline void wavm_arm_mte_st2g(void* tagged_addr) noexcept
	{
#if __has_builtin(__builtin_arm_st2g)
		__builtin_arm_st2g(tagged_addr);
#else
		__asm__ __volatile__(
			".arch_extension memtag\n"
			"st2g %0, [%0]"
			:
			: "r"(reinterpret_cast<uintptr_t>(tagged_addr))
			: "memory");
#endif
	}

	inline void wavm_arm_mte_stzg(void* tagged_addr) noexcept
	{
#if __has_builtin(__builtin_arm_stzg)
		__builtin_arm_stzg(tagged_addr);
#else
		__asm__ __volatile__(
			".arch_extension memtag\n"
			"stzg %0, [%0]"
			:
			: "r"(reinterpret_cast<uintptr_t>(tagged_addr))
			: "memory");
#endif
	}

	inline void wavm_arm_mte_stz2g(void* tagged_addr) noexcept
	{
#if __has_builtin(__builtin_arm_stz2g)
		__builtin_arm_stz2g(tagged_addr);
#else
		__asm__ __volatile__(
			".arch_extension memtag\n"
			"stz2g %0, [%0]"
			:
			: "r"(reinterpret_cast<uintptr_t>(tagged_addr))
			: "memory");
#endif
	}

}

extern "C" void wavm_aarch64_mte_settag(void* ptrvp, ::std::size_t len) noexcept
{
	char* ptr(reinterpret_cast<char*>(ptrvp));
	while(len >= 32)
	{
		wavm_arm_mte_st2g(ptr);
		ptr += 32;
		len -= 32;
	}
	if(len >= 16) { wavm_arm_mte_stg(ptr); }
}

extern "C" void wavm_aarch64_mte_settag_zero(void* ptrvp, ::std::size_t len) noexcept
{
	char* ptr(reinterpret_cast<char*>(ptrvp));
	while(len >= 32)
	{
		wavm_arm_mte_stz2g(ptr);
		ptr += 32;
		len -= 32;
	}
	if(len >= 16) { wavm_arm_mte_stzg(ptr); }
}
#else
namespace {
	inline constexpr bool platform_support_arm_mte{};
}
#endif

GrowResult Runtime::growMemory(Memory* memory, Uptr numPagesToGrow, Uptr* outOldNumPages)
{
	Uptr oldNumPages;
	if(numPagesToGrow == 0) { oldNumPages = memory->numPages.load(std::memory_order_seq_cst); }
	else
	{
		// Check the memory page quota.
		if(memory->resourceQuota && !memory->resourceQuota->memoryPages.allocate(numPagesToGrow))
		{
			return GrowResult::outOfQuota;
		}

		Platform::RWMutex::ExclusiveLock resizingLock(memory->resizingMutex);
		oldNumPages = memory->numPages.load(std::memory_order_acquire);

		// If the number of pages to grow would cause the memory's size to exceed its maximum,
		// return GrowResult::outOfMaxSize.
		U64 maxMemoryPages;
		auto baseAddressTags = memory->baseAddressTags;
		if(memory->indexType == IR::IndexType::i32)
		{
			if(baseAddressTags)
			{
				constexpr U64 maxmemtag32pages{IR::maxMemory32Pages
											   >> ::WAVM::IR::memtag32constants::bits};
				maxMemoryPages = maxmemtag32pages;
			}
			else { maxMemoryPages = IR::maxMemory32Pages; }
		}
		else { maxMemoryPages = std::min(maxMemory64WASMPages, IR::maxMemory64Pages); }
		if(numPagesToGrow > memory->maxPages || oldNumPages > memory->maxPages - numPagesToGrow
		   || numPagesToGrow > maxMemoryPages || oldNumPages > maxMemoryPages - numPagesToGrow)
		{
			if(memory->resourceQuota) { memory->resourceQuota->memoryPages.free(numPagesToGrow); }
			return GrowResult::outOfMaxSize;
		}

		Platform::MemoryAccess flags{Platform::MemoryAccess::readWrite};
		if constexpr(platform_support_arm_mte)
		{
			if(::WAVM::LLVMJIT::is_memtagstatus_armmte(memory->memtagstatus))
			{
				flags = static_cast<Platform::MemoryAccess>(
					static_cast<U32>(Platform::MemoryAccess::readWrite)
					| static_cast<U32>(Platform::MemoryAccess::mte));
			}
		}

		auto wasmlog2 = getPlatformPagesPerWebAssemblyPageLog2();
		auto grownpages = numPagesToGrow << wasmlog2;
		// Try to commit the new pages, and return GrowResult::outOfMemory if the commit fails.
		if(!Platform::commitVirtualPages(
			   memory->baseAddress + oldNumPages * IR::numBytesPerPage, grownpages, flags))
		{
			if(memory->resourceQuota) { memory->resourceQuota->memoryPages.free(numPagesToGrow); }
			return GrowResult::outOfMemory;
		}
		if(baseAddressTags)
		{
			auto wasmlog2m4 = wasmlog2 - 4u;
			auto grownpagesTagged = numPagesToGrow << wasmlog2m4;
			if(!Platform::commitVirtualPages(
				   baseAddressTags + oldNumPages * IR::numBytesTaggedPerPage,
				   grownpagesTagged,
				   flags))
			{
				if(memory->resourceQuota)
				{
					memory->resourceQuota->memoryPages.free(numPagesToGrow);
				}
				return GrowResult::outOfMemory;
			}
		}
		else if constexpr(platform_support_arm_mte)
		{
			if(memory->memtagstatus == ::WAVM::LLVMJIT::memtagStatus::armmteirg)
			{
#if defined(__aarch64__) && ((!defined(_MSC_VER) || defined(__clang__)) || defined(__GNUC__))
				wavm_arm_mte_stg(wavm_arm_mte_irg(memory->baseAddress, 0x1));
#endif
				goto endtagnullbyte;
			}
		}
		auto randombuffer{memory->memtagRandomBuffer};
		if(randombuffer.Base)
		{
			if(randombuffer.Base != randombuffer.End)
			{
				auto ch{randombuffer.End[-1]};
				if(ch == 0) // ensure ch is never a 0
				{
					if(memory->indexType == IR::IndexType::i32)
					{
						ch = ::WAVM::IR::memtag32constants::nullptrtag;
					}
					else { ch = ::WAVM::IR::memtag64constants::nullptrtag; }
				}
				*baseAddressTags = ch;
			}
		}
	endtagnullbyte:;
		Platform::registerVirtualAllocation(grownpages);

		const Uptr newNumPages = oldNumPages + numPagesToGrow;
		memory->numPages.store(newNumPages, std::memory_order_release);
		if(memory->id != UINTPTR_MAX)
		{
			memory->compartment->runtimeData->memories[memory->id].numPages.store(
				newNumPages, std::memory_order_release);
		}
	}

	if(outOldNumPages) { *outOldNumPages = oldNumPages; }
	return GrowResult::success;
}

void Runtime::unmapMemoryPages(Memory* memory, Uptr pageIndex, Uptr numPages)
{
	WAVM_ASSERT(pageIndex + numPages > pageIndex);
	WAVM_ASSERT((pageIndex + numPages) * IR::numBytesPerPage <= memory->numReservedBytes);
	auto wasmlog2 = getPlatformPagesPerWebAssemblyPageLog2();
	auto dcm = numPages << wasmlog2;
	// Decommit the pages.
	Platform::decommitVirtualPages(memory->baseAddress + pageIndex * IR::numBytesPerPage, dcm);
	Platform::deregisterVirtualAllocation(dcm);

	if(memory->baseAddressTags)
	{
		auto wasmlog2m4 = wasmlog2 - 4u;
		auto dcmtagged = numPages << wasmlog2m4;
		Platform::decommitVirtualPages(
			memory->baseAddressTags + pageIndex * IR::numBytesTaggedPerPage, dcmtagged);
		Platform::deregisterVirtualAllocation(dcmtagged);
	}
}

U8* Runtime::getMemoryBaseAddress(Memory* memory) { return memory->baseAddress; }

static U8* getValidatedMemoryOffsetRangeImpl(Memory* memory,
											 U8* memoryBase,
											 Uptr memoryNumBytes,
											 Uptr address,
											 Uptr numBytes)
{
	auto baseaddrestags = memory->baseAddressTags;
	U8 color = 0;
	if(baseaddrestags)
	{
		if(memory->indexType == IR::IndexType::i32)
		{
			using constanttype = ::WAVM::IR::memtag32constants;
			color = static_cast<U8>(address >> (constanttype::shifter));
			address &= constanttype::mask;
		}
		else
		{
			using constanttype = ::WAVM::IR::memtag64constants;
			color = static_cast<U8>(address >> (constanttype::shifter));
			address &= constanttype::mask;
		}
	}
	auto boundsaddress{address};
	if constexpr(platform_support_arm_mte)
	{
		if(memory->memtagstatus == ::WAVM::LLVMJIT::memtagStatus::armmte
		   || memory->memtagstatus == ::WAVM::LLVMJIT::memtagStatus::armmteirg)
		{
			using constanttype = ::WAVM::IR::memtagarmmteconstants;
			if(memory->indexType == IR::IndexType::i32)
			{
				using constanttype = ::WAVM::IR::memtag32constants;
				boundsaddress = address & constanttype::mask;
				address
					= (static_cast<U64>(address >> (constanttype::shifter)) << 56u) | boundsaddress;
			}
			else
			{
				boundsaddress &= constanttype::mask; // let linux kernel to handle the arm mte
			}
		}
	}
	if(boundsaddress + numBytes > memoryNumBytes || boundsaddress + numBytes < boundsaddress)
	{
		throwException(ExceptionTypes::outOfBoundsMemoryAccess,
					   {asObject(memory),
						U64(boundsaddress > memoryNumBytes ? boundsaddress : memoryNumBytes)});
	}
	if(baseaddrestags && numBytes != 0)
	{
		Uptr tagaddress = address >> 4;
		Uptr tagaddresslast = (address + numBytes - 1) >> 4;
		if(baseaddrestags[tagaddress] != color || baseaddrestags[tagaddresslast] != color)
		{
			throwException(ExceptionTypes::invalidMemoryTagAccess, {});
		}
	}
	WAVM_ASSERT(memoryBase);
	numBytes = branchlessMin(numBytes, memoryNumBytes);
	Uptr diff{static_cast<Uptr>(memoryNumBytes - numBytes)};
	return memoryBase + (boundsaddress < diff ? address : diff);
}

U8* Runtime::getReservedMemoryOffsetRange(Memory* memory, Uptr address, Uptr numBytes)
{
	WAVM_ASSERT(memory);

	// Validate that the range [offset..offset+numBytes) is contained by the memory's reserved
	// pages.
	return ::getValidatedMemoryOffsetRangeImpl(
		memory, memory->baseAddress, memory->numReservedBytes, address, numBytes);
}

U8* Runtime::getValidatedMemoryOffsetRange(Memory* memory, Uptr address, Uptr numBytes)
{
	WAVM_ASSERT(memory);

	// Validate that the range [offset..offset+numBytes) is contained by the memory's committed
	// pages.
	return ::getValidatedMemoryOffsetRangeImpl(
		memory,
		memory->baseAddress,
		memory->numPages.load(std::memory_order_acquire) * IR::numBytesPerPage,
		address,
		numBytes);
}

void Runtime::initDataSegment(Instance* instance,
							  Uptr dataSegmentIndex,
							  const std::vector<U8>* dataVector,
							  Memory* memory,
							  Uptr destAddress,
							  Uptr sourceOffset,
							  Uptr numBytes)
{
	U8* destPointer = getValidatedMemoryOffsetRange(memory, destAddress, numBytes);
	if(sourceOffset + numBytes > dataVector->size() || sourceOffset + numBytes < sourceOffset)
	{
		throwException(
			ExceptionTypes::outOfBoundsDataSegmentAccess,
			{asObject(instance),
			 U64(dataSegmentIndex),
			 U64(sourceOffset > dataVector->size() ? sourceOffset : dataVector->size())});
	}
	else
	{
		Runtime::unwindSignalsAsExceptions([destPointer, sourceOffset, numBytes, dataVector] {
			bytewiseMemCopy(destPointer, dataVector->data() + sourceOffset, numBytes);
		});
	}
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wavmIntrinsicsMemory,
							   "memory.grow",
							   Iptr,
							   memory_grow,
							   Uptr deltaPages,
							   Uptr memoryId)
{
	Memory* memory = getMemoryFromRuntimeData(contextRuntimeData, memoryId);
	Uptr oldNumPages = 0;
	if(growMemory(memory, deltaPages, &oldNumPages) != GrowResult::success) { return -1; }
	WAVM_ASSERT(oldNumPages <= INTPTR_MAX);
	return Iptr(oldNumPages);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wavmIntrinsicsMemory,
							   "memory.init",
							   void,
							   memory_init,
							   Uptr destAddress,
							   Uptr sourceOffset,
							   Uptr numBytes,
							   Uptr instanceId,
							   Uptr memoryId,
							   Uptr dataSegmentIndex)
{
	Instance* instance = getInstanceFromRuntimeData(contextRuntimeData, instanceId);
	Memory* memory = getMemoryFromRuntimeData(contextRuntimeData, memoryId);

	Platform::RWMutex::ShareableLock dataSegmentsLock(instance->dataSegmentsMutex);
	if(!instance->dataSegments[dataSegmentIndex])
	{
		if(sourceOffset != 0 || numBytes != 0)
		{
			throwException(ExceptionTypes::outOfBoundsDataSegmentAccess,
						   {instance, dataSegmentIndex, sourceOffset});
		}
	}
	else
	{
		// Make a copy of the shared_ptr to the data and unlock the data segments mutex.
		std::shared_ptr<std::vector<U8>> dataVector = instance->dataSegments[dataSegmentIndex];
		dataSegmentsLock.unlock();

		initDataSegment(instance,
						dataSegmentIndex,
						dataVector.get(),
						memory,
						destAddress,
						sourceOffset,
						numBytes);
	}
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wavmIntrinsicsMemory,
							   "data.drop",
							   void,
							   data_drop,
							   Uptr instanceId,
							   Uptr dataSegmentIndex)
{
	Instance* instance = getInstanceFromRuntimeData(contextRuntimeData, instanceId);
	Platform::RWMutex::ExclusiveLock dataSegmentsLock(instance->dataSegmentsMutex);

	if(instance->dataSegments[dataSegmentIndex])
	{
		instance->dataSegments[dataSegmentIndex].reset();
	}
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wavmIntrinsics,
							   "memoryOutOfBoundsTrap",
							   void,
							   outOfBoundsMemoryTrap,
							   Uptr address,
							   Uptr numBytes,
							   Uptr memoryNumBytes,
							   Uptr memoryId)
{
	Compartment* compartment = getCompartmentFromContextRuntimeData(contextRuntimeData);
	Platform::RWMutex::ShareableLock compartmentLock(compartment->mutex);
	Memory* memory = compartment->memories[memoryId];
	compartmentLock.unlock();

	const U64 outOfBoundsAddress = U64(address) > memoryNumBytes ? U64(address) : memoryNumBytes;

	throwException(ExceptionTypes::outOfBoundsMemoryAccess, {memory, outOfBoundsAddress});
}
#if 1
WAVM_DEFINE_INTRINSIC_FUNCTION(wavmIntrinsics,
							   "wavmdebuggingprint",
							   void,
							   wavmdebuggingprint,
							   size_t addr)
{
	fprintf(stderr, "wavmdebuggingprint: %p\n", reinterpret_cast<void*>(addr));
}
#endif

extern "C" void wavm_memtag_trap_function()
{
	throwException(ExceptionTypes::invalidMemoryTagAccess, {});
}
