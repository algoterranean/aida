#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Core/AIDAConfigLoader.h"
#include "Core/AIDASessionManager.h"
#include "Core/AIDARateLimiter.h"
#include "Core/AIDAPermissionService.h"
#include "Adapters/AnthropicAdapter.h"
#include "Adapters/OpenAICompatAdapter.h"
#include "Adapters/SSEStream.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Engine-independent tests for the parts that must be correct without launching the game
// (docs/DEV.md §5): JSONC comment stripping, config parse/validate, adapter wire format.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAStripCommentsTest, "AIDA.Config.StripComments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAStripCommentsTest::RunTest(const FString&)
{
	const FString In = TEXT(R"({ "url": "http://x//y", // line comment
  "a": 1 /* block */ })");
	const FString Out = FAIDAConfigLoader::StripJsonComments(In);

	TestTrue(TEXT("preserves // inside string literals"), Out.Contains(TEXT("http://x//y")));
	TestFalse(TEXT("removes line comments"), Out.Contains(TEXT("line comment")));
	TestFalse(TEXT("removes block comments"), Out.Contains(TEXT("block")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAConfigValidTest, "AIDA.Config.ParsesValid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAConfigValidTest::RunTest(const FString&)
{
	const FString Jsonc = TEXT(R"({
  // a comment, and a URL with // in it below
  "provider": { "type": "anthropic", "baseUrl": "https://api.anthropic.com", "model": "claude-haiku-4-5-20251001", "maxOutputTokens": 512 },
  "permissions": { "act": ["id-1", "id-2"] },
  "privacy": { "sendFactoryData": false }
})");

	FAIDAConfig Config;
	FString Error;
	const bool bOk = FAIDAConfigLoader::LoadFromString(Jsonc, Config, Error);

	TestTrue(FString::Printf(TEXT("parses (%s)"), *Error), bOk);
	TestEqual(TEXT("provider.type"), Config.Provider.Type, TEXT("anthropic"));
	TestEqual(TEXT("provider.baseUrl"), Config.Provider.BaseUrl, TEXT("https://api.anthropic.com"));
	TestEqual(TEXT("provider.model"), Config.Provider.Model, TEXT("claude-haiku-4-5-20251001"));
	TestEqual(TEXT("provider.maxOutputTokens"), Config.Provider.MaxOutputTokens, 512);
	TestEqual(TEXT("permissions.act count"), Config.Permissions.Act.Num(), 2);
	if (Config.Permissions.Act.Num() == 2)
	{
		TestEqual(TEXT("permissions.act[0]"), Config.Permissions.Act[0], TEXT("id-1"));
	}
	// camelCase bool with the "b" prefix: "sendFactoryData": false -> bSendFactoryData == false
	TestFalse(TEXT("privacy.sendFactoryData maps to bSendFactoryData"), Config.Privacy.bSendFactoryData);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAConfigRejectsBadTypeTest, "AIDA.Config.RejectsBadProviderType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAConfigRejectsBadTypeTest::RunTest(const FString&)
{
	const FString Jsonc = TEXT(R"({ "provider": { "type": "openai", "baseUrl": "x", "model": "y" } })");
	FAIDAConfig Config;
	FString Error;
	TestFalse(TEXT("rejects unknown provider type"), FAIDAConfigLoader::LoadFromString(Jsonc, Config, Error));
	TestTrue(TEXT("reports an error"), !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAConfigRejectsEmptyModelTest, "AIDA.Config.RejectsEmptyModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAConfigRejectsEmptyModelTest::RunTest(const FString&)
{
	const FString Jsonc = TEXT(R"({ "provider": { "type": "anthropic", "baseUrl": "x" } })");
	FAIDAConfig Config;
	FString Error;
	TestFalse(TEXT("rejects empty model"), FAIDAConfigLoader::LoadFromString(Jsonc, Config, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAAnthropicBodyTest, "AIDA.Adapters.AnthropicWireFormat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAAnthropicBodyTest::RunTest(const FString&)
{
	FAIDACompletionRequest Req;
	Req.Model = TEXT("test-model");
	Req.MaxTokens = 256;
	Req.System = TEXT("you are a test");
	Req.Messages.Add({ TEXT("user"), TEXT("hello") });

	const FString Body = FAnthropicAdapter::BuildRequestBody(Req);

	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	TestTrue(TEXT("body is valid JSON"), FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid());
	if (!Json.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("model"), Json->GetStringField(TEXT("model")), TEXT("test-model"));
	TestEqual(TEXT("max_tokens"), (int32)Json->GetNumberField(TEXT("max_tokens")), 256);
	TestEqual(TEXT("system"), Json->GetStringField(TEXT("system")), TEXT("you are a test"));
	// Default build is non-streaming: the "stream" field must be absent so callers opt in explicitly.
	TestFalse(TEXT("stream absent by default"), Json->HasField(TEXT("stream")));

	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	TestTrue(TEXT("messages is an array"), Json->TryGetArrayField(TEXT("messages"), Messages) && Messages);
	if (Messages && Messages->Num() == 1)
	{
		const TSharedPtr<FJsonObject> M = (*Messages)[0]->AsObject();
		TestEqual(TEXT("messages[0].role"), M->GetStringField(TEXT("role")), TEXT("user"));
		TestEqual(TEXT("messages[0].content"), M->GetStringField(TEXT("content")), TEXT("hello"));
	}
	else
	{
		AddError(TEXT("expected exactly one message"));
	}
	return true;
}

// Helper: parse a body string to a JSON object for the streaming-flag assertions below.
static TSharedPtr<FJsonObject> ParseJsonObject(const FString& Body)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	FJsonSerializer::Deserialize(Reader, Json);
	return Json;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAStreamFlagTest, "AIDA.Adapters.StreamFlag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAStreamFlagTest::RunTest(const FString&)
{
	FAIDACompletionRequest Req;
	Req.Model = TEXT("m");
	Req.Messages.Add({ TEXT("user"), TEXT("hi") });

	// Both providers must set "stream": true when asked, and omit it otherwise.
	const TSharedPtr<FJsonObject> AnthStream = ParseJsonObject(FAnthropicAdapter::BuildRequestBody(Req, true));
	TestTrue(TEXT("anthropic stream=true present"), AnthStream.IsValid() && AnthStream->HasField(TEXT("stream")));
	TestTrue(TEXT("anthropic stream=true value"), AnthStream.IsValid() && AnthStream->GetBoolField(TEXT("stream")));

	const TSharedPtr<FJsonObject> OaiStream = ParseJsonObject(FOpenAICompatAdapter::BuildRequestBody(Req, true));
	TestTrue(TEXT("openai stream=true present"), OaiStream.IsValid() && OaiStream->HasField(TEXT("stream")));
	TestTrue(TEXT("openai stream=true value"), OaiStream.IsValid() && OaiStream->GetBoolField(TEXT("stream")));

	const TSharedPtr<FJsonObject> OaiNoStream = ParseJsonObject(FOpenAICompatAdapter::BuildRequestBody(Req));
	TestFalse(TEXT("openai stream absent by default"), OaiNoStream.IsValid() && OaiNoStream->HasField(TEXT("stream")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDASSEFramingTest, "AIDA.Adapters.SSEFraming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDASSEFramingTest::RunTest(const FString&)
{
	FAIDASSEBuffer Buffer;
	TArray<FString> Lines;
	const auto Collect = [&Lines](const FString& Line) { Lines.Add(Line); };

	// A multi-byte UTF-8 codepoint (é = 0xC3 0xA9) split across two Feed() calls must survive intact,
	// and a line only completes once its terminating '\n' arrives.
	const uint8 Chunk1[] = { 'd', 'a', 't', 'a', ':', ' ', 'c', 0xC3 };
	const uint8 Chunk2[] = { 0xA9, '\r', '\n', 'x' }; // finish é, CRLF, then a partial next line
	Buffer.Feed(Chunk1, sizeof(Chunk1), Collect);
	TestEqual(TEXT("no line until newline arrives"), Lines.Num(), 0);

	Buffer.Feed(Chunk2, sizeof(Chunk2), Collect);
	TestEqual(TEXT("one complete line"), Lines.Num(), 1);
	if (Lines.Num() == 1)
	{
		// Build "data: cé" from the codepoint so the assertion doesn't depend on this file's encoding.
		FString Expected = TEXT("data: c");
		Expected.AppendChar(static_cast<TCHAR>(0x00E9)); // é
		TestEqual(TEXT("CRLF trimmed, multi-byte decoded across chunks"), Lines[0], Expected);
	}

	// The trailing 'x' stays buffered until its own newline.
	const uint8 Chunk3[] = { '\n' };
	Buffer.Feed(Chunk3, sizeof(Chunk3), Collect);
	TestEqual(TEXT("second line flushed"), Lines.Num(), 2);
	if (Lines.Num() == 2)
	{
		TestEqual(TEXT("second line content"), Lines[1], FString(TEXT("x")));
	}
	return true;
}

// ─────────────────────────── SessionManager (transcript) ───────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDASessionAssemblyTest, "AIDA.Session.StreamingAssembly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDASessionAssemblyTest::RunTest(const FString&)
{
	// No relay set — the session still owns the authoritative transcript (fan-out is a no-op).
	FAIDASessionManager Session(200);

	const FGuid PlayerId = Session.PostPlayerMessage(TEXT("Alice"), TEXT("hello"));
	const FGuid AidaId = Session.BeginAIDAMessage(TEXT("AIDA"));
	Session.AppendDelta(AidaId, TEXT("Hi "));
	Session.AppendDelta(AidaId, TEXT("there"));
	Session.CompleteMessage(AidaId);

	FAIDATranscriptEntry Player, Aida;
	TestTrue(TEXT("player message retained"), Session.GetMessageBody(PlayerId, Player));
	TestEqual(TEXT("player author"), Player.Header.Author, TEXT("Alice"));
	TestEqual(TEXT("player body"), Player.Body, TEXT("hello"));

	TestTrue(TEXT("aida message retained"), Session.GetMessageBody(AidaId, Aida));
	TestEqual(TEXT("aida body is concatenation of deltas"), Aida.Body, TEXT("Hi there"));
	TestTrue(TEXT("aida kind"), Aida.Header.Kind == EAIDAMsgKind::AIDA);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDASessionRingBufferTest, "AIDA.Session.RingBufferPrune",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDASessionRingBufferTest::RunTest(const FString&)
{
	FAIDASessionManager Session(2); // keep only the two most recent messages

	const FGuid First = Session.PostPlayerMessage(TEXT("A"), TEXT("1"));
	Session.PostPlayerMessage(TEXT("B"), TEXT("2"));
	Session.PostPlayerMessage(TEXT("C"), TEXT("3"));

	TestEqual(TEXT("bounded to max"), Session.Num(), 2);

	FAIDATranscriptEntry Dummy;
	TestFalse(TEXT("oldest pruned"), Session.GetMessageBody(First, Dummy));

	TArray<FAIDATranscriptEntry> Recent;
	Session.GetRecentTranscript(Recent);
	TestEqual(TEXT("recent count"), Recent.Num(), 2);
	if (Recent.Num() == 2)
	{
		TestEqual(TEXT("oldest kept is B"), Recent[0].Body, TEXT("2"));
		TestEqual(TEXT("newest is C"), Recent[1].Body, TEXT("3"));
	}
	return true;
}

// ─────────────────────────── RateLimiter (token bucket) ───────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDARateLimiterTest, "AIDA.Policy.RateLimiter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDARateLimiterTest::RunTest(const FString&)
{
	FAIDALimitsConfig Limits;
	Limits.PerPlayerPerMinute = 2;
	Limits.GlobalPerMinute = 100; // don't let the global bucket interfere
	FAIDARateLimiter Limiter(Limits);

	FString Reason;
	TestTrue(TEXT("1st admitted"), Limiter.TryConsume(TEXT("p1"), 0.0, Reason));
	TestTrue(TEXT("2nd admitted (burst = capacity)"), Limiter.TryConsume(TEXT("p1"), 0.0, Reason));
	TestFalse(TEXT("3rd denied at same instant"), Limiter.TryConsume(TEXT("p1"), 0.0, Reason));
	TestTrue(TEXT("denial has a reason"), !Reason.IsEmpty());

	// A different player has their own bucket.
	TestTrue(TEXT("other player unaffected"), Limiter.TryConsume(TEXT("p2"), 0.0, Reason));

	// After 30s at 2/min, one token has refilled.
	TestTrue(TEXT("refills over time"), Limiter.TryConsume(TEXT("p1"), 30.0, Reason));
	TestFalse(TEXT("but not two"), Limiter.TryConsume(TEXT("p1"), 30.0, Reason));

	// 0 disables the limit entirely.
	FAIDALimitsConfig Unlimited;
	Unlimited.PerPlayerPerMinute = 0;
	Unlimited.GlobalPerMinute = 0;
	FAIDARateLimiter Free(Unlimited);
	bool bAll = true;
	for (int32 i = 0; i < 50; ++i) { bAll &= Free.TryConsume(TEXT("p"), 0.0, Reason); }
	TestTrue(TEXT("0 = unlimited"), bAll);
	return true;
}

// ─────────────────────────── PermissionService (tiers) ───────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAPermissionTest, "AIDA.Policy.Permissions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAPermissionTest::RunTest(const FString&)
{
	FAIDAPermissionsConfig Perms;
	Perms.Chat = TEXT("everyone");
	Perms.Query = TEXT("everyone");
	Perms.Act = { TEXT("admin-1") };
	FAIDAPermissionService Service(Perms);

	TestTrue(TEXT("chat open to everyone"), Service.IsAllowed(EAIDATier::Chat, TEXT("anyone")));
	TestTrue(TEXT("query open to everyone"), Service.IsAllowed(EAIDATier::Query, TEXT("anyone")));
	TestTrue(TEXT("act allows listed id"), Service.IsAllowed(EAIDATier::Act, TEXT("admin-1")));
	TestFalse(TEXT("act denies unlisted id"), Service.IsAllowed(EAIDATier::Act, TEXT("rando")));
	TestFalse(TEXT("act denies empty id"), Service.IsAllowed(EAIDATier::Act, FString()));

	// A restricted chat tier falls back to the act allowlist.
	FAIDAPermissionsConfig Restricted;
	Restricted.Chat = TEXT("act");
	Restricted.Act = { TEXT("admin-1") };
	FAIDAPermissionService RestrictedService(Restricted);
	TestTrue(TEXT("restricted chat allows admin"), RestrictedService.IsAllowed(EAIDATier::Chat, TEXT("admin-1")));
	TestFalse(TEXT("restricted chat denies non-admin"), RestrictedService.IsAllowed(EAIDATier::Chat, TEXT("rando")));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
