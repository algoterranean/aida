#include "UI/AIDAChatWidget.h"

#include "AIDA.h"
#include "Net/AIDAChatRelay.h"
#include "Net/AIDAProposalRelay.h"
#include "Subsystem/SubsystemActorManager.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelWidget.h"
#include "Components/RichTextBlock.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/DataTable.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "Styling/CoreStyle.h"
#include "TimerManager.h"
#include "UI/AIDAMarkdown.h"

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
	constexpr float FontSize        = 10.f;  // small, like the native chat
	constexpr float TabBarHeight    = 26.f;  // tab strip along the top
	constexpr float RightAnchor     = 0.5f;  // the window occupies the left half of the screen
	// Input row's bottom edge sits this far above the screen bottom: toolbar height + a 20px margin.
	const float BottomInset = ToolbarClearance + Margin;
	const float TranscriptTop = Margin + TabBarHeight + RowGap; // leave room for the tab bar

	// Transcript: fill the left half, from below the tab bar down to just above the input row.
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(TranscriptScroll ? TranscriptScroll->Slot : nullptr))
	{
		CanvasSlot->SetAnchors(FAnchors(0.f, 0.f, RightAnchor, 1.f));
		CanvasSlot->SetOffsets(FMargin(Margin, TranscriptTop, Margin, BottomInset + InputHeight + RowGap));
	}
	// Input box: bottom row of the left half, leaving room for the Send button on the right.
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(InputBox ? InputBox->Slot : nullptr))
	{
		CanvasSlot->SetAnchors(FAnchors(0.f, 1.f, RightAnchor, 1.f));
		CanvasSlot->SetAlignment(FVector2D(0.f, 1.f));
		CanvasSlot->SetOffsets(FMargin(Margin, -BottomInset, SendWidth + RowGap + Margin, InputHeight));
	}
	// Send button: pinned to the right edge of the left half, aligned with the input row.
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(SendButton ? SendButton->Slot : nullptr))
	{
		CanvasSlot->SetAnchors(FAnchors(RightAnchor, 1.f, RightAnchor, 1.f));
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
		Font.OutlineSettings.OutlineSize = 0; // strip the asset's default dark outline
		Font.OutlineSettings.OutlineColor = FLinearColor::Transparent;
		TranscriptText->SetFont(Font);

		if (InputBox)
		{
			FSlateFontInfo InputFont = Font; // reuse the transcript's (known-valid) font family
			InputBox->WidgetStyle.TextStyle.Font = InputFont;
			// The input box keeps its default light background, so the text must be dark — the
			// inherited white-on-white was unreadable. Both states: idle and focused-while-typing.
			InputBox->WidgetStyle.SetForegroundColor(FSlateColor(FLinearColor::Black));
			InputBox->WidgetStyle.SetFocusedForegroundColor(FSlateColor(FLinearColor::Black));
			InputBox->SynchronizeProperties();
		}
	}

	// Rich-text transcript for markdown rendering, constructed in C++ under the scroll box so the BP
	// needs no RichTextBlock. Falls back to the plain TranscriptText if the scroll box is missing.
	BuildTranscriptRich(GameFont, FontSize);

	if (UPanelWidget* RootPanel = Cast<UPanelWidget>(GetRootWidget()))
	{
		// Semi-transparent dark backdrop behind the chat (left half) so text reads over the game, like
		// the native chat. Added first + given a low Z so it sits behind the tabs/transcript/input.
		UBorder* Backdrop = WidgetTree->ConstructWidget<UBorder>();
		Backdrop->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.4f));
		Backdrop->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (UCanvasPanelSlot* BgSlot = Cast<UCanvasPanelSlot>(RootPanel->AddChild(Backdrop)))
		{
			BgSlot->SetAnchors(FAnchors(0.f, 0.f, RightAnchor, 1.f));
			BgSlot->SetOffsets(FMargin(Margin - 8.f, Margin - 8.f, Margin - 8.f, ToolbarClearance - 8.f));
			BgSlot->SetZOrder(-10);
		}

		// Tab bar along the top of the left half.
		TabBar = WidgetTree->ConstructWidget<UHorizontalBox>();
		if (UCanvasPanelSlot* TabSlot = Cast<UCanvasPanelSlot>(RootPanel->AddChild(TabBar)))
		{
			TabSlot->SetAnchors(FAnchors(0.f, 0.f, RightAnchor, 0.f));
			TabSlot->SetOffsets(FMargin(Margin, Margin, Margin, TabBarHeight));
		}
	}

	// Proposal card (Phase 4): top of the unused right half, collapsed until a proposal is pending.
	BuildProposalPanel(GameFont, FontSize);

	// Create the default conversation tab and render it (more tabs appear as conversations arrive).
	EnsureConversation(CurrentConversationId);
	RenderActiveConversation();

	// The relays are replicated actors and may not have arrived on this client yet — retry until both have.
	const bool bChatBound = TryBindRelay();
	const bool bProposalsBound = TryBindProposalRelay();
	if (!bChatBound || !bProposalsBound)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(
				BindRetryTimer,
				FTimerDelegate::CreateWeakLambda(this, [this]()
				{
					const bool bChat = TryBindRelay();
					const bool bProposals = TryBindProposalRelay();
					if (bChat && bProposals) { GetWorld()->GetTimerManager().ClearTimer(BindRetryTimer); }
				}),
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

	if (AAIDAProposalRelay* PR = ProposalRelay.Get())
	{
		PR->OnProposalsChanged.RemoveDynamic(this, &UAIDAChatWidget::HandleProposalsChanged);
	}
	ProposalRelay.Reset();
}

bool UAIDAChatWidget::TryBindProposalRelay()
{
	if (ProposalRelay.IsValid())
	{
		return true;
	}
	UWorld* World = GetWorld();
	USubsystemActorManager* Mgr = World ? World->GetSubsystem<USubsystemActorManager>() : nullptr;
	AAIDAProposalRelay* Found = Mgr ? Mgr->GetSubsystemActor<AAIDAProposalRelay>() : nullptr;
	if (!Found)
	{
		return false;
	}

	ProposalRelay = Found;
	Found->OnProposalsChanged.AddDynamic(this, &UAIDAChatWidget::HandleProposalsChanged);
	HandleProposalsChanged(); // render whatever replicated before we bound (late join)
	UE_LOG(LogAIDA, Verbose, TEXT("[widget] bound to proposal relay."));
	return true;
}

void UAIDAChatWidget::BuildProposalPanel(UFont* GameFont, float FontSize)
{
	UPanelWidget* RootPanel = Cast<UPanelWidget>(GetRootWidget());
	if (!RootPanel || !WidgetTree)
	{
		return;
	}

	// Dark card anchored to the top of the right half (the chat window occupies the left half).
	ProposalPanel = WidgetTree->ConstructWidget<UBorder>();
	ProposalPanel->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.55f));
	ProposalPanel->SetPadding(FMargin(12.f));
	ProposalPanel->SetVisibility(ESlateVisibility::Collapsed);
	if (UCanvasPanelSlot* CardSlot = Cast<UCanvasPanelSlot>(RootPanel->AddChild(ProposalPanel)))
	{
		// X stretches across the right half (margins 0); Y is a point anchor with a fixed card height.
		CardSlot->SetAnchors(FAnchors(0.55f, 0.f, 0.98f, 0.f));
		CardSlot->SetOffsets(FMargin(0.f, 20.f, 0.f, 170.f));
	}

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();
	ProposalPanel->AddChild(Column);

	const auto MakeText = [this, GameFont](const FString& Text, float Size, const FLinearColor& Color) -> UTextBlock*
	{
		UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>();
		Block->SetText(FText::FromString(Text));
		FSlateFontInfo Font = GameFont ? FSlateFontInfo(GameFont, static_cast<int32>(Size)) : Block->GetFont();
		Font.Size = Size;
		Font.OutlineSettings.OutlineSize = 0;
		Font.OutlineSettings.OutlineColor = FLinearColor::Transparent;
		Block->SetFont(Font);
		Block->SetColorAndOpacity(FSlateColor(Color));
		Block->SetAutoWrapText(true);
		return Block;
	};

	// Title, body, then the Approve / Reject row.
	Column->AddChildToVerticalBox(MakeText(TEXT("AIDA proposal"), FontSize + 2.f, FLinearColor(1.0f, 0.85f, 0.4f, 1.0f)));

	ProposalText = MakeText(FString(), FontSize, FLinearColor::White);
	if (UVerticalBoxSlot* BodySlot = Column->AddChildToVerticalBox(ProposalText))
	{
		BodySlot->SetPadding(FMargin(0.f, 6.f, 0.f, 8.f));
	}

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	Column->AddChildToVerticalBox(Row);

	ApproveButton = WidgetTree->ConstructWidget<UButton>();
	ApproveButton->OnClicked.AddDynamic(this, &UAIDAChatWidget::HandleApproveClicked);
	ApproveButton->SetBackgroundColor(FLinearColor(0.25f, 0.6f, 0.25f, 1.0f));
	ApproveButton->AddChild(MakeText(TEXT("Approve"), FontSize, FLinearColor::White));
	if (UHorizontalBoxSlot* ASlot = Row->AddChildToHorizontalBox(ApproveButton))
	{
		ASlot->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	}

	RejectButton = WidgetTree->ConstructWidget<UButton>();
	RejectButton->OnClicked.AddDynamic(this, &UAIDAChatWidget::HandleRejectClicked);
	RejectButton->SetBackgroundColor(FLinearColor(0.6f, 0.25f, 0.25f, 1.0f));
	RejectButton->AddChild(MakeText(TEXT("Reject"), FontSize, FLinearColor::White));
	Row->AddChildToHorizontalBox(RejectButton);

	// Ghost-adjust keybind hint (the ghost itself is the feedback; this is the discoverability).
	if (UVerticalBoxSlot* HintSlot = Column->AddChildToVerticalBox(MakeText(
		TEXT("Ctrl+Arrows move · Ctrl+PgUp/PgDn raise · Ctrl+Wheel rotate (Shift = 1 m)"),
		FontSize - 1.f, FLinearColor(0.75f, 0.75f, 0.75f, 1.0f))))
	{
		HintSlot->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}
}

void UAIDAChatWidget::HandleProposalsChanged()
{
	if (!ProposalPanel || !ProposalText)
	{
		return;
	}

	// v1 shows one card: the oldest still-pending proposal. Outcomes land as System chat lines,
	// so a resolved proposal simply collapses the card (docs/PHASE4.md §4c).
	const AAIDAProposalRelay* R = ProposalRelay.Get();
	TArray<FAIDAProposalView> Views;
	if (R)
	{
		R->GetProposals(Views);
	}
	const FAIDAProposalView* Pending = Views.FindByPredicate(
		[](const FAIDAProposalView& View) { return View.State == TEXT("pending"); });
	if (!Pending)
	{
		ShownProposalId.Invalidate();
		ProposalPanel->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	ShownProposalId = Pending->Id;
	FString Body = FString::Printf(TEXT("%s (proposed for %s)"), *Pending->Summary, *Pending->Requester);
	if (!Pending->CostSummary.IsEmpty())
	{
		Body += FString::Printf(TEXT("\nCost: %s"), *Pending->CostSummary);
	}
	ProposalText->SetText(FText::FromString(Body));
	ProposalPanel->SetVisibility(ESlateVisibility::Visible);
}

void UAIDAChatWidget::HandleApproveClicked()
{
	if (AAIDAProposalRelay* R = ProposalRelay.Get(); R && ShownProposalId.IsValid())
	{
		R->Approve(ShownProposalId);
	}
}

void UAIDAChatWidget::HandleRejectClicked()
{
	if (AAIDAProposalRelay* R = ProposalRelay.Get(); R && ShownProposalId.IsValid())
	{
		R->Reject(ShownProposalId);
	}
}

void UAIDAChatWidget::FocusInput()
{
	if (InputBox)
	{
		InputBox->SetKeyboardFocus();
	}
}

void UAIDAChatWidget::BuildTranscriptRich(UFont* GameFont, float FontSize)
{
	if (!TranscriptScroll || !WidgetTree)
	{
		return; // no scroll box → keep the plain TranscriptText path
	}

	// Open Sans + dark outline, with typeface variants for bold/italic (fall back to regular if the font
	// lacks them). Code is tinted rather than mono'd since the game font has no monospace face.
	auto MakeFont = [GameFont](const FName& Typeface, float Size) -> FSlateFontInfo
	{
		FSlateFontInfo F = GameFont ? FSlateFontInfo(GameFont, static_cast<int32>(Size), Typeface) : FSlateFontInfo();
		// Force no outline — the DescriptionText asset carries a dark default outline; strip it so the
		// text reads flat over the backdrop (user: "remove the black border from the text").
		F.OutlineSettings.OutlineSize = 0;
		F.OutlineSettings.OutlineColor = FLinearColor::Transparent;
		return F;
	};

	UDataTable* StyleSet = NewObject<UDataTable>(this);
	StyleSet->RowStruct = FRichTextStyleRow::StaticStruct();
	auto AddStyle = [StyleSet](const FName& Name, const FSlateFontInfo& F, const FLinearColor& Color)
	{
		FRichTextStyleRow Row;
		Row.TextStyle.SetFont(F);
		Row.TextStyle.SetColorAndOpacity(FSlateColor(Color));
		Row.TextStyle.SetShadowOffset(FVector2D::ZeroVector); // no drop shadow (part of the "black border")
		StyleSet->AddRow(Name, Row);
	};
	// Engine monospace font (DroidSansMono — what the console uses) so code + tables align by character.
	FSlateFontInfo MonoFont = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), FontSize);
	MonoFont.OutlineSettings.OutlineSize = 0;
	MonoFont.OutlineSettings.OutlineColor = FLinearColor::Transparent;

	AddStyle(TEXT("Default"), MakeFont(NAME_None, FontSize), FLinearColor::White);
	AddStyle(TEXT("Bold"), MakeFont(TEXT("Bold"), FontSize), FLinearColor::White);
	AddStyle(TEXT("Italic"), MakeFont(TEXT("Italic"), FontSize), FLinearColor::White);
	AddStyle(TEXT("Code"), MonoFont, FLinearColor(0.70f, 0.88f, 1.0f, 1.0f));
	AddStyle(TEXT("Header"), MakeFont(TEXT("Bold"), FontSize + 3.f), FLinearColor::White);
	AddStyle(TEXT("Mono"), MonoFont, FLinearColor::White);
	AddStyle(TEXT("MonoHeader"), MonoFont, FLinearColor(1.0f, 0.85f, 0.4f, 1.0f)); // amber header

	TranscriptRich = WidgetTree->ConstructWidget<URichTextBlock>();
	TranscriptRich->SetTextStyleSet(StyleSet);
	TranscriptRich->SetDefaultFont(MakeFont(NAME_None, FontSize));
	TranscriptRich->SetAutoWrapText(true);
	TranscriptRich->SetVisibility(ESlateVisibility::HitTestInvisible);
	TranscriptScroll->AddChild(TranscriptRich);

	// The rich block replaces the plain one.
	if (TranscriptText)
	{
		TranscriptText->SetVisibility(ESlateVisibility::Collapsed);
	}
}

void UAIDAChatWidget::SendChat(const FString& Text)
{
	if (AAIDAChatRelay* R = Relay.Get())
	{
		R->SubmitChat(Text, CurrentConversationId);
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
	FConversationView& View = EnsureConversation(Header.ConversationId);
	ConversationOfMessage.Add(Header.Id, Header.ConversationId);

	// Open (or upsert-RESET) the message block for this Id within its conversation.
	if (int32* Existing = View.IndexById.Find(Header.Id))
	{
		View.Messages[*Existing].Body.Reset();
	}
	else
	{
		FRenderedMessage Msg;
		Msg.Id = Header.Id;
		Msg.Prefix = FString::Printf(TEXT("**%s:** "), *Header.Author); // bold author via markdown
		View.IndexById.Add(Header.Id, View.Messages.Add(MoveTemp(Msg)));
	}
	if (Header.ConversationId == CurrentConversationId) { RenderActiveConversation(); }

	OnMessageBegin(Header);
}

void UAIDAChatWidget::HandleMsgChunk(const FGuid& Id, const FString& Delta)
{
	const FGuid ConvId = ConversationOfMessage.FindRef(Id);
	if (FConversationView* View = Conversations.Find(ConvId))
	{
		if (int32* Idx = View->IndexById.Find(Id))
		{
			View->Messages[*Idx].Body += Delta;
			if (ConvId == CurrentConversationId) { RenderActiveConversation(); }
		}
	}

	OnMessageDelta(Id, Delta);
}

void UAIDAChatWidget::HandleMsgEnd(const FGuid& Id)
{
	OnMessageEnd(Id);
}

UAIDAChatWidget::FConversationView& UAIDAChatWidget::EnsureConversation(const FGuid& ConvId)
{
	if (FConversationView* Existing = Conversations.Find(ConvId))
	{
		return *Existing;
	}
	FConversationView& View = Conversations.Add(ConvId);
	TabOrder.Add(ConvId);
	RebuildTabBar();
	return View;
}

void UAIDAChatWidget::SwitchToConversation(const FGuid& ConvId)
{
	CurrentConversationId = ConvId;
	HistoryCursor = INDEX_NONE; // each tab recalls its own history from its newest line
	EnsureConversation(ConvId);
	RebuildTabBar();
	RenderActiveConversation();
	FocusInput();
}

void UAIDAChatWidget::HandleTabClicked(const FGuid& ConvId)
{
	SwitchToConversation(ConvId);
}

void UAIDAChatWidget::HandleNewTabClicked()
{
	SwitchToConversation(FGuid::NewGuid());
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
	RecordInputHistory(Text);
	InputBox->SetText(FText::GetEmpty());
	RefocusInput();
}

void UAIDAChatWidget::HandleInputCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	if (CommitMethod == ETextCommit::OnEnter)
	{
		HandleSendClicked();
	}
}

void UAIDAChatWidget::RecordInputHistory(const FString& Text)
{
	TArray<FString>& History = InputHistory.FindOrAdd(CurrentConversationId);
	if (History.Num() == 0 || History.Last() != Text)
	{
		History.Add(Text);
	}
	HistoryCursor = INDEX_NONE;
}

void UAIDAChatWidget::RefocusInput()
{
	// Committing (Enter) and button clicks both pull Slate focus off the box — the chat window is a
	// conversation, so the caret must stay put. Refocus now AND next tick (Slate finishes routing
	// the commit after this handler returns and would steal it right back).
	FocusInput();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]() { FocusInput(); }));
	}
}

bool UAIDAChatWidget::TryAdjustGhost(const FKey& Key, bool bFineStep)
{
	AAIDAProposalRelay* R = ProposalRelay.Get();
	if (!R || !R->HasPendingProposal())
	{
		return false;
	}

	// Camera-relative cardinals snapped to the world lattice: Up-arrow pushes the ghost away from
	// the camera, Right pushes it right, etc. — how players think while looking at the ghost.
	FVector Forward = FVector::ForwardVector;
	const APlayerController* PC = GetOwningPlayer();
	if (PC && PC->PlayerCameraManager)
	{
		const double SnappedYaw = FMath::RoundToDouble(PC->PlayerCameraManager->GetCameraRotation().Yaw / 90.0) * 90.0;
		Forward = FRotator(0.0, SnappedYaw, 0.0).Vector();
	}
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward) * -1.0; // yaw+90 = right hand

	const double StepCm = (bFineStep ? 1.0 : 8.0) * 100.0; // Shift = 1 m fine steps, else one tile
	FVector Delta = FVector::ZeroVector;
	if (Key == EKeys::Up)            { Delta = Forward * StepCm; }
	else if (Key == EKeys::Down)     { Delta = -Forward * StepCm; }
	else if (Key == EKeys::Right)    { Delta = Right * StepCm; }
	else if (Key == EKeys::Left)     { Delta = -Right * StepCm; }
	else if (Key == EKeys::PageUp)   { Delta = FVector(0.0, 0.0, StepCm); }
	else if (Key == EKeys::PageDown) { Delta = FVector(0.0, 0.0, -StepCm); }
	else
	{
		return false;
	}
	R->Adjust(Delta, 0);
	return true;
}

FReply UAIDAChatWidget::NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	// Ctrl+Arrows / Ctrl+PgUp/PgDn move the pending proposal ghost (Shift for 1 m fine steps).
	if (InKeyEvent.IsControlDown() && TryAdjustGhost(InKeyEvent.GetKey(), InKeyEvent.IsShiftDown()))
	{
		return FReply::Handled();
	}

	// Up/Down = shell-style recall of this conversation's sent lines, only while typing in the box.
	if (InputBox && InputBox->HasKeyboardFocus())
	{
		const FKey Key = InKeyEvent.GetKey();
		if (Key == EKeys::Up || Key == EKeys::Down)
		{
			if (const TArray<FString>* History = InputHistory.Find(CurrentConversationId); History && History->Num() > 0)
			{
				if (Key == EKeys::Up)
				{
					HistoryCursor = (HistoryCursor == INDEX_NONE) ? History->Num() - 1 : FMath::Max(0, HistoryCursor - 1);
				}
				else if (HistoryCursor != INDEX_NONE && ++HistoryCursor >= History->Num())
				{
					HistoryCursor = INDEX_NONE; // stepped past the newest entry -> back to a blank line
				}
				InputBox->SetText(HistoryCursor == INDEX_NONE ? FText::GetEmpty() : FText::FromString((*History)[HistoryCursor]));
			}
			return FReply::Handled();
		}
	}
	return Super::NativeOnPreviewKeyDown(InGeometry, InKeyEvent);
}

FReply UAIDAChatWidget::NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	// Ctrl+Wheel rotates the pending proposal ghost 90° per notch (plain wheel stays the hotbar's).
	if (InMouseEvent.IsControlDown())
	{
		if (AAIDAProposalRelay* R = ProposalRelay.Get(); R && R->HasPendingProposal())
		{
			R->Adjust(FVector::ZeroVector, InMouseEvent.GetWheelDelta() > 0.f ? 90 : -90);
			return FReply::Handled();
		}
	}
	return Super::NativeOnMouseWheel(InGeometry, InMouseEvent);
}

void UAIDAChatWidget::RenderActiveConversation()
{
	FString Out;
	if (const FConversationView* View = Conversations.Find(CurrentConversationId))
	{
		for (const FRenderedMessage& Msg : View->Messages)
		{
			if (!Out.IsEmpty()) { Out += TEXT("\n\n"); }
			Out += Msg.Prefix;
			Out += Msg.Body;
		}
	}
	RenderedTranscript = MoveTemp(Out);

	if (TranscriptRich)
	{
		TranscriptRich->SetText(FText::FromString(AIDAMarkdownToRichText(RenderedTranscript)));
	}
	else if (TranscriptText)
	{
		TranscriptText->SetText(FText::FromString(RenderedTranscript));
	}
	if (TranscriptScroll)
	{
		TranscriptScroll->ScrollToEnd();
	}
	OnTranscriptChanged.Broadcast(RenderedTranscript);
}

void UAIDAChatWidget::RebuildTabBar()
{
	if (!TabBar || !WidgetTree)
	{
		return;
	}
	TabBar->ClearChildren();

	// Small label font so the tab buttons stay compact and align to the strip height.
	auto MakeLabel = [this](const FString& Text, const FLinearColor& Color) -> UTextBlock*
	{
		UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>();
		Label->SetText(FText::FromString(Text));
		FSlateFontInfo LabelFont = Label->GetFont();
		LabelFont.Size = 10;
		Label->SetFont(LabelFont);
		Label->SetColorAndOpacity(FSlateColor(Color));
		return Label;
	};

	for (int32 i = 0; i < TabOrder.Num(); ++i)
	{
		const FGuid& ConvId = TabOrder[i];
		const bool bActive = (ConvId == CurrentConversationId);

		UAIDATabButton* Btn = WidgetTree->ConstructWidget<UAIDATabButton>();
		Btn->InitTab(ConvId);
		Btn->OnTabClickedNative.AddUObject(this, &UAIDAChatWidget::HandleTabClicked);
		Btn->AddChild(MakeLabel(FString::Printf(TEXT("Chat %d"), i + 1),
			bActive ? FLinearColor(1.0f, 0.85f, 0.4f, 1.0f) : FLinearColor(0.8f, 0.8f, 0.8f, 1.0f)));

		if (UHorizontalBoxSlot* BSlot = TabBar->AddChildToHorizontalBox(Btn))
		{
			BSlot->SetPadding(FMargin(2.f, 0.f, 2.f, 0.f));
			BSlot->SetVerticalAlignment(VAlign_Fill);
		}
	}

	// "+" new-tab button.
	UButton* Plus = WidgetTree->ConstructWidget<UButton>();
	Plus->OnClicked.AddDynamic(this, &UAIDAChatWidget::HandleNewTabClicked);
	Plus->AddChild(MakeLabel(TEXT("+"), FLinearColor(0.9f, 0.9f, 0.9f, 1.0f)));
	if (UHorizontalBoxSlot* PSlot = TabBar->AddChildToHorizontalBox(Plus))
	{
		PSlot->SetPadding(FMargin(6.f, 0.f, 2.f, 0.f));
		PSlot->SetVerticalAlignment(VAlign_Fill);
	}
}
