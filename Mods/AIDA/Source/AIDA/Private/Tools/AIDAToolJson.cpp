#include "Tools/AIDAToolJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FString AIDAToCompactJson(const TSharedRef<FJsonObject>& Object)
{
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Object, Writer);
	return Out;
}

TSharedRef<FJsonValue> AIDANumber(double Value)
{
	const double Rounded = FMath::RoundToDouble(Value * 10.0) / 10.0;
	FString Text = FString::Printf(TEXT("%.1f"), Rounded);
	if (Text.EndsWith(TEXT(".0"))) { Text.LeftChopInline(2); } // whole number -> no trailing ".0"
	return MakeShared<FJsonValueNumberString>(Text);
}
