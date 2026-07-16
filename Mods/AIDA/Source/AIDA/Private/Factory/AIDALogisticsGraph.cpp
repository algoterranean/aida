#include "Factory/AIDALogisticsGraph.h"

TArray<FAIDAConveyorEdge> AIDALogisticsGraph::CollapseChains(const TArray<FSegment>& Segments)
{
	TMap<int32, const FSegment*> ById;
	for (const FSegment& S : Segments)
	{
		if (S.SegmentId > 0) { ById.Add(S.SegmentId, &S); }
	}

	TArray<FAIDAConveyorEdge> Edges;
	for (const FSegment& Head : Segments)
	{
		if (Head.FromSegment != 0 && ById.Contains(Head.FromSegment)) { continue; } // mid-chain; its head walks it

		FAIDAConveyorEdge Edge;
		Edge.FromMachine = Head.FromNode;
		Edge.bPipe = Head.bPipe;
		Edge.PerMinute = Head.PerMinute;
		Edge.Item = Head.Item;

		// Walk downstream to the chain's end, tracking the slowest segment.
		TSet<int32> Visited;
		const FSegment* Current = &Head;
		bool bCycle = false;
		while (true)
		{
			Visited.Add(Current->SegmentId);
			Edge.PerMinute = FMath::Min(Edge.PerMinute, Current->PerMinute);
			if (Edge.Item.IsEmpty()) { Edge.Item = Current->Item; }
			if (Current->ToSegment == 0) { break; }
			const FSegment* const* Next = ById.Find(Current->ToSegment);
			if (!Next) { break; }
			if (Visited.Contains((*Next)->SegmentId)) { bCycle = true; break; }
			Current = *Next;
		}
		if (bCycle) { continue; } // a belt loop has no node endpoints worth reporting

		Edge.ToMachine = Current->ToNode;
		if (Edge.FromMachine == 0 && Edge.ToMachine == 0) { continue; } // floats free; nothing to anchor it to
		Edges.Add(MoveTemp(Edge));
	}
	return Edges;
}

void AIDALogisticsGraph::AttributeItems(TArray<FAIDAConveyorEdge>& Edges, const TArray<FAIDAMachine>& Machines)
{
	TMap<int32, const FAIDAMachine*> ById;
	for (const FAIDAMachine& M : Machines) { ById.Add(M.Id, &M); }

	for (FAIDAConveyorEdge& Edge : Edges)
	{
		if (!Edge.Item.IsEmpty() || Edge.FromMachine == 0) { continue; }
		const FAIDAMachine* const* Source = ById.Find(Edge.FromMachine);
		if (!Source || (*Source)->bLogisticsOnly) { continue; }

		const FAIDAItemRate* Best = nullptr;
		for (const FAIDAItemRate& R : (*Source)->Outputs)
		{
			if (!Best || R.PerMinute > Best->PerMinute) { Best = &R; }
		}
		if (Best) { Edge.Item = Best->Item; }
	}
}
