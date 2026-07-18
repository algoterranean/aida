#include "UI/AIDAChatWidget.h"

#include "AIDA.h"
#include "Core/AIDAImageNormalize.h"
#include "Core/AIDAImageStore.h" // FAIDAImageUploadAssembler::kChunkBytes (shared wire constant)
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
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/DataTable.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "Styling/CoreStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "TimerManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UI/AIDAMarkdown.h"

#if PLATFORM_WINDOWS
// Attach-file dialog: raw Win32 (DesktopPlatform is a Developer module — absent in Shipping).
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <commdlg.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

void UAIDAChatWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// NativeTick drives the transcript scrollbar sync, but UUserWidget's tick heuristic only
	// detects a BLUEPRINT Tick event — and this BP is a pure layout with none, so without this
	// flag the widget never ticks and the slider stays collapsed (live-verify: no scrollbar).
	bHasScriptImplementedTick = true;
	UpdateCanTick();

	// Self-wire the optional sub-widgets so the Blueprint can stay a pure layout (no event graph).
	if (SendButton) { SendButton->SetClickMethod(EButtonClickMethod::MouseDown); } // press-fired (focus-flip race)
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
		// The built-in scrollbar is dead under HitTestInvisible (it drew but could never be
		// grabbed) — hide it; the gutter slider below is the interactive scroll control.
		TranscriptScroll->SetScrollBarVisibility(ESlateVisibility::Collapsed);
	}
	if (TranscriptText)
	{
		TranscriptText->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);

	// --- Layout + font tuning (driven here so the BP stays a pure, positionless view) ---
	// Tweakables (slate units ~= design px). Bump ToolbarClearance if the input row overlaps the hotbar.
	constexpr float Margin          = 20.f;  // outer margin from screen edges
	constexpr float InputHeight     = 26.f;  // height of the input box / Send button row
	constexpr float SendWidth       = 64.f;  // width of the Send button
	constexpr float AttachWidth     = 30.f;  // width of the attach (+) button left of the input box
	constexpr float RowGap          = 8.f;   // gap between transcript, input box, and Send button
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
	// Input box: bottom row of the left half, between the attach button and the Send button.
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(InputBox ? InputBox->Slot : nullptr))
	{
		CanvasSlot->SetAnchors(FAnchors(0.f, 1.f, RightAnchor, 1.f));
		CanvasSlot->SetAlignment(FVector2D(0.f, 1.f));
		CanvasSlot->SetOffsets(FMargin(Margin + AttachWidth + RowGap, -BottomInset, SendWidth + RowGap + Margin, InputHeight));
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

		// The BP's Send label ships with the engine's large default font; pull it down to match.
		if (SendButton)
		{
			for (int32 i = 0; i < SendButton->GetChildrenCount(); ++i)
			{
				if (UTextBlock* SendLabel = Cast<UTextBlock>(SendButton->GetChildAt(i)))
				{
					SendLabel->SetFont(Font);
				}
			}
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
		Backdrop->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.025f, 0.78f)); // near-opaque dark (user: "darker")
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

		// Transcript scrollbar: the transcript is click-through by design (see above), which also
		// kills the ScrollBox's own scrollbar — a slim vertical slider in the outer-margin gutter
		// right of the transcript is the ONE hit-testable scroll control (user: "can't scroll the
		// scrollbar"). NativeTick mirrors the scroll state onto it; it hides while the text fits.
		TranscriptSlider = WidgetTree->ConstructWidget<USlider>();
		TranscriptSlider->SetOrientation(EOrientation::Orient_Vertical);
		TranscriptSlider->SetSliderBarColor(FLinearColor(1.f, 1.f, 1.f, 0.15f));
		TranscriptSlider->SetSliderHandleColor(FLinearColor(0.85f, 0.85f, 0.85f, 0.9f));
		TranscriptSlider->SetVisibility(ESlateVisibility::Collapsed);
		TranscriptSlider->OnValueChanged.AddDynamic(this, &UAIDAChatWidget::HandleSliderValueChanged);
		TranscriptSlider->OnMouseCaptureBegin.AddDynamic(this, &UAIDAChatWidget::HandleSliderCaptureBegin);
		TranscriptSlider->OnMouseCaptureEnd.AddDynamic(this, &UAIDAChatWidget::HandleSliderCaptureEnd);
		if (UCanvasPanelSlot* SliderSlot = Cast<UCanvasPanelSlot>(RootPanel->AddChild(TranscriptSlider)))
		{
			SliderSlot->SetAnchors(FAnchors(RightAnchor, 0.f, RightAnchor, 1.f));
			SliderSlot->SetAlignment(FVector2D(1.f, 0.f));
			// Right edge 4px inside the half-line; spans the transcript's vertical extent.
			SliderSlot->SetOffsets(FMargin(-4.f, TranscriptTop, 14.f, BottomInset + InputHeight + RowGap));
		}
	}

	// Proposal card (Phase 4): top of the unused right half, collapsed until a proposal is pending.
	BuildProposalPanel(GameFont, FontSize);

	// Attach button + pending-attachment chip row (Phase 5).
	BuildAttachmentRow(GameFont, FontSize, Margin, BottomInset, InputHeight, AttachWidth, RowGap, RightAnchor);

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
	Found->OnUploadAck.AddDynamic(this, &UAIDAChatWidget::HandleUploadAck);
	Found->OnUploadResult.AddDynamic(this, &UAIDAChatWidget::HandleUploadResult);

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
		R->OnUploadAck.RemoveDynamic(this, &UAIDAChatWidget::HandleUploadAck);
		R->OnUploadResult.RemoveDynamic(this, &UAIDAChatWidget::HandleUploadResult);
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
	ProposalPanel->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.025f, 0.82f));
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
	// Fire on PRESS: the press steals keyboard focus from the input box, and the focus-driven
	// input-mode flip could re-capture the mouse before a release-fired click ever completed
	// (live-verify: "click approve, nothing happens").
	ApproveButton->SetClickMethod(EButtonClickMethod::MouseDown);
	ApproveButton->OnClicked.AddDynamic(this, &UAIDAChatWidget::HandleApproveClicked);
	ApproveButton->SetBackgroundColor(FLinearColor(0.25f, 0.6f, 0.25f, 1.0f));
	ApproveButton->AddChild(MakeText(TEXT("Approve"), FontSize, FLinearColor::White));
	if (UHorizontalBoxSlot* ASlot = Row->AddChildToHorizontalBox(ApproveButton))
	{
		ASlot->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	}

	RejectButton = WidgetTree->ConstructWidget<UButton>();
	RejectButton->SetClickMethod(EButtonClickMethod::MouseDown); // press-fired, like Approve
	RejectButton->OnClicked.AddDynamic(this, &UAIDAChatWidget::HandleRejectClicked);
	RejectButton->SetBackgroundColor(FLinearColor(0.6f, 0.25f, 0.25f, 1.0f));
	RejectButton->AddChild(MakeText(TEXT("Reject"), FontSize, FLinearColor::White));
	Row->AddChildToHorizontalBox(RejectButton);

	// Ghost-adjust keybind hint (the ghost itself is the feedback; this is the discoverability).
	if (UVerticalBoxSlot* HintSlot = Column->AddChildToVerticalBox(MakeText(
		TEXT("Ctrl+Arrows move (Shift = 1 m) · Ctrl+PgUp/PgDn raise · Ctrl+Wheel rotate 90° (Shift 15°, Alt 1°)"),
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

bool UAIDAChatWidget::IsInputFocused() const
{
	return InputBox && InputBox->HasKeyboardFocus();
}

void UAIDAChatWidget::UnfocusInput()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetAllUserFocusToGameViewport();
	}
}

void UAIDAChatWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// Focus drives the input mode (user rule): typing needs UI routing + the cursor; the rest of
	// the time the window is a pure overlay and the player moves/looks with normal game input.
	// Polling covers every way focus can change — clicks, ESC, Ctrl+Enter, Slate steals.
	// Focus GAIN flips immediately; focus LOSS waits a short grace — pressing a UI button steals
	// focus from the input box, and an instant flip to game-only re-captured the mouse mid-click
	// so the button never fired (live-verify: "click approve, nothing happens").
	const bool bInputFocused = IsInputFocused();
	if (bInputFocused)
	{
		UnfocusGraceSeconds = 0.f;
		if (!bLastInputFocused)
		{
			bLastInputFocused = true;
			if (APlayerController* PC = GetOwningPlayer())
			{
				// UI ONLY while typing: GameAndUI leaks key-DOWN events the text box doesn't
				// consume straight into the game's keybinds (live-verify: typing "x" toggled the
				// game's X binding). Focus loss flips back to pure game input below.
				PC->SetInputMode(FInputModeUIOnly());
				PC->bShowMouseCursor = true;
			}
		}
	}
	else if (bLastInputFocused)
	{
		UnfocusGraceSeconds += InDeltaTime;
		if (UnfocusGraceSeconds > 0.3f)
		{
			UnfocusGraceSeconds = 0.f;
			bLastInputFocused = false;
			if (APlayerController* PC = GetOwningPlayer())
			{
				PC->SetInputMode(FInputModeGameOnly());
				PC->bShowMouseCursor = false;
			}
		}
	}

	// Mirror the (click-through) scroll box onto the gutter slider: thumb at the TOP = offset 0
	// (vertical sliders run bottom→top, so the value is inverted). Hidden while the text fits.
	if (TranscriptScroll && TranscriptSlider)
	{
		const float End = TranscriptScroll->GetScrollOffsetOfEnd();
		const ESlateVisibility Wanted = End > 1.f ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
		if (TranscriptSlider->GetVisibility() != Wanted)
		{
			TranscriptSlider->SetVisibility(Wanted);
		}
		if (!bSliderDragging && End > 1.f)
		{
			// SetValue is silent (no OnValueChanged) — no feedback loop with the drag handler.
			TranscriptSlider->SetValue(1.f - FMath::Clamp(TranscriptScroll->GetScrollOffset() / End, 0.f, 1.f));
		}
	}
}

void UAIDAChatWidget::ScrollTranscriptBy(float DeltaPx)
{
	if (TranscriptScroll)
	{
		const float End = FMath::Max(0.f, TranscriptScroll->GetScrollOffsetOfEnd());
		TranscriptScroll->SetScrollOffset(FMath::Clamp(TranscriptScroll->GetScrollOffset() + DeltaPx, 0.f, End));
	}
}

bool UAIDAChatWidget::IsScreenPositionOverTranscript(const FVector2D& ScreenPos) const
{
	return TranscriptScroll && TranscriptScroll->GetCachedGeometry().IsUnderLocation(ScreenPos);
}

void UAIDAChatWidget::HandleSliderValueChanged(float Value)
{
	if (TranscriptScroll)
	{
		TranscriptScroll->SetScrollOffset((1.f - Value) * FMath::Max(0.f, TranscriptScroll->GetScrollOffsetOfEnd()));
	}
}

void UAIDAChatWidget::HandleSliderCaptureBegin()
{
	bSliderDragging = true;
}

void UAIDAChatWidget::HandleSliderCaptureEnd()
{
	bSliderDragging = false;
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
	AddStyle(TEXT("Header"), MakeFont(TEXT("Bold"), FontSize + 2.f), FLinearColor::White); // compact: +2, was +3
	AddStyle(TEXT("Mono"), MonoFont, FLinearColor::White);
	AddStyle(TEXT("MonoHeader"), MonoFont, FLinearColor(1.0f, 0.85f, 0.4f, 1.0f)); // amber header

	// Per-participant name colors (user ask): AIDA amber, System gray, players from a fixed palette
	// by name hash — the same speaker keeps the same color all session. Sys dims whole system lines.
	AddStyle(TEXT("NameAida"), MakeFont(TEXT("Bold"), FontSize), FLinearColor(1.0f, 0.72f, 0.30f, 1.0f));
	AddStyle(TEXT("NameSys"), MakeFont(TEXT("Bold"), FontSize - 1.f), FLinearColor(0.62f, 0.62f, 0.62f, 1.0f));
	AddStyle(TEXT("Sys"), MakeFont(NAME_None, FontSize - 1.f), FLinearColor(0.66f, 0.66f, 0.66f, 1.0f));
	const FLinearColor NamePalette[6] = {
		FLinearColor(0.45f, 0.85f, 1.00f, 1.f),  // cyan
		FLinearColor(0.55f, 0.95f, 0.55f, 1.f),  // green
		FLinearColor(0.95f, 0.60f, 0.95f, 1.f),  // magenta
		FLinearColor(1.00f, 0.95f, 0.50f, 1.f),  // yellow
		FLinearColor(0.60f, 0.70f, 1.00f, 1.f),  // blue
		FLinearColor(1.00f, 0.60f, 0.55f, 1.f)   // salmon
	};
	for (int32 i = 0; i < 6; ++i)
	{
		AddStyle(FName(*FString::Printf(TEXT("Name%d"), i)), MakeFont(TEXT("Bold"), FontSize), NamePalette[i]);
	}

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
	// Typing always snaps the view to the tail (user rule) — the reply lands where you're looking.
	if (TranscriptScroll)
	{
		TranscriptScroll->ScrollToEnd();
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
		// Author names get their own color style (rich tags pass through the markdown converter
		// untouched): AIDA amber, System gray, players hashed into a stable palette slot.
		const TCHAR* Tag = nullptr;
		FString PlayerTag;
		const bool bSystem = Header.Author.Equals(TEXT("System"), ESearchCase::IgnoreCase);
		if (bSystem) { Tag = TEXT("NameSys"); }
		else if (Header.Author.Equals(TEXT("AIDA"), ESearchCase::IgnoreCase)) { Tag = TEXT("NameAida"); }
		else
		{
			PlayerTag = FString::Printf(TEXT("Name%u"), GetTypeHash(Header.Author.ToLower()) % 6u);
			Tag = *PlayerTag;
		}

		FRenderedMessage Msg;
		Msg.Id = Header.Id;
		Msg.bSystem = bSystem;
		Msg.Prefix = FString::Printf(TEXT("<%s>%s:</> "), Tag, *Header.Author);
		if (Header.ImageCount > 0)
		{
			// Attachment marker only — pixels never replicate to other clients (docs/PHASE5.md).
			Msg.Prefix += FString::Printf(TEXT("[+%d image%s] "), Header.ImageCount,
				Header.ImageCount == 1 ? TEXT("") : TEXT("s"));
		}
		View.IndexById.Add(Header.Id, View.Messages.Add(MoveTemp(Msg)));
	}
	if (Header.ConversationId == CurrentConversationId)
	{
		RenderActiveConversation();
		// A NEW message (the player's echo, AIDA's reply opening, a system line) always snaps to
		// the tail (user rule: fresh text must be visible). Mid-stream deltas stay sticky-only so
		// deliberately scrolling up during a long reply still works.
		if (TranscriptScroll)
		{
			TranscriptScroll->ScrollToEnd();
		}
	}

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
	if (TranscriptScroll)
	{
		TranscriptScroll->ScrollToEnd(); // a fresh tab always opens at its newest message
	}
	RebuildAttachmentChips(); // chips are per conversation and follow the active tab
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

	// `/aida attach [path]` is the ONE chat command parsed client-side — the path lives on THIS
	// machine's disk, so the server parser can never resolve it. No arg = open the file dialog.
	if (Text.TrimStartAndEnd().StartsWith(TEXT("/aida attach"), ESearchCase::IgnoreCase))
	{
		FString PathArg = Text.TrimStartAndEnd().Mid(12).TrimStartAndEnd();
		PathArg = PathArg.TrimQuotes();
		RecordInputHistory(Text);
		InputBox->SetText(FText::GetEmpty());
		RefocusInput();
		if (PathArg.IsEmpty())
		{
			HandleAttachClicked();
		}
		else
		{
			StartAttach(PathArg);
		}
		return;
	}

	// Attach the chips that finished uploading (they leave the row); still-uploading or failed
	// chips stay pending for a later send so nothing silently detaches.
	TArray<FGuid> ReadyIds;
	if (TArray<FPendingAttachment>* List = PendingAttachments.Find(CurrentConversationId))
	{
		for (int32 i = List->Num() - 1; i >= 0; --i)
		{
			if ((*List)[i].ImageId.IsValid() && !(*List)[i].bFailed)
			{
				ReadyIds.Insert((*List)[i].ImageId, 0);
				List->RemoveAt(i);
			}
		}
	}

	if (Text.IsEmpty() && ReadyIds.Num() == 0)
	{
		return;
	}

	if (ReadyIds.Num() > 0)
	{
		if (AAIDAChatRelay* R = Relay.Get())
		{
			R->SubmitChatWithImages(Text, CurrentConversationId, ReadyIds);
		}
		RebuildAttachmentChips();
	}
	else
	{
		SendChat(Text);
	}

	if (!Text.IsEmpty())
	{
		RecordInputHistory(Text);
	}
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
	// User rule: the window captures keys ONLY while the input box has focus — otherwise the
	// player is walking around with the chat as an overlay and every key belongs to the game.
	if (!IsInputFocused())
	{
		return Super::NativeOnPreviewKeyDown(InGeometry, InKeyEvent);
	}

	// Ctrl+Arrows / Ctrl+PgUp/PgDn move the pending proposal ghost (Shift for 1 m fine steps).
	if (InKeyEvent.IsControlDown() && TryAdjustGhost(InKeyEvent.GetKey(), InKeyEvent.IsShiftDown()))
	{
		return FReply::Handled();
	}

	// Plain PageUp/PageDown page the transcript (the Ctrl variants are the ghost's raise/lower).
	if (!InKeyEvent.IsControlDown() &&
		(InKeyEvent.GetKey() == EKeys::PageUp || InKeyEvent.GetKey() == EKeys::PageDown))
	{
		const float Viewport = TranscriptScroll ? TranscriptScroll->GetCachedGeometry().GetLocalSize().Y : 0.f;
		const float Page = Viewport > 1.f ? Viewport * 0.8f : 300.f;
		ScrollTranscriptBy(InKeyEvent.GetKey() == EKeys::PageUp ? -Page : Page);
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

bool UAIDAChatWidget::TryRotatePendingProposal(int32 YawDeltaDeg)
{
	AAIDAProposalRelay* R = ProposalRelay.Get();
	if (!R || !R->HasPendingProposal())
	{
		return false;
	}
	R->Adjust(FVector::ZeroVector, YawDeltaDeg);
	return true;
}

FReply UAIDAChatWidget::NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	// Ctrl+Wheel rotates the pending proposal ghost: 90° per notch, Shift = 15°, Alt = 1°.
	if (InMouseEvent.IsControlDown())
	{
		const int32 Step = InMouseEvent.IsAltDown() ? 1 : (InMouseEvent.IsShiftDown() ? 15 : 90);
		if (TryRotatePendingProposal(InMouseEvent.GetWheelDelta() > 0.f ? Step : -Step))
		{
			return FReply::Handled();
		}
	}
	// Plain wheel scrolls the transcript. This only fires when the cursor is over one of the
	// window's HIT-TESTABLE parts (input row, tabs, slider) — the transcript itself is
	// click-through, so wheel over it (and over the game world) still drives the hotbar.
	ScrollTranscriptBy(InMouseEvent.GetWheelDelta() * -60.f);
	return FReply::Handled();
}

void UAIDAChatWidget::RenderActiveConversation()
{
	FString Out;
	if (const FConversationView* View = Conversations.Find(CurrentConversationId))
	{
		for (const FRenderedMessage& Msg : View->Messages)
		{
			// Compact transcript (user ask): single newline between messages, like a terminal.
			if (!Out.IsEmpty()) { Out += TEXT("\n"); }
			Out += Msg.Prefix;
			if (Msg.bSystem)
			{
				// Whole system line dimmed. Rich-text tags don't nest, so strip anything the
				// markdown pass could turn into a tag — system lines are plain server prose anyway.
				FString Plain = Msg.Body;
				Plain.ReplaceInline(TEXT("*"), TEXT(""));
				Plain.ReplaceInline(TEXT("`"), TEXT(""));
				Plain.ReplaceInline(TEXT("<"), TEXT("("));
				Plain.ReplaceInline(TEXT(">"), TEXT(")"));
				Plain.ReplaceInline(TEXT("\n"), TEXT("</>\n<Sys>")); // tags don't span lines
				Out += FString::Printf(TEXT("<Sys>%s</>"), *Plain);
			}
			else
			{
				Out += Msg.Body;
			}
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
		// Stick to the end only when the reader is already there — a player scrolled up to read
		// must not be yanked back down by every streamed delta. (Offsets are PRE-append here;
		// content only grows, so "within a line of the end" is the right stickiness test.)
		const float End = TranscriptScroll->GetScrollOffsetOfEnd();
		if (End <= 1.f || TranscriptScroll->GetScrollOffset() >= End - 20.f)
		{
			TranscriptScroll->ScrollToEnd();
		}
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
		Btn->SetClickMethod(EButtonClickMethod::MouseDown); // press-fired (focus-flip race)
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
	Plus->SetClickMethod(EButtonClickMethod::MouseDown);
	Plus->OnClicked.AddDynamic(this, &UAIDAChatWidget::HandleNewTabClicked);
	Plus->AddChild(MakeLabel(TEXT("+"), FLinearColor(0.9f, 0.9f, 0.9f, 1.0f)));
	if (UHorizontalBoxSlot* PSlot = TabBar->AddChildToHorizontalBox(Plus))
	{
		PSlot->SetPadding(FMargin(6.f, 0.f, 2.f, 0.f));
		PSlot->SetVerticalAlignment(VAlign_Fill);
	}
}

//~ ─────────────────────── Phase 5: reference-image attachments ───────────────────────

void UAIDAChatWidget::BuildAttachmentRow(UFont* GameFont, float FontSize, float Margin,
	float BottomInset, float InputHeight, float AttachWidth, float RowGap, float RightAnchor)
{
	UPanelWidget* RootPanel = Cast<UPanelWidget>(GetRootWidget());
	if (!RootPanel || !WidgetTree)
	{
		return;
	}

	const auto MakeLabel = [this, GameFont, FontSize](const FString& Text, const FLinearColor& Color) -> UTextBlock*
	{
		UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>();
		Block->SetText(FText::FromString(Text));
		FSlateFontInfo Font = GameFont ? FSlateFontInfo(GameFont, static_cast<int32>(FontSize)) : Block->GetFont();
		Font.Size = FontSize;
		Font.OutlineSettings.OutlineSize = 0;
		Font.OutlineSettings.OutlineColor = FLinearColor::Transparent;
		Block->SetFont(Font);
		Block->SetColorAndOpacity(FSlateColor(Color));
		return Block;
	};

	// Attach button: bottom-left, square, in the slot the input box was shifted right to clear.
	AttachButton = WidgetTree->ConstructWidget<UButton>();
	AttachButton->SetClickMethod(EButtonClickMethod::MouseDown);
	AttachButton->OnClicked.AddDynamic(this, &UAIDAChatWidget::HandleAttachClicked);
	AttachButton->SetToolTipText(FText::FromString(TEXT("Attach a reference image (or type /aida attach <path>)")));
	AttachButton->AddChild(MakeLabel(TEXT("+img"), FLinearColor(0.9f, 0.9f, 0.9f, 1.0f)));
	if (UCanvasPanelSlot* BtnSlot = Cast<UCanvasPanelSlot>(RootPanel->AddChild(AttachButton)))
	{
		BtnSlot->SetAnchors(FAnchors(0.f, 1.f, 0.f, 1.f));
		BtnSlot->SetAlignment(FVector2D(0.f, 1.f));
		BtnSlot->SetOffsets(FMargin(Margin, -BottomInset, AttachWidth, InputHeight));
	}

	// Chip row: hugs the top edge of the input row (overlaying the transcript's last pixels is fine
	// — the row is empty and hit-test-invisible until something is attached).
	AttachmentChips = WidgetTree->ConstructWidget<UHorizontalBox>();
	if (UCanvasPanelSlot* ChipSlot = Cast<UCanvasPanelSlot>(RootPanel->AddChild(AttachmentChips)))
	{
		ChipSlot->SetAnchors(FAnchors(0.f, 1.f, RightAnchor, 1.f));
		ChipSlot->SetAlignment(FVector2D(0.f, 1.f));
		ChipSlot->SetOffsets(FMargin(Margin, -(BottomInset + InputHeight + 4.f), Margin, 24.f));
	}
}

void UAIDAChatWidget::HandleAttachClicked()
{
#if PLATFORM_WINDOWS
	TCHAR FileName[MAX_PATH] = { 0 };
	OPENFILENAMEW Ofn = { 0 };
	Ofn.lStructSize = sizeof(Ofn);
	Ofn.hwndOwner = GetActiveWindow();
	Ofn.lpstrFilter = TEXT("Images (*.jpg;*.jpeg;*.png;*.bmp)\0*.jpg;*.jpeg;*.png;*.bmp\0All files (*.*)\0*.*\0");
	Ofn.lpstrFile = FileName;
	Ofn.nMaxFile = MAX_PATH;
	Ofn.lpstrTitle = TEXT("Attach a reference image");
	Ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (GetOpenFileNameW(&Ofn))
	{
		StartAttach(FString(FileName));
	}
#else
	UE_LOG(LogAIDA, Warning, TEXT("[widget] file dialog unavailable on this platform — use /aida attach <path>."));
#endif
}

void UAIDAChatWidget::StartAttach(const FString& FilePath)
{
	TArray<FPendingAttachment>& List = PendingAttachments.FindOrAdd(CurrentConversationId);
	if (List.Num() >= kMaxAttachmentsPerMessage)
	{
		UE_LOG(LogAIDA, Warning, TEXT("[widget] attachment limit (%d) reached for this conversation."),
			kMaxAttachmentsPerMessage);
		return;
	}

	FPendingAttachment Att;
	Att.FileName = FPaths::GetCleanFilename(FilePath);

	TArray<uint8> FileBytes;
	FString Error;
	if (!FFileHelper::LoadFileToArray(FileBytes, *FilePath))
	{
		Att.bFailed = true;
		Att.Error = TEXT("could not read file");
	}
	else if (!AIDANormalizeImageBytes(FileBytes, kClientMaxImageDim, Att.Jpeg, Error))
	{
		Att.bFailed = true;
		Att.Error = Error;
	}
	else
	{
		Att.ChunkCount = FMath::DivideAndRoundUp(Att.Jpeg.Num(), FAIDAImageUploadAssembler::kChunkBytes);
	}

	List.Add(MoveTemp(Att));
	RebuildAttachmentChips();
	StartNextUpload();
}

UAIDAChatWidget::FPendingAttachment* UAIDAChatWidget::FindUploading(FGuid* OutConversation)
{
	for (auto& Pair : PendingAttachments)
	{
		for (FPendingAttachment& Att : Pair.Value)
		{
			if (Att.bUploading)
			{
				if (OutConversation) { *OutConversation = Pair.Key; }
				return &Att;
			}
		}
	}
	return nullptr;
}

void UAIDAChatWidget::StartNextUpload()
{
	if (FindUploading())
	{
		return; // one in-flight upload per player (the server keeps one session per PlayerId)
	}
	AAIDAChatRelay* R = Relay.Get();
	if (!R)
	{
		return; // relay not bound yet; StartNextUpload re-runs on the next attach/result
	}
	for (auto& Pair : PendingAttachments)
	{
		for (FPendingAttachment& Att : Pair.Value)
		{
			if (!Att.bUploading && !Att.bFailed && !Att.ImageId.IsValid())
			{
				Att.bUploading = true;
				Att.NextChunk = 0;
				R->BeginImageUpload(TEXT("image/jpeg"), Att.Jpeg.Num(), Att.ChunkCount);
				RebuildAttachmentChips();
				return;
			}
		}
	}
}

void UAIDAChatWidget::HandleUploadAck(int32 UpToSeq)
{
	FPendingAttachment* Att = FindUploading();
	AAIDAChatRelay* R = Relay.Get();
	if (!Att || !R)
	{
		return;
	}

	// Ack-paced window (docs/PHASE5.md §3): never more than kUploadWindow chunks beyond the ack.
	// Claim the chunk index BEFORE sending: on a listen host the RPC round-trip is synchronous, so
	// the next ack re-enters this handler before the line after the send runs.
	while (Att->NextChunk < Att->ChunkCount && Att->NextChunk <= UpToSeq + kUploadWindow)
	{
		const int32 Seq = Att->NextChunk++;
		const int32 Offset = Seq * FAIDAImageUploadAssembler::kChunkBytes;
		const int32 Len = FMath::Min(FAIDAImageUploadAssembler::kChunkBytes, Att->Jpeg.Num() - Offset);
		TArray<uint8> Chunk(Att->Jpeg.GetData() + Offset, Len);
		R->SendImageUploadChunk(Seq, Chunk);
	}

	if (UpToSeq == Att->ChunkCount - 1)
	{
		R->CommitImageUpload(FCrc::MemCrc32(Att->Jpeg.GetData(), Att->Jpeg.Num()));
	}
	RebuildAttachmentChips(); // progress % moved
}

void UAIDAChatWidget::HandleUploadResult(bool bOk, const FGuid& ImageId, const FString& Error)
{
	if (FPendingAttachment* Att = FindUploading())
	{
		Att->bUploading = false;
		if (bOk)
		{
			Att->ImageId = ImageId;
			Att->Jpeg.Empty(); // committed server-side; the id is all a send needs
		}
		else
		{
			Att->bFailed = true;
			Att->Error = Error;
			UE_LOG(LogAIDA, Warning, TEXT("[widget] attachment '%s' failed: %s"), *Att->FileName, *Error);
		}
	}
	RebuildAttachmentChips();
	StartNextUpload();
}

void UAIDAChatWidget::HandleChipRemoveClicked(int32 Index)
{
	TArray<FPendingAttachment>* List = PendingAttachments.Find(CurrentConversationId);
	if (!List || !List->IsValidIndex(Index) || (*List)[Index].bUploading)
	{
		return; // an in-flight upload can't be cancelled client-side; let it finish, then remove
	}
	List->RemoveAt(Index);
	RebuildAttachmentChips();
}

void UAIDAChatWidget::RebuildAttachmentChips()
{
	if (!AttachmentChips || !WidgetTree)
	{
		return;
	}
	AttachmentChips->ClearChildren();

	const TArray<FPendingAttachment>* List = PendingAttachments.Find(CurrentConversationId);
	if (!List || List->Num() == 0)
	{
		return;
	}

	UFont* GameFont = LoadObject<UFont>(nullptr, TEXT("/Game/FactoryGame/Interface/Font/DescriptionText.DescriptionText"));

	for (int32 i = 0; i < List->Num(); ++i)
	{
		const FPendingAttachment& Att = (*List)[i];

		FString Status;
		FLinearColor Color(0.85f, 0.85f, 0.85f, 1.f);
		if (Att.bFailed)
		{
			Status = FString::Printf(TEXT(" — %s"), *Att.Error);
			Color = FLinearColor(1.f, 0.45f, 0.4f, 1.f);
		}
		else if (Att.ImageId.IsValid())
		{
			Status = TEXT(" [ok]");
			Color = FLinearColor(0.6f, 1.f, 0.6f, 1.f);
		}
		else if (Att.bUploading)
		{
			Status = FString::Printf(TEXT(" %d%%"), Att.ChunkCount > 0 ? Att.NextChunk * 100 / Att.ChunkCount : 0);
		}
		else
		{
			Status = TEXT(" ...");
		}

		UBorder* Chip = WidgetTree->ConstructWidget<UBorder>();
		Chip->SetBrushColor(FLinearColor(0.05f, 0.05f, 0.06f, 0.9f));
		Chip->SetPadding(FMargin(6.f, 2.f));

		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		Chip->AddChild(Row);

		UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>();
		Label->SetText(FText::FromString(FString::Printf(TEXT("%s%s"), *Att.FileName, *Status)));
		FSlateFontInfo Font = GameFont ? FSlateFontInfo(GameFont, 9) : Label->GetFont();
		Font.Size = 9;
		Font.OutlineSettings.OutlineSize = 0;
		Label->SetFont(Font);
		Label->SetColorAndOpacity(FSlateColor(Color));
		Row->AddChildToHorizontalBox(Label);

		if (!Att.bUploading)
		{
			UAIDAIndexButton* Remove = WidgetTree->ConstructWidget<UAIDAIndexButton>();
			Remove->SetClickMethod(EButtonClickMethod::MouseDown);
			Remove->InitIndex(i);
			Remove->OnIndexClickedNative.AddUObject(this, &UAIDAChatWidget::HandleChipRemoveClicked);
			UTextBlock* X = WidgetTree->ConstructWidget<UTextBlock>();
			X->SetText(FText::FromString(TEXT("x")));
			FSlateFontInfo XFont = GameFont ? FSlateFontInfo(GameFont, 8) : X->GetFont();
			XFont.Size = 8;
			XFont.OutlineSettings.OutlineSize = 0;
			X->SetFont(XFont);
			Remove->AddChild(X);
			if (UHorizontalBoxSlot* XSlot = Row->AddChildToHorizontalBox(Remove))
			{
				XSlot->SetPadding(FMargin(4.f, 0.f, 0.f, 0.f));
			}
		}

		if (UHorizontalBoxSlot* ChipSlot = AttachmentChips->AddChildToHorizontalBox(Chip))
		{
			ChipSlot->SetPadding(FMargin(0.f, 0.f, 6.f, 0.f));
		}
	}
}
