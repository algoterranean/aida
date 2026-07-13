#pragma once

#include "CoreMinimal.h"
#include "Adapters/AIDALLMTypes.h"

/**
 * One provider's translation to/from the common completion interface.
 * Complete() is async and non-blocking; exactly one of OnComplete/OnError fires (on the game thread).
 */
class ILLMAdapter
{
public:
	virtual ~ILLMAdapter() = default;

	virtual void Complete(const FAIDACompletionRequest& Request, FAIDAOnComplete OnComplete, FAIDAOnError OnError) = 0;
	virtual FString GetName() const = 0;
};
