#include <string.h>
#include <atomic>
#include <vector>
#include "RuntimePrivate.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Platform/Memory.h"
#include "WAVM/Platform/RWMutex.h"
#include "WAVM/Platform/Random.h"
#include "WAVM/Runtime/Runtime.h"
#include "WAVM/RuntimeABI/RuntimeABI.h"

using namespace WAVM;
using namespace WAVM::Runtime;

Context* Runtime::createContext(Compartment* compartment, std::string&& debugName)
{
	WAVM_ASSERT(compartment);
	Context* context = new Context(compartment, std::move(debugName));
	{
		Platform::RWMutex::ExclusiveLock lock(compartment->mutex);

		// Allocate an ID for the context in the compartment.
		context->id = compartment->contexts.add(UINTPTR_MAX, context);
		if(context->id == UINTPTR_MAX)
		{
			delete context;
			return nullptr;
		}
		context->runtimeData = &compartment->runtimeData->contexts[context->id];

		// Commit the page(s) for the context's runtime data.
		if(!Platform::commitVirtualPages(
			   (U8*)context->runtimeData,
			   sizeof(ContextRuntimeData) >> Platform::getBytesPerPageLog2()))
		{
			delete context;
			return nullptr;
		}
		Platform::registerVirtualAllocation(sizeof(ContextRuntimeData));

		// Initialize the context's global data.
		memcpy(context->runtimeData->mutableGlobals,
			   compartment->initialContextMutableGlobals,
			   maxMutableGlobals * sizeof(IR::UntaggedValue));

		context->runtimeData->context = context;

		// Point this thread's cursor at a random position in the compartment's shared tag-byte
		// buffer. The buffer is shared, but each thread loops over it independently, so the cursor
		// lives here (per-thread) rather than in MemoryRuntimeData (per-compartment); starting each
		// thread at a different position keeps two threads from emitting the same tag sequence in
		// lockstep. The buffer outlives every context (it is owned by the compartment), so the
		// bounds stored here never go stale.
		MemtagRandomBufferInMemory& randomBuffer = compartment->memtagRandomBuffer;
		if(randomBuffer.Base && randomBuffer.Base != randomBuffer.End)
		{
			const Uptr numBytes = Uptr(randomBuffer.End - randomBuffer.Base);
			Uptr offset = 0;
			Platform::getCryptographicRNG(reinterpret_cast<U8*>(&offset), sizeof(offset));
			context->runtimeData->memtagRandomBuffer
				= {randomBuffer.Base, randomBuffer.Base + (offset % numBytes), randomBuffer.End};
		}
		else
		{
			context->runtimeData->memtagRandomBuffer = {};
		}
	}

	return context;
}

Runtime::Context::~Context()
{
	WAVM_ASSERT_RWMUTEX_IS_EXCLUSIVELY_LOCKED_BY_CURRENT_THREAD(compartment->mutex);
	if(id != UINTPTR_MAX) { compartment->contexts.removeOrFail(id); }

	Platform::decommitVirtualPages((U8*)runtimeData,
								   sizeof(ContextRuntimeData) >> Platform::getBytesPerPageLog2());
	Platform::deregisterVirtualAllocation(sizeof(ContextRuntimeData));
}

Compartment* Runtime::getCompartment(const Context* context) { return context->compartment; }

Context* Runtime::cloneContext(const Context* context, Compartment* newCompartment)
{
	// Create a new context and initialize its runtime data with the values from the source context.
	Context* clonedContext = createContext(newCompartment);
	if(clonedContext)
	{
		memcpy(clonedContext->runtimeData->mutableGlobals,
			   context->runtimeData->mutableGlobals,
			   maxMutableGlobals * sizeof(IR::UntaggedValue));
	}
	return clonedContext;
}
