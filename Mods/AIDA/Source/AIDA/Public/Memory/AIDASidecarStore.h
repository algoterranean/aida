#pragma once

#include "CoreMinimal.h"
#include "Memory/AIDAMemoryTypes.h"

/**
 * Sidecar persistence backend (docs/PHASE3.md §1c): bulky, append-mostly history stored as JSONL under
 * `<ProjectDir>/Configs/AIDA/data/<session-guid>/` — the same writable root as config.jsonc, keyed to the
 * save's session GUID so a rolled-back save orphans (never corrupts) newer data. Host-side only; writes
 * are best-effort (a failure logs and degrades, it never blocks chat).
 *
 * The JSON (de)serialization and the ring-buffer trim are pure static helpers so they unit-test without a
 * filesystem; only Init/AppendSnapshot/LoadSnapshots touch disk.
 */
class FAIDASidecarStore
{
public:
	/** Point the store at a session's directory (creates it lazily on first write). */
	void Init(const FString& SessionId);

	bool IsReady() const { return !BaseDir.IsEmpty(); }

	/** Append a snapshot, keeping only the newest KeepMax lines. Returns false on I/O failure. */
	bool AppendSnapshot(const FAIDASnapshot& Snapshot, int32 KeepMax);

	/** Load all snapshots (oldest first). Empty if none / not ready. */
	TArray<FAIDASnapshot> LoadSnapshots() const;

	//~ Pure helpers (unit-tested).
	/** One snapshot as a compact JSON object (a single JSONL line). */
	static FString SnapshotToJson(const FAIDASnapshot& Snapshot);
	/** Parse one JSONL line back into a snapshot. Returns false on malformed input. */
	static bool SnapshotFromJson(const FString& Line, FAIDASnapshot& OutSnapshot);
	/** Append NewLine to Lines, then drop oldest lines so at most KeepMax remain (KeepMax<=0 → unbounded). */
	static void AppendWithRingBuffer(TArray<FString>& Lines, const FString& NewLine, int32 KeepMax);

private:
	FString SnapshotsPath() const;

	FString BaseDir; // <ProjectDir>/Configs/AIDA/data/<session-guid>/
};
