#include "UI/AIDAChatWidget.h"

#include "AIDA.h"
#include "Net/AIDAChatRelay.h"
#include "Subsystem/SubsystemActorManager.h"
#include "Components/Button.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Engine/Font.h"
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

	// This is an overlay shown on top of live gameplay AND menus (pause/escape). Only the input row
	// should capture the mouse; the transcript covers most of the viewport and would otherwise eat
	// clicks meant for whatever is underneath. Make the read-only transcript click-through so the menu
	// beneath it stays usable. InputBox + SendButton keep their default (hit-testable) visibility.
	// (The root canvas panel is already SelfHitTestInvisible, so empty space passes clicks through.)
	if (TranscriptScroll)
	{
		TranscriptScroll->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	if (TranscriptText)
	{
		TranscriptText->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);

	// --- Layout + font tuning (driven here so the BP stays a pure, positionless view) ---
	// Tweakables (slate units ~= design px). Bump ToolbarClearance if the input row overlaps the hotbar.
	constexpr float Margin          = 20.f;  // outer margin from screen edges
	constexpr float InputHeight     = 44.f;  // height of the input box / Send button row
	constexpr float SendWidth       = 140.f; // width of the Send button
	constexpr float RowGap          = 12.f;  // gap between transcript, input box, and Send button
	constexpr float ToolbarClearance= 96.f;  // approx Satisfactory hotbar height to clear at the bottom
	constexpr float FontSize        = 12.f;  // just above the console font; default UMG is 24
	// Input row's bottom edge sits this far above the screen bottom: toolbar height + a 20px margin.
	const float BottomInset = ToolbarClearance + Margin;

	// Transcript: stretch to fill from top+margin down to just above the input row.
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(TranscriptScroll ? TranscriptScroll->Slot : nullptr))
	{
		CanvasSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f)); // stretch both axes
		CanvasSlot->SetOffsets(FMargin(Margin, Margin, Margin, BottomInset + InputHeight + RowGap));
	}
	// Input box: bottom row, stretched horizontally but leaving room for the Send button on the right.
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(InputBox ? InputBox->Slot : nullptr))
	{
		CanvasSlot->SetAnchors(FAnchors(0.f, 1.f, 1.f, 1.f)); // stretch X, pin to bottom edge
		CanvasSlot->SetAlignment(FVector2D(0.f, 1.f));
		CanvasSlot->SetOffsets(FMargin(Margin, -BottomInset, SendWidth + RowGap + Margin, InputHeight));
	}
	// Send button: bottom-right corner, fixed width, aligned with the input row.
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(SendButton ? SendButton->Slot : nullptr))
	{
		CanvasSlot->SetAnchors(FAnchors(1.f, 1.f, 1.f, 1.f)); // pin to bottom-right corner
		CanvasSlot->SetAlignment(FVector2D(1.f, 1.f));
		CanvasSlot->SetOffsets(FMargin(-Margin, -BottomInset, SendWidth, InputHeight));
	}

	// Fonts: use the game's UI font (Open Sans — /Game/FactoryGame/Interface/Font/DescriptionText) so the
	// window matches the native chat; sized just above console size, with a dark outline so it reads over
	// the game the way the native chat does. Falls back to the BP's default font if the asset can't load.
	UFont* GameFont = LoadObject<UFont>(nullptr, TEXT("/Game/FactoryGame/Interface/Font/DescriptionText.DescriptionText"));
	if (TranscriptText)
	{
		FSlateFontInfo Font = GameFont ? FSlateFontInfo(GameFont, FontSize) : TranscriptText->GetFont();
		Font.Size = FontSize;
		Font.OutlineSettings.OutlineSize = 1;
		Font.OutlineSettings.OutlineColor = FLinearColor(0.f, 0.f, 0.f, 1.f);
		TranscriptText->SetFont(Font);

		if (InputBox)
		{
			FSlateFontInfo InputFont = Font; // reuse the transcript's (known-valid) font family
			InputBox->WidgetStyle.TextStyle.Font = InputFont;
			InputBox->SynchronizeProperties();
		}
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

void UAIDAChatWidget::FocusInput()
{
	if (InputBox)
	{
		InputBox->SetKeyboardFocus();
	}
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
