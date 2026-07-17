#include "AIDA.h"

#include "Net/AIDARemoteCallObject.h"
#include "UI/AIDAChatWidget.h"

#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CoreDelegates.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogAIDA);

#define LOCTEXT_NAMESPACE "FAIDAModule"

namespace
{
	/** The content path to the Blueprint view authored over UAIDAChatWidget (see Mods/AIDA/Content/UI). */
	const TCHAR* GAIDAChatWidgetClassPath = TEXT("/AIDA/UI/WBP_AIDAChat.WBP_AIDAChat_C");

	IConsoleCommand* GShowChatCommand = nullptr;
	IConsoleCommand* GFontsCommand = nullptr;

	/** `AIDA.Fonts` — log every loaded /Game/ font asset, to discover the game's chat font for styling. */
	void DumpFonts(const TArray<FString>& /*Args*/, UWorld* /*World*/)
	{
		int32 Count = 0;
		for (TObjectIterator<UFont> It; It; ++It)
		{
			const UFont* Font = *It;
			if (Font && Font->GetPathName().StartsWith(TEXT("/Game/")))
			{
				UE_LOG(LogAIDA, Log, TEXT("[fonts] %s"), *Font->GetPathName());
				++Count;
			}
		}
		UE_LOG(LogAIDA, Log, TEXT("[fonts] %d loaded /Game/ font(s). (Open the game chat once first if the chat font is missing.)"), Count);
	}

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
	void ShowChatForWorld(UWorld* World, APlayerController* PC, bool bFocusInput)
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

		if (bFocusInput)
		{
			Widget->FocusInput();
		}
	}

	/**
	 * Toggle the WBP_AIDAChat widget in EVERY local game/PIE viewport at once (shared by the
	 * `AIDA.ShowChat` console command and the Ctrl+Enter keybind). Iterating all local world contexts —
	 * rather than the single UWorld the console happens to pass — makes it robust from the editor Cmd
	 * box, a PIE window's console, or the global input pre-processor. In a packaged game there is only
	 * one local world, so it simply toggles that one. bFocusInputOnShow puts the cursor in the input box
	 * (used by the keybind so you can type immediately).
	 */
	void ToggleChatWidgets(bool bFocusInputOnShow)
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
			UE_LOG(LogAIDA, Warning, TEXT("AIDA chat: no local player viewport ready yet."));
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
				ShowChatForWorld(T.Key, T.Value, bFocusInputOnShow);
			}
		}

		UE_LOG(LogAIDA, Log, TEXT("AIDA chat: %s in %d viewport(s)."), bShow ? TEXT("shown") : TEXT("hidden"), Targets.Num());
	}

	/** `AIDA.ShowChat` console command — toggles the window (no auto-focus; the console box has focus). */
	void ShowChatConsole(const TArray<FString>& /*Args*/, UWorld* /*ExecWorld*/)
	{
		ToggleChatWidgets(/*bFocusInputOnShow=*/false);
	}

	/**
	 * Global Slate input pre-processor: Ctrl+Enter toggles the AIDA window from anywhere (even while
	 * playing) and drops the cursor straight into the input box. Enter then submits (the widget's own
	 * OnTextCommitted). Runs ahead of game input, so it consumes the chord.
	 */
	class FAIDAChatInputProcessor : public IInputProcessor
	{
	public:
		virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}
		virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& KeyEvent) override
		{
			if (KeyEvent.GetKey() == EKeys::Enter && KeyEvent.IsControlDown())
			{
				ToggleChatWidgets(/*bFocusInputOnShow=*/true);
				return true; // consume so the game doesn't also act on the chord
			}
			return false;
		}

		/** Plain wheel over the transcript scrolls it; Ctrl+Wheel rotates a pending proposal ghost
		 *  from ANYWHERE. Both are claimed here, ahead of Slate routing: the transcript AND the game
		 *  view are click-through, so the widget's own wheel handler only ever fires over the
		 *  window's few hit-testable parts (live-verify: Ctrl+Wheel rotate "did nothing").
		 *  Captured-mouse play (mouse-look, drags) falls through untouched. */
		virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& WheelEvent, const FPointerEvent* /*GestureEvent*/) override
		{
			if (WheelEvent.GetWheelDelta() == 0.f || SlateApp.HasAnyMouseCaptor())
			{
				return false;
			}
			if (WheelEvent.IsControlDown())
			{
				// Only consumed while a pending proposal exists — otherwise the game keeps the
				// chord. 90° per notch; Shift = 15°, Alt = 1° for fine alignment.
				const int32 Step = WheelEvent.IsAltDown() ? 1 : (WheelEvent.IsShiftDown() ? 15 : 90);
				for (const auto& Pair : ShownChatWidgets())
				{
					UAIDAChatWidget* Widget = Pair.Value.Get();
					if (Widget && Widget->IsInViewport() &&
						Widget->TryRotatePendingProposal(WheelEvent.GetWheelDelta() > 0.f ? Step : -Step))
					{
						return true; // consume — the hotbar must not also spin
					}
				}
				return false;
			}
			for (const auto& Pair : ShownChatWidgets())
			{
				UAIDAChatWidget* Widget = Pair.Value.Get();
				if (Widget && Widget->IsInViewport() &&
					Widget->IsScreenPositionOverTranscript(WheelEvent.GetScreenSpacePosition()))
				{
					Widget->ScrollTranscriptBy(WheelEvent.GetWheelDelta() * -60.f);
					return true; // consume — the hotbar must not also spin
				}
			}
			return false;
		}
		virtual const TCHAR* GetDebugName() const override { return TEXT("AIDAChatInput"); }
	};

	TSharedPtr<FAIDAChatInputProcessor> GChatInputProcessor;

	/** Register the Ctrl+Enter pre-processor once Slate is available (idempotent). */
	void RegisterChatInputProcessor()
	{
		if (!GChatInputProcessor.IsValid() && FSlateApplication::IsInitialized())
		{
			GChatInputProcessor = MakeShared<FAIDAChatInputProcessor>();
			FSlateApplication::Get().RegisterInputPreProcessor(GChatInputProcessor);
		}
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
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ShowChatConsole),
		ECVF_Default);

	GFontsCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.Fonts"),
		TEXT("Log loaded /Game/ font assets (to find the game chat font). Usage: AIDA.Fonts"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DumpFonts),
		ECVF_Default);

	// Ctrl+Enter opens/closes the AIDA window from anywhere. Slate may not be up yet at module load,
	// so register now if it is, otherwise once the engine finishes initializing.
	if (FSlateApplication::IsInitialized())
	{
		RegisterChatInputProcessor();
	}
	else
	{
		FCoreDelegates::OnPostEngineInit.AddStatic(&RegisterChatInputProcessor);
	}
}

void FAIDAModule::ShutdownModule()
{
	if (GShowChatCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GShowChatCommand);
		GShowChatCommand = nullptr;
	}
	if (GFontsCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GFontsCommand);
		GFontsCommand = nullptr;
	}

	if (GChatInputProcessor.IsValid())
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(GChatInputProcessor);
		}
		GChatInputProcessor.Reset();
	}

	UAIDARemoteCallObject::UnregisterHooks();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAIDAModule, AIDA)
