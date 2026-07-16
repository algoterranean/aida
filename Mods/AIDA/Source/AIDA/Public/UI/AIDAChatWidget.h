#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Types/SlateEnums.h"
#include "Net/AIDANetTypes.h"
#include "AIDAChatWidget.generated.h"

class AAIDAChatRelay;
class AAIDAProposalRelay;
class UBorder;
class UEditableTextBox;
class UHorizontalBox;
class URichTextBlock;
class UScrollBox;
class UTextBlock;

/** Broadcast whenever the rendered transcript string changes (for optional BP text bindings). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAIDATranscriptChanged, const FString&, Transcript);

/** A tab button that carries its conversation id and reports clicks via a native delegate. */
UCLASS()
class UAIDATabButton : public UButton
{
	GENERATED_BODY()

public:
	FGuid ConversationId;

	DECLARE_MULTICAST_DELEGATE_OneParam(FAIDAOnTabClicked, const FGuid&);
	FAIDAOnTabClicked OnTabClickedNative;

	/** Bind the conversation this tab selects. */
	void InitTab(const FGuid& InConv)
	{
		ConversationId = InConv;
		if (!OnClicked.IsAlreadyBound(this, &UAIDATabButton::Forward))
		{
			OnClicked.AddDynamic(this, &UAIDATabButton::Forward);
		}
	}

private:
	UFUNCTION()
	void Forward() { OnTabClickedNative.Broadcast(ConversationId); }
};

/**
 * Client-only base for the ChatWidget (docs/ARCHITECTURE.md §3.1). Owns all the wiring — finds the
 * replicated relay, binds its Begin/Chunk/End delegates, drives late-join sync, and sends chat —
 * and hands the Blueprint subclass a pure view via BlueprintImplementableEvents. No business logic
 * lives in the BP (coding rule: "No business logic in ChatWidget").
 *
 * To keep the Blueprint a pure layout with ZERO event-graph nodes, this base also self-wires a set of
 * optional, name-matched sub-widgets (BindWidgetOptional). If the BP contains widgets named InputBox /
 * SendButton / TranscriptText / TranscriptScroll, the base drives them directly: the Send button and
 * Enter submit chat, and the transcript is rendered natively into TranscriptText. A BP that wants full
 * control can instead ignore those names and implement the OnMessage* events itself.
 *
 * A UUserWidget is inherently client-side, so local-player/UI access here is allowed (rule 2).
 */
UCLASS(Abstract)
class UAIDAChatWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	/** Up/Down recall the current conversation's input history while the input box has focus.
	 *  Ctrl+Arrows / Ctrl+PgUp/PgDn nudge a pending proposal ghost (Shift = 1 m fine steps). */
	virtual FReply NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	/** Ctrl+Wheel rotates a pending proposal ghost by 90° per notch. */
	virtual FReply NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

	/** Send a chat line to the server (bind your text box's submit to this). */
	UFUNCTION(BlueprintCallable, Category = "AIDA")
	void SendChat(const FString& Text);

	/** The current assembled transcript (e.g. to rebuild the list on open). */
	UFUNCTION(BlueprintCallable, Category = "AIDA")
	void GetTranscript(TArray<FAIDATranscriptEntry>& OutEntries) const;

	/** Put keyboard focus into the input box (used when the window is opened via the Ctrl+Enter keybind). */
	void FocusInput();

	/** The whole transcript rendered to a single display string ("Author: body" blocks). */
	UPROPERTY(BlueprintReadOnly, Category = "AIDA")
	FString RenderedTranscript;

	/** Fires whenever RenderedTranscript changes (native default view + optional BP text binding). */
	UPROPERTY(BlueprintAssignable, Category = "AIDA")
	FOnAIDATranscriptChanged OnTranscriptChanged;

	//~ View hooks — implement in the Blueprint subclass to render.
	/** A message opened (author + kind known; body may still stream). */
	UFUNCTION(BlueprintImplementableEvent, Category = "AIDA")
	void OnMessageBegin(const FAIDAMessageHeader& Header);

	/** A body fragment arrived for Id (append it to that message's view). */
	UFUNCTION(BlueprintImplementableEvent, Category = "AIDA")
	void OnMessageDelta(const FGuid& Id, const FString& Delta);

	/** A message completed (finalize its view; safe to stop the typing indicator). */
	UFUNCTION(BlueprintImplementableEvent, Category = "AIDA")
	void OnMessageEnd(const FGuid& Id);

protected:
	//~ Optional, name-matched sub-widgets. Present them in the BP (matching names) to get a working
	//~ view with no event-graph logic; leave them out to render entirely from the OnMessage* events.
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "AIDA")
	TObjectPtr<UEditableTextBox> InputBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "AIDA")
	TObjectPtr<UButton> SendButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "AIDA")
	TObjectPtr<UTextBlock> TranscriptText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "AIDA")
	TObjectPtr<UScrollBox> TranscriptScroll;

private:
	/** The conversation/tab this widget is currently sending to (Phase C will switch this per tab). */
	FGuid CurrentConversationId = AIDADefaultConversationId();

	/** Rich-text transcript, constructed in C++ under TranscriptScroll to render AIDA's markdown. */
	UPROPERTY(Transient)
	TObjectPtr<URichTextBlock> TranscriptRich;

	/** Build a transient rich-text style set (Default/Bold/Italic/Code/Header) from the given font. */
	void BuildTranscriptRich(class UFont* GameFont, float FontSize);

	UPROPERTY(Transient)
	TWeakObjectPtr<AAIDAChatRelay> Relay;

	FTimerHandle BindRetryTimer;

	/** Locate the relay and bind its delegates. Returns true once bound. */
	bool TryBindRelay();
	void UnbindRelay();

	UFUNCTION()
	void HandleMsgBegin(const FAIDAMessageHeader& Header);
	UFUNCTION()
	void HandleMsgChunk(const FGuid& Id, const FString& Delta);
	UFUNCTION()
	void HandleMsgEnd(const FGuid& Id);

	//~ Self-wiring for the optional sub-widgets.
	UFUNCTION()
	void HandleSendClicked();
	UFUNCTION()
	void HandleInputCommitted(const FText& Text, ETextCommit::Type CommitMethod);

	//~ Per-conversation input history (Up = older, Down = newer, past-newest = empty line).
	TMap<FGuid, TArray<FString>> InputHistory;
	int32 HistoryCursor = INDEX_NONE; // into the CURRENT conversation's history; INDEX_NONE = live line

	/** Append a sent line to the current conversation's history (deduping repeats) and reset recall. */
	void RecordInputHistory(const FString& Text);

	/** Refocus the input box now AND next tick — Slate steals focus after commits/button clicks. */
	void RefocusInput();

	/**
	 * Ghost-adjust keybinds: translate a Ctrl+key/wheel gesture into a proposal adjustment and send
	 * it via the relay. Arrow directions are camera-relative, snapped to the world lattice. Returns
	 * false when there is no pending proposal (the input falls through to its normal meaning).
	 */
	bool TryAdjustGhost(const FKey& Key, bool bFineStep);

	//~ Per-conversation transcript state (one per tab). Each conversation is independent.
	struct FRenderedMessage
	{
		FGuid Id;
		FString Prefix;
		FString Body;
		bool bSystem = false; // System lines render dimmed + markdown-sanitized (no nested tags)
	};
	struct FConversationView
	{
		TArray<FRenderedMessage> Messages;
		TMap<FGuid, int32> IndexById;
	};
	TMap<FGuid, FConversationView> Conversations; // conversation id -> its ordered messages
	TMap<FGuid, FGuid> ConversationOfMessage;     // message id -> conversation id (routes Chunk/End)
	TArray<FGuid> TabOrder;                        // stable left-to-right tab order

	/** Ensure a conversation (and its tab) exists; returns its view. New conversations rebuild the tab bar. */
	FConversationView& EnsureConversation(const FGuid& ConvId);
	/** Make ConvId the active tab and re-render its transcript. */
	void SwitchToConversation(const FGuid& ConvId);
	/** Render the active conversation's transcript (markdown) into TranscriptRich. */
	void RenderActiveConversation();

	//~ Tab bar (constructed in C++; a button per conversation plus a '+' new-tab button).
	UPROPERTY(Transient)
	TObjectPtr<UHorizontalBox> TabBar;

	void RebuildTabBar();
	void HandleTabClicked(const FGuid& ConvId);
	UFUNCTION()
	void HandleNewTabClicked();

	//~ Proposal panel (Phase 4 Slice 3) — constructed in C++ like the tab bar, so the BP needs no
	//~ changes. Shows the first pending proposal (summary + cost + Approve/Reject); collapsed when
	//~ none is pending. Buttons are cosmetic — the server enforces both approval gates.
	UPROPERTY(Transient)
	TWeakObjectPtr<AAIDAProposalRelay> ProposalRelay;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> ProposalPanel;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> ProposalText;

	UPROPERTY(Transient)
	TObjectPtr<UButton> ApproveButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> RejectButton;

	/** The proposal the panel currently shows (what Approve/Reject act on). */
	FGuid ShownProposalId;

	/** Build the (initially collapsed) proposal card into the root canvas. */
	void BuildProposalPanel(UFont* GameFont, float FontSize);

	/** Locate the proposal relay and bind its change delegate (shares the chat relay's retry timer). */
	bool TryBindProposalRelay();

	UFUNCTION()
	void HandleProposalsChanged();
	UFUNCTION()
	void HandleApproveClicked();
	UFUNCTION()
	void HandleRejectClicked();
};
