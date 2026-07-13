#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Net/AIDANetTypes.h"
#include "AIDAChatWidget.generated.h"

class AAIDAChatRelay;

/**
 * Client-only base for the ChatWidget (docs/ARCHITECTURE.md §3.1). Owns all the wiring — finds the
 * replicated relay, binds its Begin/Chunk/End delegates, drives late-join sync, and sends chat —
 * and hands the Blueprint subclass a pure view via BlueprintImplementableEvents. No business logic
 * lives in the BP (coding rule: "No business logic in ChatWidget").
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
};
