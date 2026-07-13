#include "Core/AIDAConfigLoader.h"

#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FString FAIDAConfigLoader::StripJsonComments(const FString& Input)
{
	FString Out;
	Out.Reserve(Input.Len());

	bool bInString = false;
	bool bEscaped = false;
	const int32 Len = Input.Len();

	for (int32 i = 0; i < Len; ++i)
	{
		const TCHAR C = Input[i];
		const TCHAR Next = (i + 1 < Len) ? Input[i + 1] : TEXT('\0');

		if (bInString)
		{
			Out.AppendChar(C);
			if (bEscaped)            { bEscaped = false; }
			else if (C == TEXT('\\')) { bEscaped = true; }
			else if (C == TEXT('"'))  { bInString = false; }
			continue;
		}

		if (C == TEXT('"'))
		{
			bInString = true;
			Out.AppendChar(C);
			continue;
		}

		// Line comment: // ... to end of line. The newline is preserved.
		if (C == TEXT('/') && Next == TEXT('/'))
		{
			i += 2;
			while (i < Len && Input[i] != TEXT('\n') && Input[i] != TEXT('\r')) { ++i; }
			--i; // let the outer ++i land on (and keep) the line break
			continue;
		}

		// Block comment: /* ... */
		if (C == TEXT('/') && Next == TEXT('*'))
		{
			i += 2;
			while (i + 1 < Len && !(Input[i] == TEXT('*') && Input[i + 1] == TEXT('/'))) { ++i; }
			++i; // skip the closing '/'
			continue;
		}

		Out.AppendChar(C);
	}

	return Out;
}

bool FAIDAConfigLoader::LoadFromString(const FString& Jsonc, FAIDAConfig& OutConfig, FString& OutError)
{
	const FString Json = StripJsonComments(Jsonc);

	FAIDAConfig Parsed;
	if (!FJsonObjectConverter::JsonObjectStringToUStruct(Json, &Parsed, 0, 0))
	{
		OutError = TEXT("config is not valid JSON after comment stripping");
		return false;
	}

	if (!Validate(Parsed, OutError))
	{
		return false;
	}

	OutConfig = MoveTemp(Parsed);
	return true;
}

bool FAIDAConfigLoader::LoadFromFile(const FString& FilePath, FAIDAConfig& OutConfig, FString& OutError)
{
	if (!FPaths::FileExists(FilePath))
	{
		OutError = FString::Printf(TEXT("config file not found: %s"), *FilePath);
		return false;
	}

	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *FilePath))
	{
		OutError = FString::Printf(TEXT("failed to read config file: %s"), *FilePath);
		return false;
	}

	return LoadFromString(Raw, OutConfig, OutError);
}

bool FAIDAConfigLoader::Validate(const FAIDAConfig& Config, FString& OutError)
{
	const FString& Type = Config.Provider.Type;
	if (Type != TEXT("openai-compatible") && Type != TEXT("anthropic"))
	{
		OutError = FString::Printf(TEXT("provider.type must be 'openai-compatible' or 'anthropic', got '%s'"), *Type);
		return false;
	}
	if (Config.Provider.BaseUrl.IsEmpty())
	{
		OutError = TEXT("provider.baseUrl is empty");
		return false;
	}
	if (Config.Provider.Model.IsEmpty())
	{
		OutError = TEXT("provider.model is empty");
		return false;
	}
	if (Config.Limits.MaxToolRoundTrips < 0)
	{
		OutError = TEXT("limits.maxToolRoundTrips must be >= 0");
		return false;
	}

	return true;
}
