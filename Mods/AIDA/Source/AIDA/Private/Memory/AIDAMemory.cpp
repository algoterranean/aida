#include "Memory/AIDAMemory.h"

#include "AIDA.h"
#include "Memory/AIDAMemoryStore.h"

AAIDAMemoryStore* FAIDAMemory::Store(UObject* WorldContext) const
{
	if (!CachedStore.IsValid())
	{
		CachedStore = AAIDAMemoryStore::Get(WorldContext);
	}
	return CachedStore.Get();
}

void FAIDAMemory::Init(UObject* WorldContext)
{
	AAIDAMemoryStore* MemStore = Store(WorldContext);
	if (!MemStore)
	{
		// The store spawns on the server via the SubsystemActorManager; a client (or a not-yet-spawned
		// world) has none. Leave the sidecar uninitialized; Init can be called again once it exists.
		UE_LOG(LogAIDA, Verbose, TEXT("[memory] no in-save store yet; sidecar deferred."));
		return;
	}

	SessionId = MemStore->GetSessionId();
	Sidecar.Init(SessionId.ToString(EGuidFormats::DigitsWithHyphens));
	UE_LOG(LogAIDA, Log, TEXT("[memory] ready: session=%s"), *SessionId.ToString(EGuidFormats::DigitsWithHyphens));
}
