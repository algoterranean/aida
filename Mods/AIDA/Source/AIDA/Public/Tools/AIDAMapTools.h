#pragma once

#include "CoreMinimal.h"
#include "Map/AIDAMapModel.h"

/**
 * Pure serializer for the Slice 3 map tool (docs/ARCHITECTURE.md §4). Turns a resource-node list into
 * bounded, LLM-friendly JSON: counts grouped by resource + purity (total vs free), and — when asked for
 * only untapped nodes — a capped list of free ones with their world-grid cell. No game headers, so it
 * unit-tests on synthetic nodes; the orchestrator wires it into a tool over the live AIDAMapService scan.
 */
namespace AIDAMapTools
{
	/**
	 * get_resource_nodes: grouped node summary. ResourceFilter (optional, case-insensitive substring)
	 * narrows to one resource; bUntappedOnly restricts counts to unoccupied nodes and adds a bounded
	 * free-node list with grid coordinates.
	 */
	FString BuildResourceNodesJson(const TArray<FAIDAResourceNode>& Nodes, const FString& ResourceFilter, bool bUntappedOnly);

	/**
	 * tag_node target resolution: the untapped node whose resource matches ResourceFilter (and, if
	 * PurityFilter is non-empty, its purity) — the nearest to From when bHasFrom, else the first match.
	 * Returns nullptr when nothing matches. Pure, so it unit-tests on synthetic nodes.
	 */
	const FAIDAResourceNode* FindNearestUntapped(const TArray<FAIDAResourceNode>& Nodes, const FString& ResourceFilter,
		const FString& PurityFilter, const FVector& From, bool bHasFrom);
}
