#include "Map/AIDAMapService.h"

#include "AIDA.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FGWorldGridSubsystem.h"
#include "Resources/FGResourceNode.h"
#include "Resources/FGResourceDescriptor.h"
#include "Resources/FGItemDescriptor.h"

void FAIDAMapService::ExtractNodesInto(UObject* WorldContext, TArray<FAIDAResourceNode>& OutNodes)
{
	OutNodes.Reset();

	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World)
	{
		UE_LOG(LogAIDA, Warning, TEXT("[map] no world for node extraction."));
		return;
	}

	AFGWorldGridSubsystem* Grid = AFGWorldGridSubsystem::Get(WorldContext);

	for (TActorIterator<AFGResourceNode> It(World); It; ++It)
	{
		AFGResourceNode* Node = *It;
		if (!Node) { continue; }

		FAIDAResourceNode Out;
		const TSubclassOf<UFGResourceDescriptor> ResourceClass = Node->GetResourceClass();
		Out.Resource = ResourceClass ? UFGItemDescriptor::GetItemName(ResourceClass).ToString() : FString(TEXT("Unknown"));
		Out.Purity = Node->GetResourcePurityText().ToString();
		Out.bOccupied = Node->IsOccupied();
		Out.Location = Node->GetActorLocation();
		if (Grid) { Out.Grid = Grid->GetWorldGridCoordinatesForLocation(Out.Location); }
		OutNodes.Add(MoveTemp(Out));
	}

	UE_LOG(LogAIDA, Log, TEXT("[map] scanned %d resource nodes."), OutNodes.Num());
}

const TArray<FAIDAResourceNode>& FAIDAMapService::GetNodes(UObject* WorldContext, double NowSeconds, double TtlSeconds)
{
	if (!bValid || (NowSeconds - LastExtractSeconds) >= TtlSeconds)
	{
		ExtractNodesInto(WorldContext, Cached);
		LastExtractSeconds = NowSeconds;
		bValid = true;
	}
	return Cached;
}
