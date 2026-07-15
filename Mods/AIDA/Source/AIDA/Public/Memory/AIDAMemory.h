#pragma once

#include "CoreMinimal.h"
#include "Memory/AIDAMemoryTypes.h"
#include "Memory/AIDASidecarStore.h"

class AAIDAMemoryStore;
class UObject;

/**
 * The one memory facade the orchestrator + services talk to (docs/PHASE3.md §1d). Routes by data type:
 * notes / journal / markers → the in-save store (AAIDAMemoryStore); snapshots / transcripts → the sidecar
 * (FAIDASidecarStore). Keeps callers backend-agnostic. Server-side only.
 *
 * The in-save store is a world actor that spawns via the SubsystemActorManager, so it is resolved lazily
 * (and re-resolved if the world changes); Init binds the sidecar to the save's session GUID.
 */
class FAIDAMemory
{
public:
	/** Resolve the in-save store, mint/read the session id, and point the sidecar at its directory. */
	void Init(UObject* WorldContext);

	bool IsReady() const { return Sidecar.IsReady(); }

	/** The save's session GUID (empty if not yet initialized). */
	const FGuid& GetSessionId() const { return SessionId; }

	/** The in-save store for this world (server-side), or null if it hasn't spawned. */
	AAIDAMemoryStore* Store(UObject* WorldContext) const;

	//~ Sidecar-backed (history).
	bool AppendSnapshot(const FAIDASnapshot& Snapshot, int32 KeepMax) { return Sidecar.AppendSnapshot(Snapshot, KeepMax); }
	TArray<FAIDASnapshot> LoadSnapshots() const { return Sidecar.LoadSnapshots(); }

private:
	mutable TWeakObjectPtr<AAIDAMemoryStore> CachedStore;
	FAIDASidecarStore Sidecar;
	FGuid SessionId;
};
