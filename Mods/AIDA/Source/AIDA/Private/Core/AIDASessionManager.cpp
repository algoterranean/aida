#include "Core/AIDASessionManager.h"

#include "Net/AIDAChatRelay.h"

FAIDASessionManager::FAIDASessionManager(int32 InMaxMessages)
	: MaxMessages(FMath::Max(1, InMaxMessages))
{
}

void FAIDASessionManager::SetRelay(AAIDAChatRelay* InRelay)
{
	Relay = InRelay;
}

uint32 FAIDASessionManager::HashBody(const FString& Body)
{
	// Must match the client's verification in AAIDAChatRelay::Multicast_MsgEnd_Implementation.
	return FCrc::StrCrc32(*Body);
}

void FAIDASessionManager::Store(const FAIDAMessageHeader& Header, const FString& Body)
{
	FAIDATranscriptEntry Entry;
	Entry.Header = Header;
	Entry.Body = Body;
	const int32 Index = Transcript.Add(MoveTemp(Entry));
	IndexById.Add(Header.Id, Index);
	Prune();
}

void FAIDASessionManager::Prune()
{
	const int32 Overflow = Transcript.Num() - MaxMessages;
	if (Overflow <= 0)
	{
		return;
	}
	Transcript.RemoveAt(0, Overflow);
	IndexById.Reset();
	for (int32 i = 0; i < Transcript.Num(); ++i)
	{
		IndexById.Add(Transcript[i].Header.Id, i);
	}
}

FAIDATranscriptEntry* FAIDASessionManager::Find(const FGuid& Id)
{
	if (const int32* Index = IndexById.Find(Id))
	{
		return &Transcript[*Index];
	}
	return nullptr;
}

FGuid FAIDASessionManager::PostPlayerMessage(const FString& Author, const FString& Text)
{
	FAIDAMessageHeader Header;
	Header.Id = FGuid::NewGuid();
	Header.Author = Author;
	Header.Kind = EAIDAMsgKind::Player;

	Store(Header, Text);

	if (AAIDAChatRelay* R = Relay.Get())
	{
		R->ServerBeginMessage(Header);
		R->ServerPushDelta(Header.Id, Text);
		R->ServerEndMessage(Header.Id, HashBody(Text));
	}
	return Header.Id;
}

FGuid FAIDASessionManager::PostSystemMessage(const FString& Text)
{
	FAIDAMessageHeader Header;
	Header.Id = FGuid::NewGuid();
	Header.Author = TEXT("AIDA");
	Header.Kind = EAIDAMsgKind::System;

	Store(Header, Text);

	if (AAIDAChatRelay* R = Relay.Get())
	{
		R->ServerBeginMessage(Header);
		R->ServerPushDelta(Header.Id, Text);
		R->ServerEndMessage(Header.Id, HashBody(Text));
	}
	return Header.Id;
}

FGuid FAIDASessionManager::BeginAIDAMessage(const FString& Author)
{
	FAIDAMessageHeader Header;
	Header.Id = FGuid::NewGuid();
	Header.Author = Author;
	Header.Kind = EAIDAMsgKind::AIDA;

	Store(Header, FString());

	if (AAIDAChatRelay* R = Relay.Get())
	{
		R->ServerBeginMessage(Header);
	}
	return Header.Id;
}

void FAIDASessionManager::AppendDelta(const FGuid& Id, const FString& Delta)
{
	if (FAIDATranscriptEntry* Entry = Find(Id))
	{
		Entry->Body.Append(Delta);
	}
	if (AAIDAChatRelay* R = Relay.Get())
	{
		R->ServerPushDelta(Id, Delta);
	}
}

void FAIDASessionManager::CompleteMessage(const FGuid& Id)
{
	uint32 Hash = 0;
	if (const FAIDATranscriptEntry* Entry = Find(Id))
	{
		Hash = HashBody(Entry->Body);
	}
	if (AAIDAChatRelay* R = Relay.Get())
	{
		R->ServerEndMessage(Id, Hash);
	}
}

bool FAIDASessionManager::GetMessageBody(const FGuid& Id, FAIDATranscriptEntry& OutEntry) const
{
	if (const int32* Index = IndexById.Find(Id))
	{
		OutEntry = Transcript[*Index];
		return true;
	}
	return false;
}

void FAIDASessionManager::GetRecentTranscript(TArray<FAIDATranscriptEntry>& OutEntries) const
{
	OutEntries = Transcript;
}
