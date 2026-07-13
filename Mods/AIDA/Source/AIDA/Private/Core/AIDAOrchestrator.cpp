#include "Core/AIDAOrchestrator.h"

#include "AIDA.h"
#include "Core/AIDAConfigLoader.h"
#include "Adapters/LLMClient.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"

bool UAIDAOrchestrator::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Only real game worlds (dedicated/listen server, standalone, PIE) — not editor/preview worlds.
	if (const UWorld* World = Cast<UWorld>(Outer))
	{
		return World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE;
	}
	return false;
}

void UAIDAOrchestrator::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// The orchestrator is the only network-egress authority. Clients hold no config and no key.
	if (InWorld.GetNetMode() == NM_Client)
	{
		UE_LOG(LogAIDA, Verbose, TEXT("Orchestrator idle on client world."));
		return;
	}

	UE_LOG(LogAIDA, Log, TEXT("AIDA orchestrator starting (netmode=%d)."), static_cast<int32>(InWorld.GetNetMode()));
	LoadConfig();
	RegisterConsoleCommands();
}

void UAIDAOrchestrator::Deinitialize()
{
	if (PingCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(PingCommand);
		PingCommand = nullptr;
	}
	LLMClient.Reset();

	Super::Deinitialize();
}

void UAIDAOrchestrator::RegisterConsoleCommands()
{
	if (PingCommand)
	{
		return; // already registered (OnWorldBeginPlay can fire per-world)
	}

	PingCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.Ping"),
		TEXT("Send a one-shot test prompt to the configured LLM and log the reply. Usage: AIDA.Ping [prompt...]"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::Ping),
		ECVF_Default);
}

void UAIDAOrchestrator::Ping(const TArray<FString>& Args)
{
	if (!LLMClient.IsValid() || !LLMClient->IsReady())
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.Ping: LLM client not ready (config not loaded, or provider not implemented)."));
		return;
	}

	const FString Prompt = Args.Num() > 0 ? FString::Join(Args, TEXT(" ")) : TEXT("Reply with a short, friendly hello.");
	UE_LOG(LogAIDA, Log, TEXT("AIDA.Ping -> \"%s\""), *Prompt);

	LLMClient->Complete(Prompt,
		[](const FString& Text)
		{
			UE_LOG(LogAIDA, Log, TEXT("AIDA reply: %s"), *Text);
		},
		[](int32 Status, const FString& Message)
		{
			UE_LOG(LogAIDA, Error, TEXT("AIDA.Ping failed (HTTP %d): %s"), Status, *Message);
		});
}

FString UAIDAOrchestrator::ResolveConfigPath(FString& OutSource) const
{
	// 1) Server admin override: <ProjectDir>/Configs/AIDA/config.jsonc
	const FString Override = FPaths::Combine(FPaths::ProjectDir(), TEXT("Configs"), TEXT("AIDA"), TEXT("config.jsonc"));
	if (FPaths::FileExists(Override))
	{
		OutSource = TEXT("admin override");
		return Override;
	}

	// 2) Fall back to the mod's shipped example: <PluginDir>/Configs/config.example.jsonc
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AIDA")))
	{
		OutSource = TEXT("shipped example");
		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Configs"), TEXT("config.example.jsonc"));
	}

	OutSource = TEXT("unresolved");
	return Override;
}

void UAIDAOrchestrator::LoadConfig()
{
	FString Source;
	const FString Path = ResolveConfigPath(Source);

	FString Error;
	if (FAIDAConfigLoader::LoadFromFile(Path, Config, Error))
	{
		bConfigLoaded = true;
		// NEVER log the API key — only whether one is present.
		UE_LOG(LogAIDA, Log,
			TEXT("Config loaded (%s): provider=%s baseUrl=%s model=%s apiKey=%s"),
			*Source,
			*Config.Provider.Type, *Config.Provider.BaseUrl, *Config.Provider.Model,
			Config.Provider.ApiKey.IsEmpty() ? TEXT("<none>") : TEXT("<set:redacted>"));

		LLMClient = MakeShared<FLLMClient>(Config);
		UE_LOG(LogAIDA, Log, TEXT("LLM client %s. Run console command 'AIDA.Ping' to test a round-trip."),
			LLMClient->IsReady() ? TEXT("ready") : TEXT("NOT ready (provider unimplemented)"));
	}
	else
	{
		bConfigLoaded = false;
		UE_LOG(LogAIDA, Warning,
			TEXT("Config not loaded (%s). AIDA will stay idle until configured. [tried: %s]"),
			*Error, *Path);
	}
}
