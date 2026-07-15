#include "Map/AIDAMapService.h"

#include "AIDA.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FGMapArea.h"
#include "FGMapAreaTexture.h"
#include "FGMinimapCaptureActor.h"
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

FString FAIDAMapService::RegionNameForLocation(UObject* WorldContext, const FVector& WorldLocation)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World) { return FString(); }

	// The area lookup lives on the minimap capture actor's area texture; it may be absent on a
	// dedicated server or before the minimap is built. Callers fall back to grid coordinates.
	for (TActorIterator<AFGMinimapCaptureActor> It(World); It; ++It)
	{
		if (UFGMapAreaTexture* AreaTexture = It->GetMapAreaTexture())
		{
			const TSubclassOf<UFGMapArea> Area = AreaTexture->GetMapAreaForWorldLocation(WorldLocation);
			if (!Area) { return FString(); }
			FString Name = UFGMapArea::GetUserSetAreaDisplayName(Area).ToString();
			if (Name.IsEmpty()) { Name = UFGMapArea::GetAreaDisplayName(Area).ToString(); }
			return Name;
		}
	}
	return FString();
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

	// Resolve the area texture once and reuse it for every node (rather than iterating actors per node).
	UFGMapAreaTexture* AreaTexture = nullptr;
	for (TActorIterator<AFGMinimapCaptureActor> It(World); It; ++It)
	{
		AreaTexture = It->GetMapAreaTexture();
		if (AreaTexture) { break; }
	}

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
		if (AreaTexture)
		{
			if (const TSubclassOf<UFGMapArea> Area = AreaTexture->GetMapAreaForWorldLocation(Out.Location))
			{
				Out.Region = UFGMapArea::GetUserSetAreaDisplayName(Area).ToString();
				if (Out.Region.IsEmpty()) { Out.Region = UFGMapArea::GetAreaDisplayName(Area).ToString(); }
			}
		}
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
