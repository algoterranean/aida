#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "FGSaveInterface.h"
#include "Memory/AIDAMemoryTypes.h"
#include "AIDAMemoryStore.generated.h"

/**
 * In-save persistence backend (docs/PHASE3.md §1b). A server-authoritative SML subsystem actor that
 * implements IFGSaveInterface so its SaveGame properties — the session GUID, player notes, the action
 * journal (Phase 4), and the tag_node marker registry — travel inside the game save and survive server
 * migration. AFGSubsystem does NOT implement the save interface, so we do it here explicitly.
 *
 * SpawnOnServer (not replicated): the orchestrator's tools read/write it server-side; clients never touch
 * it directly. Registered via the SubsystemActorManager like AAIDAChatRelay.
 */
UCLASS()
class AAIDAMemoryStore : public AModSubsystem, public IFGSaveInterface
{
	GENERATED_BODY()

public:
	AAIDAMemoryStore();

	/** Fetch the world's memory store (server-side). Null on clients or before it spawns. */
	static AAIDAMemoryStore* Get(UObject* WorldContext);

	/** The save's stable session id, minted on first access if unset. Keys the sidecar directory. */
	const FGuid& GetSessionId();

	//~ IFGSaveInterface — persist SaveGame properties; mint the session id on load.
	virtual bool ShouldSave_Implementation() const override { return true; }
	virtual void PreSaveGame_Implementation(int32 saveVersion, int32 gameVersion) override {}
	virtual void PostSaveGame_Implementation(int32 saveVersion, int32 gameVersion) override {}
	virtual void PreLoadGame_Implementation(int32 saveVersion, int32 gameVersion) override {}
	virtual void PostLoadGame_Implementation(int32 saveVersion, int32 gameVersion) override;
	virtual void GatherDependencies_Implementation(TArray<UObject*>& out_dependentObjects) override {}
	virtual bool NeedTransform_Implementation() override { return false; }

	//~ Persisted state (SaveGame). Note/marker/journal accessors land in later slices.
	UPROPERTY(SaveGame) FGuid SessionId;
	UPROPERTY(SaveGame) TArray<FAIDANote> Notes;
	UPROPERTY(SaveGame) TArray<FAIDAJournalEntry> Journal;
	UPROPERTY(SaveGame) TArray<FAIDAMarkerRecord> Markers;

private:
	void EnsureSessionId();
};
