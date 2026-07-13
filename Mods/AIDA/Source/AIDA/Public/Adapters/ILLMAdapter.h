#pragma once

#include "CoreMinimal.h"
#include "Adapters/AIDALLMTypes.h"

/**
 * One provider's translation to/from the common completion interface.
 *
 * Complete() is async and non-blocking. The reply is streamed: OnChunk fires zero or more times
 * with incremental text deltas, then exactly one of OnComplete/OnError fires. Every callback is
 * delivered on the game thread (the underlying HTTP body arrives on a worker thread and adapters
 * marshal it across). OnChunk may be unset when the caller only wants the final assembled text.
 */
class ILLMAdapter
{
public:
	virtual ~ILLMAdapter() = default;

	virtual void Complete(const FAIDACompletionRequest& Request, FAIDAOnChunk OnChunk,
		FAIDAOnComplete OnComplete, FAIDAOnError OnError) = 0;
	virtual FString GetName() const = 0;
};
