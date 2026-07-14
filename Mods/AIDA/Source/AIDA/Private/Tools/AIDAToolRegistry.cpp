#include "Tools/AIDAToolRegistry.h"

#include "AIDA.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

void FAIDAToolRegistry::Register(FAIDAToolSpec Spec)
{
	if (Spec.Name.IsEmpty() || !Spec.Handler)
	{
		UE_LOG(LogAIDA, Warning, TEXT("[tools] ignoring tool with empty name or missing handler."));
		return;
	}

	const FString Name = Spec.Name;
	Tools.Add(Name, MoveTemp(Spec));
}

void FAIDAToolRegistry::GetSpecs(TArray<const FAIDAToolSpec*>& OutSpecs) const
{
	// Stable, name-sorted order so the tools block of the request is byte-identical across calls
	// (prompt-cache friendly — docs/PHASE2.md / claude-api prompt-caching notes).
	TArray<FString> Names;
	Tools.GetKeys(Names);
	Names.Sort();

	OutSpecs.Reset(Names.Num());
	for (const FString& Name : Names)
	{
		OutSpecs.Add(&Tools[Name]);
	}
}

FAIDAToolResult FAIDAToolRegistry::Dispatch(const FString& Name, const FString& ArgsJson, const FAIDAToolContext& Context) const
{
	const FAIDAToolSpec* Spec = Tools.Find(Name);
	if (!Spec)
	{
		return FAIDAToolResult::Error(FString::Printf(TEXT("Unknown tool: %s"), *Name));
	}

	// Parse the model-supplied arguments. Empty/whitespace is treated as {} so a no-arg tool works
	// whether the model emits "", "{}", or nothing at all.
	TSharedPtr<FJsonObject> Parsed;
	const FString Trimmed = ArgsJson.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		Parsed = MakeShared<FJsonObject>();
	}
	else
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
		{
			return FAIDAToolResult::Error(FString::Printf(TEXT("Tool '%s' received arguments that were not a valid JSON object."), *Name));
		}
	}

	return Spec->Handler(Parsed.ToSharedRef(), Context);
}
