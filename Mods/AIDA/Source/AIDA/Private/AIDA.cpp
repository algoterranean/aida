#include "AIDA.h"

DEFINE_LOG_CATEGORY(LogAIDA);

#define LOCTEXT_NAMESPACE "FAIDAModule"

void FAIDAModule::StartupModule()
{
	// Module load point. The AIDAOrchestrator is initialized as a world subsystem
	// (see Core/AIDAOrchestrator), never here — it must be safe with zero players connected.
	UE_LOG(LogAIDA, Log, TEXT("AIDA module loaded."));
}

void FAIDAModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAIDAModule, AIDA)
