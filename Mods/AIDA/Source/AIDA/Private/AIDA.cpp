#include "AIDA.h"

#include "Net/AIDARemoteCallObject.h"
#include "UI/AIDAChatWidget.h"

#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogAIDA);

#define LOCTEXT_NAMESPACE "FAIDAModule"

namespace
{
	/** The content path to the Blueprint view authored over UAIDAChatWidget (see Mods/AIDA/Content/UI). */
	const TCHAR* GAIDAChatWidgetClassPath = TEXT("/AIDA/UI/WBP_AIDAChat.WBP_AIDAChat_C");

	IConsoleCommand* GShowChatCommand = nullptr;

	/**
	 * The widget currently shown by AIDA.ShowChat, keyed by world so a second invocation can toggle it
	 * off. Keyed by world (not a single global) because PIE runs the listen-server and client worlds in
	 * ONE process: a lone global would be shared across both PIE windows, so toggling in one window would
	 * hide the other window's widget and both could never be visible at once. Each world toggles its own.
	 */
	TMap<TWeakObjectPtr<UWorld>, TWeakObjectPtr<UAIDAChatWidget>>& ShownChatWidgets()
	{
		static TMap<TWeakObjectPtr<UWorld>, TWeakObjectPtr<UAIDAChatWidget>> Map;
		return Map;
	}

	/** Hide + forget any widget currently shown for World. Returns true if one was removed. */
	bool HideChatForWorld(UWorld* World)
	{
		TWeakObjectPtr<UAIDAChatWidget> Tracked;
		if (!ShownChatWidgets().RemoveAndCopyValue(World, Tracked))
		{
			return false;
		}
		if (UAIDAChatWidget* Existing = Tracked.Get())
		{
			APlayerController* PC = Existing->GetOwningPlayer();
			Existing->RemoveFromParent();
			if (PC)
			{
				PC->SetInputMode(FInputModeGameOnly());
				PC->bShowMouseCursor = false;
			}
			return true;
		}
		return false; // entry was stale (widget already GC'd)
	}

	/** Create + show the chat widget in PC's viewport, tracked under World. */
	void ShowChatForWorld(UWorld* World, APlayerController* PC)
	{
		UClass* WidgetClass = LoadClass<UAIDAChatWidget>(nullptr, GAIDAChatWidgetClassPath);
		if (!WidgetClass)
		{
			UE_LOG(LogAIDA, Error, TEXT("AIDA.ShowChat: could not load widget class '%s'. Is WBP_AIDAChat authored under /AIDA/UI?"), GAIDAChatWidgetClassPath);
			return;
		}

		UAIDAChatWidget* Widget = CreateWidget<UAIDAChatWidget>(PC, WidgetClass);
		if (!Widget)
		{
			UE_LOG(LogAIDA, Error, TEXT("AIDA.ShowChat: CreateWidget failed."));
			return;
		}

		Widget->AddToViewport();
		ShownChatWidgets().Add(World, Widget);

		// Let the player click into the chat box while the game keeps running underneath.
		PC->SetInputMode(FInputModeGameAndUI());
		PC->bShowMouseCursor = true;
	}

	/**
	 * `AIDA.ShowChat` — client-side UI command. Toggles the WBP_AIDAChat widget in EVERY local
	 * game/PIE viewport at once. Registered from module startup (not the orchestrator, which is idle
	 * on client worlds) so any client can open it. Iterating all local world contexts — rather than
	 * relying on the single UWorld the console happens to pass — makes it robust from either the
	 * editor's Cmd box or a PIE window's own console, and in a 2-window listen-server PIE it opens
	 * the widget in both the host and client windows from one invocation. In a packaged game there is
	 * only one local world, so it simply toggles that one.
	 */
	void ShowChat(const TArray<FString>& Args, UWorld* /*ExecWorld*/)
	{
		if (!GEngine)
		{
			return;
		}

		// Collect local game/PIE worlds that actually have a local player controller.
		TArray<TPair<UWorld*, APlayerController*>> Targets;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType != EWorldType::PIE && Ctx.WorldType != EWorldType::Game)
			{
				continue;
			}
			UWorld* W = Ctx.World();
			if (!W)
			{
				continue;
			}
			if (APlayerController* PC = GEngine->GetFirstLocalPlayerController(W))
			{
				Targets.Emplace(W, PC);
			}
		}

		if (Targets.Num() == 0)
		{
			UE_LOG(LogAIDA, Warning, TEXT("AIDA.ShowChat: no local player viewport ready yet (start PIE first)."));
			return;
		}

		// Keep the windows in sync: if any is currently shown, hide all; otherwise show all.
		bool bAnyShown = false;
		for (const TPair<UWorld*, APlayerController*>& T : Targets)
		{
			if (const TWeakObjectPtr<UAIDAChatWidget>* W = ShownChatWidgets().Find(T.Key); W && W->IsValid())
			{
				bAnyShown = true;
				break;
			}
		}

		const bool bShow = !bAnyShown;
		for (const TPair<UWorld*, APlayerController*>& T : Targets)
		{
			HideChatForWorld(T.Key); // clear any existing first (also resets input mode)
			if (bShow)
			{
				ShowChatForWorld(T.Key, T.Value);
			}
		}

		UE_LOG(LogAIDA, Log, TEXT("AIDA.ShowChat: %s in %d viewport(s)."), bShow ? TEXT("shown") : TEXT("hidden"), Targets.Num());
	}
}

void FAIDAModule::StartupModule()
{
	// Module load point. The AIDAOrchestrator is initialized as a world subsystem
	// (see Core/AIDAOrchestrator), never here — it must be safe with zero players connected.
	UE_LOG(LogAIDA, Log, TEXT("AIDA module loaded."));

	// Register the per-player RCO on every Satisfactory game mode as it initializes.
	UAIDARemoteCallObject::RegisterHooks();

	// Client-side UI command — lives here (not the orchestrator) so it exists on client worlds too.
	GShowChatCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.ShowChat"),
		TEXT("Toggle the AIDA chat widget in the local player's viewport. Usage: AIDA.ShowChat"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ShowChat),
		ECVF_Default);
}

void FAIDAModule::ShutdownModule()
{
	if (GShowChatCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GShowChatCommand);
		GShowChatCommand = nullptr;
	}

	UAIDARemoteCallObject::UnregisterHooks();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAIDAModule, AIDA)
