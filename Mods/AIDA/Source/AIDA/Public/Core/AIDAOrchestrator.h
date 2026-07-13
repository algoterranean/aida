#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/AIDAConfig.h"
#include "AIDAOrchestrator.generated.h"

/**
 * Server-authoritative orchestrator. Created with the world and safe with zero players
 * connected (never initialized per-player). The only network-egress authority.
 *
 * Phase 0: loads + validates config on world begin play and logs its status.
 * Later phases wire in SessionManager, LLMClient, ToolRegistry, etc. (docs/ARCHITECTURE.md §3.2).
 */
UCLASS()
class UAIDAOrchestrator : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ UWorldSubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	//~ End UWorldSubsystem

	const FAIDAConfig& GetConfig() const { return Config; }
	bool IsConfigLoaded() const { return bConfigLoaded; }

private:
	void LoadConfig();

	/** Resolves the config path: server admin override first, then the mod's shipped example. */
	FString ResolveConfigPath(FString& OutSource) const;

	FAIDAConfig Config;
	bool bConfigLoaded = false;
};
