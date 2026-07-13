#pragma once

#include "CoreMinimal.h"
#include "Core/AIDAConfig.h"

/**
 * Loads and validates the AIDA server config. Accepts JSONC (// line and block comments).
 * Engine-only — no FactoryGame headers. Kept static/free-standing so it is unit-testable
 * without a world (docs/DEV.md §5: config parsing is exercised by automation).
 */
class FAIDAConfigLoader
{
public:
	/** Reads FilePath, strips comments, parses, validates. Returns false and fills OutError on any failure. */
	static bool LoadFromFile(const FString& FilePath, FAIDAConfig& OutConfig, FString& OutError);

	/** Parses an in-memory JSONC string. Split out so tests don't need a file on disk. */
	static bool LoadFromString(const FString& Jsonc, FAIDAConfig& OutConfig, FString& OutError);

	/** Strips // line and block comments while preserving string literals (so "http://x" survives). */
	static FString StripJsonComments(const FString& Input);

private:
	static bool Validate(const FAIDAConfig& Config, FString& OutError);
};
