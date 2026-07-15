#include "UI/AIDAChatWidget.h"

#include "AIDA.h"
#include "Net/AIDAChatRelay.h"
#include "Subsystem/SubsystemActorManager.h"
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

	// Create the default conversation tab and render it (more tabs appear as conversations arrive).
	EnsureConversation(CurrentConversationId);
	RenderActiveConversation();

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
	InputBox->SetText(FText::GetEmpty());
}

void UAIDAChatWidget::HandleInputCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	if (CommitMethod == ETextCommit::OnEnter)
	{
		HandleSendClicked();
	}
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
