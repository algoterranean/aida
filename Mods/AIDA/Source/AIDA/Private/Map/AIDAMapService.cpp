#include "Map/AIDAMapService.h"

#include "AIDA.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "FGMapArea.h"
#include "FGMapAreaTexture.h"
#include "FGMapManager.h"
#include "FGMapMarker.h"
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

bool FAIDAMapService::PlaceMapMarker(UObject* WorldContext, const FVector& WorldLocation, const FString& Label)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	AFGMapManager* MapManager = World ? AFGMapManager::Get(World) : nullptr;
	if (!MapManager)
	{
		UE_LOG(LogAIDA, Warning, TEXT("[map] no map manager for marker placement."));
		return false;
	}
	if (!MapManager->CanAddNewMapMarker())
	{
		UE_LOG(LogAIDA, Warning, TEXT("[map] map-marker limit reached; not placing '%s'."), *Label);
		return false;
	}

	FMapMarker Marker;
	Marker.Location = WorldLocation;
	Marker.Name = Label;
	Marker.MapMarkerType = ERepresentationType::RT_MapMarker;
	Marker.Color = FLinearColor(1.0f, 0.65f, 0.0f, 1.0f); // AIDA amber
	Marker.Scale = 1.0f;
	Marker.CompassViewDistance = ECompassViewDistance::CVD_Always;

	FMapMarker Created;
	const bool bOk = MapManager->AddNewMapMarker(Marker, Created);
	UE_LOG(LogAIDA, Log, TEXT("[map] tag_node marker '%s' at %s -> %s"), *Label, *WorldLocation.ToCompactString(), bOk ? TEXT("ok") : TEXT("rejected"));
	return bOk;
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

bool FAIDAMapService::SpawnAttentionPing(UObject* WorldContext, const FString& PlayerId, const FVector& WorldLocation)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World) { return false; }

	// Same identity convention as everywhere else: the listen host's net id is null, so an empty
	// requester id matches the controller whose net id string is also empty.
	APlayerController* Requester = nullptr;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		APlayerState* PS = PC ? PC->PlayerState : nullptr;
		if (!PS) { continue; }
		const TSharedPtr<const FUniqueNetId> NetId = PS->GetUniqueId().GetUniqueNetId();
		const FString Id = NetId.IsValid() ? NetId->ToString() : FString();
		if (Id == PlayerId)
		{
			Requester = PC;
			break;
		}
	}
	if (!Requester)
	{
		// Any player's ping beats no ping — the marker is cosmetic and shared.
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (It->Get()) { Requester = It->Get(); break; }
		}
	}
	if (!Requester) { return false; }

	// Server_SpawnAttentionPingActor(FVector, FVector) is private C++ but a UFUNCTION — invoke it by
	// reflection. Called on the server, a Server RPC executes its implementation directly.
	UFunction* Fn = Requester->FindFunction(FName(TEXT("Server_SpawnAttentionPingActor")));
	if (!Fn) { return false; }
	struct { FVector Location; FVector Normal; } Params{ WorldLocation, FVector::UpVector };
	if (Fn->ParmsSize != sizeof(Params))
	{
		UE_LOG(LogAIDA, Warning, TEXT("[map] attention-ping RPC signature drifted (parms %d != %d) — ping skipped."),
			Fn->ParmsSize, static_cast<int32>(sizeof(Params)));
		return false;
	}
	Requester->ProcessEvent(Fn, &Params);
	return true;
}
