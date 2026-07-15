#include "Memory/AIDAMemoryStore.h"

#include "AIDA.h"
#include "Subsystem/SubsystemActorManager.h"

#include "Engine/Engine.h"
#include "Engine/World.h"

AAIDAMemoryStore::AAIDAMemoryStore()
{
	// Server-authoritative and not replicated — tools read/write it server-side. It persists via the
	// save system (IFGSaveInterface), so it is not a transient/replicated actor like the chat relay.
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer;
}

AAIDAMemoryStore* AAIDAMemoryStore::Get(UObject* WorldContext)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World) { return nullptr; }
	if (USubsystemActorManager* Mgr = World->GetSubsystem<USubsystemActorManager>())
	{
		return Mgr->GetSubsystemActor<AAIDAMemoryStore>();
	}
	return nullptr;
}

void AAIDAMemoryStore::EnsureSessionId()
{
	if (!SessionId.IsValid())
	{
		SessionId = FGuid::NewGuid();
		UE_LOG(LogAIDA, Log, TEXT("[memory] minted session id %s"), *SessionId.ToString(EGuidFormats::DigitsWithHyphens));
	}
}

const FGuid& AAIDAMemoryStore::GetSessionId()
{
	EnsureSessionId();
	return SessionId;
}

FGuid AAIDAMemoryStore::AddNote(FAIDANote Note)
{
	if (!Note.Id.IsValid()) { Note.Id = FGuid::NewGuid(); }
	const FGuid Id = Note.Id;
	Notes.Add(MoveTemp(Note));
	UE_LOG(LogAIDA, Log, TEXT("[memory] note added (%s); %d total."), *Id.ToString(EGuidFormats::DigitsWithHyphens), Notes.Num());
	return Id;
}

void AAIDAMemoryStore::PostLoadGame_Implementation(int32 /*saveVersion*/, int32 /*gameVersion*/)
{
	// A save that predates AIDA (or a brand-new one) has no session id yet — mint one so the sidecar has
	// a stable key from here on.
	EnsureSessionId();
	UE_LOG(LogAIDA, Log, TEXT("[memory] loaded: session=%s notes=%d markers=%d journal=%d"),
		*SessionId.ToString(EGuidFormats::DigitsWithHyphens), Notes.Num(), Markers.Num(), Journal.Num());
}
