#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Core/AIDAConfigLoader.h"
#include "Core/AIDASessionManager.h"
#include "Core/AIDARateLimiter.h"
#include "Core/AIDAPermissionService.h"
#include "Adapters/AnthropicAdapter.h"
#include "Adapters/OpenAICompatAdapter.h"
#include "Adapters/SSEStream.h"
#include "Tools/AIDAToolRegistry.h"
#include "Factory/AIDAFactoryAggregator.h"
#include "Tools/AIDAFactoryTools.h"
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

//~ ─────────────────────────────── ToolRegistry ───────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAToolRegistryDispatchTest, "AIDA.Tools.Dispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAToolRegistryDispatchTest::RunTest(const FString&)
{
	FAIDAToolRegistry Registry;

	// An echo tool that reads a string arg and reflects it, attributed to the caller.
	Registry.Register({
		TEXT("echo"),
		TEXT("Echo the 'text' argument back."),
		TEXT(R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})"),
		EAIDAToolTier::Query,
		[](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			FString Text;
			Args->TryGetStringField(TEXT("text"), Text);
			return FAIDAToolResult::Ok(FString::Printf(TEXT("%s said: %s"), *Ctx.Author, *Text));
		}
	});

	TestEqual(TEXT("one tool registered"), Registry.Num(), 1);
	TestTrue(TEXT("contains echo"), Registry.Contains(TEXT("echo")));

	FAIDAToolContext Ctx;
	Ctx.Author = TEXT("Ada");
	Ctx.PlayerId = TEXT("p1");

	const FAIDAToolResult Ok = Registry.Dispatch(TEXT("echo"), TEXT(R"({"text":"hi"})"), Ctx);
	TestFalse(TEXT("echo not an error"), Ok.bIsError);
	TestEqual(TEXT("echo content"), Ok.Content, TEXT("Ada said: hi"));

	// Unknown tool -> error result (not a crash).
	const FAIDAToolResult Unknown = Registry.Dispatch(TEXT("nope"), TEXT("{}"), Ctx);
	TestTrue(TEXT("unknown tool is error"), Unknown.bIsError);

	// Malformed args -> error result.
	const FAIDAToolResult Bad = Registry.Dispatch(TEXT("echo"), TEXT("not json"), Ctx);
	TestTrue(TEXT("bad args is error"), Bad.bIsError);

	// Empty args are treated as {} (no-arg tools work); echo then reflects an empty string.
	const FAIDAToolResult Empty = Registry.Dispatch(TEXT("echo"), FString(), Ctx);
	TestFalse(TEXT("empty args not an error"), Empty.bIsError);
	TestEqual(TEXT("empty args -> empty text"), Empty.Content, TEXT("Ada said: "));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAToolRegistryOrderingTest, "AIDA.Tools.SpecsSortedAndReplace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAToolRegistryOrderingTest::RunTest(const FString&)
{
	FAIDAToolRegistry Registry;
	auto NoOp = [](const TSharedRef<FJsonObject>&, const FAIDAToolContext&) { return FAIDAToolResult::Ok(TEXT("{}")); };

	Registry.Register({ TEXT("zebra"), TEXT(""), TEXT(""), EAIDAToolTier::Query, NoOp });
	Registry.Register({ TEXT("alpha"), TEXT(""), TEXT(""), EAIDAToolTier::Chat, NoOp });
	Registry.Register({ TEXT("mango"), TEXT(""), TEXT(""), EAIDAToolTier::Act, NoOp });

	// Empty name / missing handler are ignored.
	Registry.Register({ TEXT(""), TEXT(""), TEXT(""), EAIDAToolTier::Query, NoOp });
	Registry.Register({ TEXT("noHandler"), TEXT(""), TEXT(""), EAIDAToolTier::Query, FAIDAToolHandler() });
	TestEqual(TEXT("invalid tools ignored"), Registry.Num(), 3);

	TArray<const FAIDAToolSpec*> Specs;
	Registry.GetSpecs(Specs);
	TestEqual(TEXT("spec count"), Specs.Num(), 3);
	if (Specs.Num() == 3)
	{
		TestEqual(TEXT("sorted[0]"), Specs[0]->Name, TEXT("alpha"));
		TestEqual(TEXT("sorted[1]"), Specs[1]->Name, TEXT("mango"));
		TestEqual(TEXT("sorted[2]"), Specs[2]->Name, TEXT("zebra"));
	}

	// Re-registering the same name replaces (and preserves the new tier), not duplicates.
	Registry.Register({ TEXT("alpha"), TEXT("v2"), TEXT(""), EAIDAToolTier::Act, NoOp });
	TestEqual(TEXT("replace does not grow count"), Registry.Num(), 3);
	TestEqual(TEXT("replaced tier"), (int32)Registry.Find(TEXT("alpha"))->Tier, (int32)EAIDAToolTier::Act);

	return true;
}

//~ ──────────────────────── Tool wire format (Phase 2 Slice 0) ────────────────────────

// Build a request that exercises all three tool-carrying shapes: a tools block, an assistant turn
// with a tool_use call, and a user turn with a tool_result. Both adapters must translate them to
// their provider's wire form without a network call.
static FAIDACompletionRequest MakeToolRequest()
{
	FAIDACompletionRequest Req;
	Req.Model = TEXT("test-model");
	Req.MaxTokens = 256;
	Req.Tools.Add({ TEXT("echo"), TEXT("Echo text back."),
		TEXT(R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})") });

	Req.Messages.Add({ TEXT("user"), TEXT("echo hi") });

	FAIDAChatMessage Assistant;
	Assistant.Role = TEXT("assistant");
	Assistant.ToolCalls.Add({ TEXT("call_1"), TEXT("echo"), TEXT(R"({"text":"hi"})") });
	Req.Messages.Add(MoveTemp(Assistant));

	FAIDAChatMessage ToolTurn;
	ToolTurn.Role = TEXT("user");
	ToolTurn.ToolResults.Add({ TEXT("call_1"), TEXT("echo: hi"), false });
	Req.Messages.Add(MoveTemp(ToolTurn));

	return Req;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAAnthropicToolWireTest, "AIDA.Adapters.AnthropicToolWireFormat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAAnthropicToolWireTest::RunTest(const FString&)
{
	const TSharedPtr<FJsonObject> Json = ParseJsonObject(FAnthropicAdapter::BuildRequestBody(MakeToolRequest()));
	TestTrue(TEXT("valid JSON"), Json.IsValid());
	if (!Json.IsValid()) { return false; }

	// tools block: name + object input_schema.
	const TArray<TSharedPtr<FJsonValue>>* ToolsArr = nullptr;
	TestTrue(TEXT("tools is an array"), Json->TryGetArrayField(TEXT("tools"), ToolsArr) && ToolsArr && ToolsArr->Num() == 1);
	if (ToolsArr && ToolsArr->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Tool = (*ToolsArr)[0]->AsObject();
		TestEqual(TEXT("tools[0].name"), Tool->GetStringField(TEXT("name")), TEXT("echo"));
		const TSharedPtr<FJsonObject>* Schema = nullptr;
		TestTrue(TEXT("input_schema is an object"), Tool->TryGetObjectField(TEXT("input_schema"), Schema) && Schema);
	}

	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	Json->TryGetArrayField(TEXT("messages"), Messages);
	if (!Messages || Messages->Num() != 3) { AddError(TEXT("expected 3 messages")); return false; }

	// messages[1]: assistant content array with a tool_use block (parsed input, not a string).
	{
		const TSharedPtr<FJsonObject> M = (*Messages)[1]->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
		TestTrue(TEXT("assistant content is an array"), M->TryGetArrayField(TEXT("content"), Blocks) && Blocks && Blocks->Num() == 1);
		if (Blocks && Blocks->Num() == 1)
		{
			const TSharedPtr<FJsonObject> B = (*Blocks)[0]->AsObject();
			TestEqual(TEXT("tool_use type"), B->GetStringField(TEXT("type")), TEXT("tool_use"));
			TestEqual(TEXT("tool_use id"), B->GetStringField(TEXT("id")), TEXT("call_1"));
			TestEqual(TEXT("tool_use name"), B->GetStringField(TEXT("name")), TEXT("echo"));
			const TSharedPtr<FJsonObject>* Input = nullptr;
			TestTrue(TEXT("tool_use input is an object"), B->TryGetObjectField(TEXT("input"), Input) && Input);
			if (Input) { TestEqual(TEXT("input.text parsed"), (*Input)->GetStringField(TEXT("text")), TEXT("hi")); }
		}
	}

	// messages[2]: user content array with a tool_result block.
	{
		const TSharedPtr<FJsonObject> M = (*Messages)[2]->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
		TestTrue(TEXT("tool_result content is an array"), M->TryGetArrayField(TEXT("content"), Blocks) && Blocks && Blocks->Num() == 1);
		if (Blocks && Blocks->Num() == 1)
		{
			const TSharedPtr<FJsonObject> B = (*Blocks)[0]->AsObject();
			TestEqual(TEXT("tool_result type"), B->GetStringField(TEXT("type")), TEXT("tool_result"));
			TestEqual(TEXT("tool_result tool_use_id"), B->GetStringField(TEXT("tool_use_id")), TEXT("call_1"));
			TestEqual(TEXT("tool_result content"), B->GetStringField(TEXT("content")), TEXT("echo: hi"));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAOpenAIToolWireTest, "AIDA.Adapters.OpenAIToolWireFormat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAOpenAIToolWireTest::RunTest(const FString&)
{
	const TSharedPtr<FJsonObject> Json = ParseJsonObject(FOpenAICompatAdapter::BuildRequestBody(MakeToolRequest()));
	TestTrue(TEXT("valid JSON"), Json.IsValid());
	if (!Json.IsValid()) { return false; }

	// tools block: {type:function, function:{name, parameters}}.
	const TArray<TSharedPtr<FJsonValue>>* ToolsArr = nullptr;
	TestTrue(TEXT("tools is an array"), Json->TryGetArrayField(TEXT("tools"), ToolsArr) && ToolsArr && ToolsArr->Num() == 1);
	if (ToolsArr && ToolsArr->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Tool = (*ToolsArr)[0]->AsObject();
		TestEqual(TEXT("tools[0].type"), Tool->GetStringField(TEXT("type")), TEXT("function"));
		const TSharedPtr<FJsonObject>* Fn = nullptr;
		TestTrue(TEXT("tools[0].function object"), Tool->TryGetObjectField(TEXT("function"), Fn) && Fn);
		if (Fn) { TestEqual(TEXT("function.name"), (*Fn)->GetStringField(TEXT("name")), TEXT("echo")); }
	}

	// No system => messages are [user, assistant(with tool_calls), tool].
	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	Json->TryGetArrayField(TEXT("messages"), Messages);
	if (!Messages || Messages->Num() != 3) { AddError(TEXT("expected 3 messages")); return false; }

	{
		const TSharedPtr<FJsonObject> M = (*Messages)[1]->AsObject();
		TestEqual(TEXT("assistant role"), M->GetStringField(TEXT("role")), TEXT("assistant"));
		const TArray<TSharedPtr<FJsonValue>>* Calls = nullptr;
		TestTrue(TEXT("tool_calls array"), M->TryGetArrayField(TEXT("tool_calls"), Calls) && Calls && Calls->Num() == 1);
		if (Calls && Calls->Num() == 1)
		{
			const TSharedPtr<FJsonObject> C = (*Calls)[0]->AsObject();
			TestEqual(TEXT("tool_call id"), C->GetStringField(TEXT("id")), TEXT("call_1"));
			TestEqual(TEXT("tool_call type"), C->GetStringField(TEXT("type")), TEXT("function"));
			const TSharedPtr<FJsonObject>* Fn = nullptr;
			C->TryGetObjectField(TEXT("function"), Fn);
			if (Fn)
			{
				TestEqual(TEXT("function.name"), (*Fn)->GetStringField(TEXT("name")), TEXT("echo"));
				TestEqual(TEXT("function.arguments (raw string)"), (*Fn)->GetStringField(TEXT("arguments")), TEXT(R"({"text":"hi"})"));
			}
		}
	}

	{
		const TSharedPtr<FJsonObject> M = (*Messages)[2]->AsObject();
		TestEqual(TEXT("tool role"), M->GetStringField(TEXT("role")), TEXT("tool"));
		TestEqual(TEXT("tool tool_call_id"), M->GetStringField(TEXT("tool_call_id")), TEXT("call_1"));
		TestEqual(TEXT("tool content"), M->GetStringField(TEXT("content")), TEXT("echo: hi"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAAnthropicToolStreamTest, "AIDA.Adapters.AnthropicToolStreaming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAAnthropicToolStreamTest::RunTest(const FString&)
{
	FAIDAStreamAccumulator Acc;

	// A text_delta returns visible text; tool_use blocks accumulate across input_json_delta events.
	const FString Text = FAnthropicAdapter::HandleStreamEvent(
		TEXT(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Sure. "}})"), Acc);
	TestEqual(TEXT("text_delta returned"), Text, TEXT("Sure. "));

	FAnthropicAdapter::HandleStreamEvent(
		TEXT(R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_1","name":"echo"}})"), Acc);
	FAnthropicAdapter::HandleStreamEvent(
		TEXT(R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"text\":"}})"), Acc);
	FAnthropicAdapter::HandleStreamEvent(
		TEXT(R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"\"hi\"}"}})"), Acc);
	TestEqual(TEXT("not finalized until block stop"), Acc.ToolCalls.Num(), 0);

	FAnthropicAdapter::HandleStreamEvent(TEXT(R"({"type":"content_block_stop","index":1})"), Acc);
	FAnthropicAdapter::HandleStreamEvent(
		TEXT(R"({"type":"message_delta","delta":{"stop_reason":"tool_use"}})"), Acc);

	TestEqual(TEXT("one tool call finalized"), Acc.ToolCalls.Num(), 1);
	TestEqual(TEXT("no stragglers pending"), Acc.PendingByIndex.Num(), 0);
	if (Acc.ToolCalls.Num() == 1)
	{
		TestEqual(TEXT("tool id"), Acc.ToolCalls[0].Id, TEXT("toolu_1"));
		TestEqual(TEXT("tool name"), Acc.ToolCalls[0].Name, TEXT("echo"));
		TestEqual(TEXT("assembled args"), Acc.ToolCalls[0].ArgsJson, TEXT(R"({"text":"hi"})"));
	}
	TestEqual(TEXT("stop reason"), Acc.StopReason, TEXT("tool_use"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAOpenAIToolStreamTest, "AIDA.Adapters.OpenAIToolStreaming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAOpenAIToolStreamTest::RunTest(const FString&)
{
	FAIDAStreamAccumulator Acc;

	FOpenAICompatAdapter::HandleStreamEvent(
		TEXT(R"({"choices":[{"index":0,"delta":{"role":"assistant","content":""},"finish_reason":null}]})"), Acc);
	FOpenAICompatAdapter::HandleStreamEvent(
		TEXT(R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"echo","arguments":""}}]},"finish_reason":null}]})"), Acc);
	FOpenAICompatAdapter::HandleStreamEvent(
		TEXT(R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"text\":"}}]},"finish_reason":null}]})"), Acc);
	FOpenAICompatAdapter::HandleStreamEvent(
		TEXT(R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"hi\"}"}}]},"finish_reason":null}]})"), Acc);
	TestEqual(TEXT("not finalized until finish_reason"), Acc.ToolCalls.Num(), 0);

	FOpenAICompatAdapter::HandleStreamEvent(
		TEXT(R"({"choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})"), Acc);

	TestEqual(TEXT("one tool call finalized"), Acc.ToolCalls.Num(), 1);
	TestEqual(TEXT("no stragglers pending"), Acc.PendingByIndex.Num(), 0);
	if (Acc.ToolCalls.Num() == 1)
	{
		TestEqual(TEXT("tool id"), Acc.ToolCalls[0].Id, TEXT("call_1"));
		TestEqual(TEXT("tool name"), Acc.ToolCalls[0].Name, TEXT("echo"));
		TestEqual(TEXT("assembled args"), Acc.ToolCalls[0].ArgsJson, TEXT(R"({"text":"hi"})"));
	}
	TestEqual(TEXT("stop reason"), Acc.StopReason, TEXT("tool_calls"));
	return true;
}

// ----------------------------------------------------------------------------------------------------
// Phase 2 Slice 1 — factory aggregation math (docs/PHASE2.md). Pure logic on synthetic entities.
// ----------------------------------------------------------------------------------------------------

static FAIDAMachine AIDAMakeTestMachine(int32 Id, const FVector& Loc, const FString& Cls = TEXT("Machine"))
{
	FAIDAMachine M;
	M.Id = Id;
	M.Location = Loc;
	M.BuildingClass = Cls;
	return M;
}

#define AIDA_TEST_NEAR(What, A, B) TestTrue(TEXT(What), FMath::IsNearlyEqual((double)(A), (double)(B), 1e-6))

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAFactoryClusteringTest, "AIDA.Factory.Clustering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAFactoryClusteringTest::RunTest(const FString&)
{
	// Two tight pairs, ~20 km apart: with the 50 m epsilon this must be exactly two clusters.
	TArray<FAIDAMachine> Machines;
	Machines.Add(AIDAMakeTestMachine(1, FVector(0, 0, 0)));
	Machines.Add(AIDAMakeTestMachine(2, FVector(500, 0, 0)));
	Machines.Add(AIDAMakeTestMachine(3, FVector(20000, 0, 0)));
	Machines.Add(AIDAMakeTestMachine(4, FVector(20400, 0, 0)));

	const TArray<int32> Labels = FAIDAFactoryAggregator::ClusterMachines(Machines, FAIDAAggregatorConfig());
	TestEqual(TEXT("label per machine"), Labels.Num(), 4);
	TestEqual(TEXT("pair A shares a cluster"), Labels[0], Labels[1]);
	TestEqual(TEXT("pair B shares a cluster"), Labels[2], Labels[3]);
	TestNotEqual(TEXT("the pairs are different clusters"), Labels[0], Labels[2]);
	TestEqual(TEXT("exactly two clusters"), TSet<int32>(Labels).Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAFactoryBalanceTest, "AIDA.Factory.BalanceSheet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAFactoryBalanceTest::RunTest(const FString&)
{
	FAIDAMachine Smelter = AIDAMakeTestMachine(1, FVector::ZeroVector, TEXT("Smelter"));
	Smelter.Inputs = { { TEXT("IronOre"), 30.0 } };
	Smelter.Outputs = { { TEXT("IronIngot"), 30.0 } };
	FAIDAMachine Ctor = AIDAMakeTestMachine(2, FVector(100, 0, 0), TEXT("Constructor"));
	Ctor.Inputs = { { TEXT("IronIngot"), 30.0 } };
	Ctor.Outputs = { { TEXT("IronPlate"), 20.0 } };

	const TArray<FAIDAItemBalance> Balance = FAIDAFactoryAggregator::BuildBalanceSheet({ Smelter, Ctor });
	// Sorted by item key: IronIngot, IronOre, IronPlate.
	TestEqual(TEXT("three items"), Balance.Num(), 3);
	if (Balance.Num() == 3)
	{
		TestEqual(TEXT("[0] item"), Balance[0].Item, FString(TEXT("IronIngot")));
		AIDA_TEST_NEAR("IronIngot balances to net 0", Balance[0].Net(), 0.0);
		TestFalse(TEXT("IronIngot not in deficit"), Balance[0].IsDeficit());

		TestEqual(TEXT("[1] item"), Balance[1].Item, FString(TEXT("IronOre")));
		AIDA_TEST_NEAR("IronOre consumed 30", Balance[1].Consumed, 30.0);
		AIDA_TEST_NEAR("IronOre produced 0", Balance[1].Produced, 0.0);
		TestTrue(TEXT("IronOre is a deficit (raw input)"), Balance[1].IsDeficit());

		TestEqual(TEXT("[2] item"), Balance[2].Item, FString(TEXT("IronPlate")));
		AIDA_TEST_NEAR("IronPlate net +20", Balance[2].Net(), 20.0);
		TestFalse(TEXT("IronPlate not in deficit"), Balance[2].IsDeficit());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAFactoryClusterNetFlowsTest, "AIDA.Factory.ClusterNetFlows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAFactoryClusterNetFlowsTest::RunTest(const FString&)
{
	// One cluster: internal IronIngot cancels; IronOre is a net import, IronPlate a net export.
	FAIDAMachine Smelter = AIDAMakeTestMachine(1, FVector::ZeroVector, TEXT("Smelter"));
	Smelter.Inputs = { { TEXT("IronOre"), 30.0 } };
	Smelter.Outputs = { { TEXT("IronIngot"), 30.0 } };
	Smelter.Productivity = 1.0f;
	FAIDAMachine Ctor = AIDAMakeTestMachine(2, FVector(100, 0, 0), TEXT("Constructor"));
	Ctor.Inputs = { { TEXT("IronIngot"), 30.0 } };
	Ctor.Outputs = { { TEXT("IronPlate"), 20.0 } };
	Ctor.Productivity = 0.5f;

	const TArray<FAIDAMachine> Machines = { Smelter, Ctor };
	const TArray<FAIDACluster> Clusters = FAIDAFactoryAggregator::BuildClusters(Machines, { 0, 0 }, FAIDAAggregatorConfig());

	TestEqual(TEXT("single cluster"), Clusters.Num(), 1);
	if (Clusters.Num() == 1)
	{
		const FAIDACluster& C = Clusters[0];
		TestEqual(TEXT("two machines"), C.MachineIds.Num(), 2);
		TestEqual(TEXT("census Smelter"), C.BuildingCensus.FindRef(TEXT("Smelter")), 1);
		TestEqual(TEXT("census Constructor"), C.BuildingCensus.FindRef(TEXT("Constructor")), 1);
		AIDA_TEST_NEAR("efficiency = mean productivity", C.Efficiency, 0.75);

		TestEqual(TEXT("one net import"), C.NetInputs.Num(), 1);
		if (C.NetInputs.Num() == 1)
		{
			TestEqual(TEXT("imports IronOre"), C.NetInputs[0].Item, FString(TEXT("IronOre")));
			AIDA_TEST_NEAR("imports 30/min", C.NetInputs[0].PerMinute, 30.0);
		}
		TestEqual(TEXT("one net export"), C.NetOutputs.Num(), 1);
		if (C.NetOutputs.Num() == 1)
		{
			TestEqual(TEXT("exports IronPlate"), C.NetOutputs[0].Item, FString(TEXT("IronPlate")));
			AIDA_TEST_NEAR("exports 20/min", C.NetOutputs[0].PerMinute, 20.0);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAFactoryLogisticsTest, "AIDA.Factory.Logistics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAFactoryLogisticsTest::RunTest(const FString&)
{
	// Machines 1 & 3 form cluster 0; machine 2 is the distant cluster 1.
	TArray<FAIDAMachine> Machines;
	Machines.Add(AIDAMakeTestMachine(1, FVector(0, 0, 0)));
	Machines.Add(AIDAMakeTestMachine(3, FVector(300, 0, 0)));
	Machines.Add(AIDAMakeTestMachine(2, FVector(20000, 0, 0)));

	const TArray<int32> Labels = FAIDAFactoryAggregator::ClusterMachines(Machines, FAIDAAggregatorConfig());
	TMap<int32, int32> IdToCluster;
	for (int32 i = 0; i < Machines.Num(); ++i) { IdToCluster.Add(Machines[i].Id, Labels[i]); }

	TArray<FAIDAConveyorEdge> Edges;
	Edges.Add({ 1, 3, TEXT("IronRod"), 30.0 });     // intra-cluster -> dropped
	Edges.Add({ 1, 2, TEXT("IronPlate"), 60.0 });   // cross-cluster
	Edges.Add({ 3, 2, TEXT("IronPlate"), 20.0 });   // cross-cluster, same item -> accumulates
	Edges.Add({ 2, 99, TEXT("Screw"), 10.0 });      // dangling (unknown machine) -> dropped

	const TArray<FAIDALogisticsFlow> Flows = FAIDAFactoryAggregator::BuildLogistics(Edges, IdToCluster);
	TestEqual(TEXT("one aggregated inter-cluster flow"), Flows.Num(), 1);
	if (Flows.Num() == 1)
	{
		TestEqual(TEXT("from cluster 0"), Flows[0].FromCluster, 0);
		TestEqual(TEXT("to cluster 1"), Flows[0].ToCluster, 1);
		TestEqual(TEXT("item IronPlate"), Flows[0].Item, FString(TEXT("IronPlate")));
		AIDA_TEST_NEAR("60 + 20 = 80/min", Flows[0].PerMinute, 80.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAFactoryPowerTest, "AIDA.Factory.Power",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAFactoryPowerTest::RunTest(const FString&)
{
	TArray<FAIDAMachine> Machines;
	FAIDAMachine A = AIDAMakeTestMachine(1, FVector::ZeroVector); A.CircuitId = 1; A.PowerMW = 60.0;
	FAIDAMachine B = AIDAMakeTestMachine(2, FVector::ZeroVector); B.CircuitId = 1; B.PowerMW = 30.0;
	FAIDAMachine Gen = AIDAMakeTestMachine(3, FVector::ZeroVector); Gen.CircuitId = 1; Gen.PowerMW = -50.0; // generator: not a draw
	FAIDAMachine C = AIDAMakeTestMachine(4, FVector::ZeroVector); C.CircuitId = 2; C.PowerMW = 70.0;
	Machines = { A, B, Gen, C };

	TArray<FAIDAPowerCircuitStats> Circuits;
	Circuits.Add({ 1, /*Produced*/ 90.0, /*Capacity*/ 100.0, /*Battery*/ 0.0, /*Drain*/ -1.0 });
	Circuits.Add({ 2, /*Produced*/ 20.0, /*Capacity*/ 50.0,  /*Battery*/ 0.0, /*Drain*/ -1.0 });

	const TArray<FAIDAPowerReport> Reports = FAIDAFactoryAggregator::BuildPowerReport(Machines, Circuits);
	TestEqual(TEXT("two circuits, sorted by id"), Reports.Num(), 2);
	if (Reports.Num() == 2)
	{
		TestEqual(TEXT("[0] circuit 1"), Reports[0].CircuitId, 1);
		AIDA_TEST_NEAR("circuit 1 draw excludes generator", Reports[0].ConsumedMW, 90.0);
		AIDA_TEST_NEAR("circuit 1 capacity", Reports[0].CapacityMW, 100.0);
		AIDA_TEST_NEAR("circuit 1 headroom", Reports[0].Headroom(), 10.0);
		TestFalse(TEXT("circuit 1 not overloaded"), Reports[0].IsOverloaded());

		TestEqual(TEXT("[1] circuit 2"), Reports[1].CircuitId, 2);
		AIDA_TEST_NEAR("circuit 2 draw", Reports[1].ConsumedMW, 70.0);
		TestTrue(TEXT("circuit 2 overloaded (70 > 50)"), Reports[1].IsOverloaded());
	}
	return true;
}

static TSharedPtr<FJsonObject> AIDAParseTestJson(const FString& Json)
{
	TSharedPtr<FJsonObject> Obj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	FJsonSerializer::Deserialize(Reader, Obj);
	return Obj;
}

/** A small synthetic aggregate: one cluster exporting IronPlate, an IronOre deficit, one healthy circuit. */
static FAIDAFactoryAggregates AIDAMakeTestAggregates()
{
	FAIDAFactoryAggregates Agg;
	FAIDACluster C;
	C.Id = 0;
	C.Centroid = FVector(1000.0, 2000.0, 0.0); // -> 10 m, 20 m
	C.Efficiency = 0.75f;
	C.MachineIds = { 1, 2 };
	C.BuildingCensus.Add(TEXT("Smelter"), 1);
	C.BuildingCensus.Add(TEXT("Constructor"), 1);
	C.NetInputs = { { TEXT("IronOre"), 30.0 } };
	C.NetOutputs = { { TEXT("IronPlate"), 20.0 } };
	Agg.Clusters.Add(C);

	Agg.Balance.Add({ TEXT("IronOre"), 0.0, 30.0 });   // deficit 30
	Agg.Balance.Add({ TEXT("IronPlate"), 20.0, 0.0 }); // surplus 20

	FAIDAPowerReport P;
	P.CircuitId = 1; P.ProducedMW = 90.0; P.CapacityMW = 100.0; P.ConsumedMW = 90.0;
	Agg.Power.Add(P);
	return Agg;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAFactoryToolsOverviewTest, "AIDA.FactoryTools.Overview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAFactoryToolsOverviewTest::RunTest(const FString&)
{
	const TSharedPtr<FJsonObject> Root = AIDAParseTestJson(AIDAFactoryTools::BuildOverviewJson(AIDAMakeTestAggregates()));
	if (!TestNotNull(TEXT("overview parses as JSON"), Root.Get())) { return false; }

	TestEqual(TEXT("cluster count"), Root->GetIntegerField(TEXT("clusters")), 1);
	TestEqual(TEXT("machine count"), Root->GetIntegerField(TEXT("machines")), 2);

	const TArray<TSharedPtr<FJsonValue>>& List = Root->GetArrayField(TEXT("clusterList"));
	TestEqual(TEXT("one cluster listed"), List.Num(), 1);
	if (List.Num() == 1)
	{
		const TSharedPtr<FJsonObject> C = List[0]->AsObject();
		TestEqual(TEXT("cluster id"), C->GetIntegerField(TEXT("id")), 0);
		TestEqual(TEXT("primary output"), C->GetStringField(TEXT("primaryOutput")), FString(TEXT("IronPlate")));
	}

	const TArray<TSharedPtr<FJsonValue>>& Deficits = Root->GetArrayField(TEXT("topDeficits"));
	TestEqual(TEXT("one deficit"), Deficits.Num(), 1);
	if (Deficits.Num() == 1)
	{
		TestEqual(TEXT("deficit item"), Deficits[0]->AsObject()->GetStringField(TEXT("item")), FString(TEXT("IronOre")));
	}

	const TArray<TSharedPtr<FJsonValue>>& Power = Root->GetArrayField(TEXT("power"));
	TestEqual(TEXT("one circuit"), Power.Num(), 1);
	if (Power.Num() == 1)
	{
		TestFalse(TEXT("circuit not overloaded"), Power[0]->AsObject()->GetBoolField(TEXT("overloaded")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAFactoryToolsBalanceClusterTest, "AIDA.FactoryTools.BalanceAndCluster",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAFactoryToolsBalanceClusterTest::RunTest(const FString&)
{
	const FAIDAFactoryAggregates Agg = AIDAMakeTestAggregates();

	// Filter to a known item (case-insensitive).
	const TSharedPtr<FJsonObject> Filtered = AIDAParseTestJson(AIDAFactoryTools::BuildItemBalanceJson(Agg, TEXT("ironore")));
	if (TestNotNull(TEXT("filtered balance parses"), Filtered.Get()))
	{
		TestEqual(TEXT("item echoed"), Filtered->GetStringField(TEXT("item")), FString(TEXT("IronOre")));
		TestTrue(TEXT("flagged as deficit"), Filtered->GetBoolField(TEXT("deficit")));
	}

	// Unknown item -> error object.
	const TSharedPtr<FJsonObject> Missing = AIDAParseTestJson(AIDAFactoryTools::BuildItemBalanceJson(Agg, TEXT("Nonexistium")));
	TestTrue(TEXT("unknown item yields an error"), Missing.IsValid() && Missing->HasField(TEXT("error")));

	// Valid cluster.
	const TSharedPtr<FJsonObject> Cluster = AIDAParseTestJson(AIDAFactoryTools::BuildClusterJson(Agg, 0));
	if (TestNotNull(TEXT("cluster parses"), Cluster.Get()))
	{
		TestEqual(TEXT("cluster id"), Cluster->GetIntegerField(TEXT("id")), 0);
		TestEqual(TEXT("machine count"), Cluster->GetIntegerField(TEXT("machines")), 2);
		const TSharedPtr<FJsonObject> Census = Cluster->GetObjectField(TEXT("census"));
		TestEqual(TEXT("census Smelter"), Census->GetIntegerField(TEXT("Smelter")), 1);
	}

	// Unknown cluster -> error object.
	const TSharedPtr<FJsonObject> BadCluster = AIDAParseTestJson(AIDAFactoryTools::BuildClusterJson(Agg, 99));
	TestTrue(TEXT("unknown cluster yields an error"), BadCluster.IsValid() && BadCluster->HasField(TEXT("error")));
	return true;
}

#undef AIDA_TEST_NEAR

#endif // WITH_AUTOMATION_TESTS
