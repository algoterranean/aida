#include "UI/AIDAChatWidget.h"

#include "AIDA.h"
#include "Net/AIDAChatRelay.h"
#include "Subsystem/SubsystemActorManager.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Engine/World.h"
#include "TimerManager.h"

void UAIDAChatWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Self-wire the optional sub-widgets so the Blueprint can stay a pure layout (no event graph).
	if (SendButton && !SendButton->OnClicked.IsAlreadyBound(this, &UAIDAChatWidget::HandleSendClicked))
	{
		SendButton->OnClicked.AddDynamic(this, &UAIDAChatWidget::HandleSendClicked);
	}
	if (InputBox && !InputBox->OnTextCommitted.IsAlreadyBound(this, &UAIDAChatWidget::HandleInputCommitted))
	{
		InputBox->OnTextCommitted.AddDynamic(this, &UAIDAChatWidget::HandleInputCommitted);
	}
	if (TranscriptText)
	{
		TranscriptText->SetAutoWrapText(true);
	}

	// The relay is a replicated actor and may not have arrived on this client yet — retry until it has.
	if (!TryBindRelay())
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(
				BindRetryTimer,
				FTimerDelegate::CreateWeakLambda(this, [this]() { if (TryBindRelay()) { GetWorld()->GetTimerManager().ClearTimer(BindRetryTimer); } }),
				0.5f, /*bLoop=*/true);
		}
	}
}

void UAIDAChatWidget::NativeDestruct()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(BindRetryTimer);
	}
	UnbindRelay();
	Super::NativeDestruct();
}

bool UAIDAChatWidget::TryBindRelay()
{
	if (Relay.IsValid())
	{
		return true;
	}
	UWorld* World = GetWorld();
	USubsystemActorManager* Mgr = World ? World->GetSubsystem<USubsystemActorManager>() : nullptr;
	AAIDAChatRelay* Found = Mgr ? Mgr->GetSubsystemActor<AAIDAChatRelay>() : nullptr;
	if (!Found)
	{
		return false;
	}

	Relay = Found;
	Found->OnMsgBegin.AddDynamic(this, &UAIDAChatWidget::HandleMsgBegin);
	Found->OnMsgChunk.AddDynamic(this, &UAIDAChatWidget::HandleMsgChunk);
	Found->OnMsgEnd.AddDynamic(this, &UAIDAChatWidget::HandleMsgEnd);

	// Replay whatever transcript this client already assembled, then pull anything it missed (late join).
	// Route through the Handle* path so the native default view and any BP view both get populated.
	TArray<FAIDATranscriptEntry> Existing;
	Found->GetTranscript(Existing);
	for (const FAIDATranscriptEntry& Entry : Existing)
	{
		HandleMsgBegin(Entry.Header);
		HandleMsgChunk(Entry.Header.Id, Entry.Body);
		HandleMsgEnd(Entry.Header.Id);
	}
	Found->RequestRecentTranscript();

	UE_LOG(LogAIDA, Verbose, TEXT("[widget] bound to chat relay."));
	return true;
}

void UAIDAChatWidget::UnbindRelay()
{
	if (AAIDAChatRelay* R = Relay.Get())
	{
		R->OnMsgBegin.RemoveDynamic(this, &UAIDAChatWidget::HandleMsgBegin);
		R->OnMsgChunk.RemoveDynamic(this, &UAIDAChatWidget::HandleMsgChunk);
		R->OnMsgEnd.RemoveDynamic(this, &UAIDAChatWidget::HandleMsgEnd);
	}
	Relay.Reset();
}

void UAIDAChatWidget::SendChat(const FString& Text)
{
	if (AAIDAChatRelay* R = Relay.Get())
	{
		R->SubmitChat(Text);
	}
}

void UAIDAChatWidget::GetTranscript(TArray<FAIDATranscriptEntry>& OutEntries) const
{
	if (const AAIDAChatRelay* R = Relay.Get())
	{
		R->GetTranscript(OutEntries);
	}
	else
	{
		OutEntries.Reset();
	}
}

void UAIDAChatWidget::HandleMsgBegin(const FAIDAMessageHeader& Header)
{
	// Native default view: open (or upsert-RESET) the message block for this Id.
	if (int32* Existing = RenderedIndexById.Find(Header.Id))
	{
		RenderedMessages[*Existing].Body.Reset();
	}
	else
	{
		FRenderedMessage Msg;
		Msg.Id = Header.Id;
		Msg.Prefix = FString::Printf(TEXT("%s: "), *Header.Author);
		RenderedIndexById.Add(Header.Id, RenderedMessages.Add(MoveTemp(Msg)));
	}
	RebuildRenderedTranscript();

	OnMessageBegin(Header);
}

void UAIDAChatWidget::HandleMsgChunk(const FGuid& Id, const FString& Delta)
{
	if (int32* Idx = RenderedIndexById.Find(Id))
	{
		RenderedMessages[*Idx].Body += Delta;
		RebuildRenderedTranscript();
	}

	OnMessageDelta(Id, Delta);
}

void UAIDAChatWidget::HandleMsgEnd(const FGuid& Id)
{
	OnMessageEnd(Id);
}

void UAIDAChatWidget::HandleSendClicked()
{
	if (!InputBox)
	{
		return;
	}
	const FString Text = InputBox->GetText().ToString();
	if (Text.IsEmpty())
	{
		return;
	}
	SendChat(Text);
	InputBox->SetText(FText::GetEmpty());
}

void UAIDAChatWidget::HandleInputCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	if (CommitMethod == ETextCommit::OnEnter)
	{
		HandleSendClicked();
	}
}

void UAIDAChatWidget::RebuildRenderedTranscript()
{
	FString Out;
	for (const FRenderedMessage& Msg : RenderedMessages)
	{
		if (!Out.IsEmpty())
		{
			Out += TEXT("\n\n");
		}
		Out += Msg.Prefix;
		Out += Msg.Body;
	}
	RenderedTranscript = MoveTemp(Out);

	if (TranscriptText)
	{
		TranscriptText->SetText(FText::FromString(RenderedTranscript));
	}
	if (TranscriptScroll)
	{
		TranscriptScroll->ScrollToEnd();
	}
	OnTranscriptChanged.Broadcast(RenderedTranscript);
}
