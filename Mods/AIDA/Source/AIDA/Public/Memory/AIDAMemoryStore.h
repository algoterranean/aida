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

	/** Add a note (assigns a fresh Id, stamps nothing else — caller fills the rest). Returns the new Id. */
	FGuid AddNote(FAIDANote Note);

	/** Read-only access to the stored notes (server-side). */
	const TArray<FAIDANote>& GetNotes() const { return Notes; }

	/** Append an executed action to the undo journal (mints an Id if unset). Returns the entry Id. */
	FGuid AppendJournal(FAIDAJournalEntry Entry);

	/** Read-only access to the journal, oldest-first (server-side). */
	const TArray<FAIDAJournalEntry>& GetJournal() const { return Journal; }

	/** Flag a journal entry as undone (so /aida undo skips past it). False if the Id is unknown. */
	bool MarkUndone(const FGuid& Id);

	//~ P8 Slice 5 standing tasks (human-created recurring Query-only checks).
	/** Add a standing task (mints an Id if unset). Returns the task Id. */
	FGuid AddTask(FAIDAStandingTask Task);
	const TArray<FAIDAStandingTask>& GetTasks() const { return Tasks; }
	/** Mutable lookup for the scheduler (LastRunUtc/LastResult) and pause/resume. Null = unknown. */
	FAIDAStandingTask* FindTask(const FGuid& Id);
	bool RemoveTask(const FGuid& Id);

	//~ IFGSaveInterface — persist SaveGame properties; mint the session id on load.
	virtual bool ShouldSave_Implementation() const override { return true; }
	virtual void PreSaveGame_Implementation(int32 saveVersion, int32 gameVersion) override {}
	virtual void PostSaveGame_Implementation(int32 saveVersion, int32 gameVersion) override {}
	virtual void PreLoadGame_Implementation(int32 saveVersion, int32 gameVersion) override {}
	virtual void PostLoadGame_Implementation(int32 saveVersion, int32 gameVersion) override;
	virtual void GatherDependencies_Implementation(TArray<UObject*>& out_dependentObjects) override {}
	virtual bool NeedTransform_Implementation() override { return false; }

	//~ Persisted state (SaveGame). Marker accessors land with P4's marker cleanup.
	UPROPERTY(SaveGame) FGuid SessionId;
	UPROPERTY(SaveGame) TArray<FAIDANote> Notes;
	UPROPERTY(SaveGame) TArray<FAIDAJournalEntry> Journal;
	UPROPERTY(SaveGame) TArray<FAIDAMarkerRecord> Markers;
	UPROPERTY(SaveGame) TArray<FAIDAStandingTask> Tasks;

private:
	void EnsureSessionId();
};
