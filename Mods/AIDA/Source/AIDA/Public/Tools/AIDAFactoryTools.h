#pragma once

#include "CoreMinimal.h"
#include "Factory/AIDAFactoryModel.h"

/**
 * Pure serializers for the Phase 2 Slice 2 factory tools (docs/ARCHITECTURE.md §4). Each turns the
 * plain aggregates into bounded, LLM-friendly JSON (coordinates humanized to metres, lists capped with
 * a "N more" hint). No game headers and no registry dependency, so they unit-test on synthetic
 * aggregates; the orchestrator wires them into FAIDAToolRegistry handlers that supply a live snapshot.
 */
namespace AIDAFactoryTools
{
	/** get_factory_overview: cluster census summary, biggest deficits, and the power picture. */
	FString BuildOverviewJson(const FAIDAFactoryAggregates& Aggregates);

	/**
	 * get_item_balance: whole-factory per-item net production/consumption. With ItemFilter set, returns
	 * just that item (or an {"error"} object if unknown); otherwise the biggest deficits first, bounded.
	 */
	FString BuildItemBalanceJson(const FAIDAFactoryAggregates& Aggregates, const FString& ItemFilter);

	/** inspect_cluster: one cluster's census, net flows, efficiency, and (bounded) machine ids. */
	FString BuildClusterJson(const FAIDAFactoryAggregates& Aggregates, int32 ClusterId);

	/** find_bottleneck: a status, a plain-language explanation, and the supporting numbers. */
	FString BuildBottleneckJson(const FAIDABottleneckResult& Result);
}
