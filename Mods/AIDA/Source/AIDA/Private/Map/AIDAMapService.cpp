#include "Map/AIDAMapService.h"

#include "AIDA.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FGWorldGridSubsystem.h"
#include "Resources/FGResourceNode.h"
#include "Resources/FGResourceDescriptor.h"
#include "Resources/FGItemDescriptor.h"

namespace
{
	/** Clean purity label from the enum (GetResourcePurityText carries rich-text markup like "<Bold>(Pure)</>"). */
	FString PurityName(EResourcePurity Purity)
	{
		switch (Purity)
		{
		case EResourcePurity::RP_Inpure: return TEXT("Impure");
		case EResourcePurity::RP_Normal: return TEXT("Normal");
		case EResourcePurity::RP_Pure:   return TEXT("Pure");
		default:                         return TEXT("Unknown");
		}
	}
}

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
		Out.Purity = PurityName(Node->GetResourcePurity());
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
