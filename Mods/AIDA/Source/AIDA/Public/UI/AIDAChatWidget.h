#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Types/SlateEnums.h"
#include "Net/AIDANetTypes.h"
#include "AIDAChatWidget.generated.h"

class AAIDAChatRelay;
class UButton;
class UEditableTextBox;
class UScrollBox;
class UTextBlock;

/** Broadcast whenever the rendered transcript string changes (for optional BP text bindings). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAIDATranscriptChanged, const FString&, Transcript);

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

	//~ Native default rendering: one string assembled from ordered messages, pushed to TranscriptText.
	struct FRenderedMessage
	{
		FGuid Id;
		FString Prefix;
		FString Body;
	};
	TArray<FRenderedMessage> RenderedMessages;
	TMap<FGuid, int32> RenderedIndexById;
	void RebuildRenderedTranscript();
};
