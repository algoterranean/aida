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
#include "Tools/AIDAMapTools.h"
#include "Tools/AIDARecipeTools.h"
#include "Memory/AIDASidecarStore.h"
#include "Tools/AIDANotesTools.h"
#include "Tools/AIDASnapshotTools.h"
#include "UI/AIDAMarkdown.h"
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

	const FGuid PlayerId = Session.PostPlayerMessage(TEXT("Alice"), TEXT("hello"), AIDADefaultConversationId());
	const FGuid AidaId = Session.BeginAIDAMessage(TEXT("AIDA"), AIDADefaultConversationId());
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

	const FGuid First = Session.PostPlayerMessage(TEXT("A"), TEXT("1"), AIDADefaultConversationId());
	Session.PostPlayerMessage(TEXT("B"), TEXT("2"), AIDADefaultConversationId());
	Session.PostPlayerMessage(TEXT("C"), TEXT("3"), AIDADefaultConversationId());

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

	// An "everyone" entry in the act allowlist opens the act tier to ALL players — including an
	// unidentified host (empty net id on a listen-server/offline session).
	FAIDAPermissionsConfig OpenAct;
	OpenAct.Act = { TEXT("everyone") };
	FAIDAPermissionService OpenActService(OpenAct);
	TestTrue(TEXT("act 'everyone' allows any id"), OpenActService.IsAllowed(EAIDATier::Act, TEXT("rando")));
	TestTrue(TEXT("act 'everyone' allows empty host id"), OpenActService.IsAllowed(EAIDATier::Act, FString()));
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

// Regression: a no-arg tool passes an empty schema; providers require input_schema.type, so it must
// default to an object schema (Anthropic rejected the empty {} with "input_schema.type: Field required").
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDANoArgToolSchemaTest, "AIDA.Adapters.NoArgToolSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDANoArgToolSchemaTest::RunTest(const FString&)
{
	FAIDACompletionRequest Req;
	Req.Model = TEXT("test-model");
	Req.MaxTokens = 128;
	Req.Tools.Add({ TEXT("get_factory_overview"), TEXT("overview"), TEXT("") }); // empty schema
	Req.Messages.Add({ TEXT("user"), TEXT("hi") });

	const TSharedPtr<FJsonObject> Json = ParseJsonObject(FAnthropicAdapter::BuildRequestBody(Req));
	const TArray<TSharedPtr<FJsonValue>>* ToolsArr = nullptr;
	if (!TestTrue(TEXT("tools array of 1"), Json.IsValid() && Json->TryGetArrayField(TEXT("tools"), ToolsArr) && ToolsArr && ToolsArr->Num() == 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Schema = nullptr;
	if (TestTrue(TEXT("input_schema is an object"), (*ToolsArr)[0]->AsObject()->TryGetObjectField(TEXT("input_schema"), Schema) && Schema))
	{
		TestEqual(TEXT("input_schema.type defaults to object"), (*Schema)->GetStringField(TEXT("type")), FString(TEXT("object")));
	}
	return true;
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
	// Reports come straight from circuit stats now (authoritative consumed), incl. circuit id 0.
	TArray<FAIDAPowerCircuitStats> Circuits;
	Circuits.Add({ 0, /*Produced*/ 90.0, /*Capacity*/ 100.0, /*Consumed*/ 90.0, /*Battery*/ 0.0, /*Drain*/ -1.0 });
	Circuits.Add({ 2, /*Produced*/ 20.0, /*Capacity*/ 50.0,  /*Consumed*/ 70.0, /*Battery*/ 0.0, /*Drain*/ -1.0 });

	const TArray<FAIDAPowerReport> Reports = FAIDAFactoryAggregator::BuildPowerReport(Circuits);
	TestEqual(TEXT("two circuits, sorted by id"), Reports.Num(), 2);
	if (Reports.Num() == 2)
	{
		TestEqual(TEXT("[0] circuit id 0 kept"), Reports[0].CircuitId, 0);
		AIDA_TEST_NEAR("circuit 0 consumed", Reports[0].ConsumedMW, 90.0);
		AIDA_TEST_NEAR("circuit 0 capacity", Reports[0].CapacityMW, 100.0);
		AIDA_TEST_NEAR("circuit 0 headroom", Reports[0].Headroom(), 10.0);
		TestFalse(TEXT("circuit 0 not overloaded"), Reports[0].IsOverloaded());

		TestEqual(TEXT("[1] circuit 2"), Reports[1].CircuitId, 2);
		AIDA_TEST_NEAR("circuit 2 consumed", Reports[1].ConsumedMW, 70.0);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAFactoryBottleneckTest, "AIDA.Factory.Bottleneck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAFactoryBottleneckTest::RunTest(const FString&)
{
	// Upstream: a constructor makes IronPlate from IronIngot, but nothing makes IronIngot -> starved.
	{
		FAIDAMachine Ctor = AIDAMakeTestMachine(1, FVector::ZeroVector, TEXT("Constructor"));
		Ctor.Inputs = { { TEXT("IronIngot"), 30.0 } };
		Ctor.Outputs = { { TEXT("IronPlate"), 20.0 } };
		Ctor.bProducing = false;
		FAIDAFactorySnapshot Snap; Snap.Machines = { Ctor };

		const FAIDABottleneckResult R = FAIDAFactoryAggregator::FindBottleneck(Snap, TEXT("IronPlate"));
		TestTrue(TEXT("upstream-limited"), R.Kind == EAIDABottleneck::Upstream);
		TestEqual(TEXT("limiting input is IronIngot"), R.LimitingDetail, FString(TEXT("IronIngot")));
		TestEqual(TEXT("one starved input"), R.StarvedInputs.Num(), 1);

		const TSharedPtr<FJsonObject> J = AIDAParseTestJson(AIDAFactoryTools::BuildBottleneckJson(R));
		if (TestNotNull(TEXT("bottleneck json parses"), J.Get()))
		{
			TestEqual(TEXT("status"), J->GetStringField(TEXT("status")), FString(TEXT("starved_upstream")));
		}
	}

	// No producers: something burns Coal but nothing mines it.
	{
		FAIDAMachine Gen = AIDAMakeTestMachine(2, FVector::ZeroVector, TEXT("CoalGenerator"));
		Gen.Inputs = { { TEXT("Coal"), 15.0 } };
		FAIDAFactorySnapshot Snap; Snap.Machines = { Gen };
		const FAIDABottleneckResult R = FAIDAFactoryAggregator::FindBottleneck(Snap, TEXT("Coal"));
		TestTrue(TEXT("no producers"), R.Kind == EAIDABottleneck::NoProducers);
	}

	// Unknown item: not produced or consumed anywhere.
	{
		FAIDAMachine M = AIDAMakeTestMachine(3, FVector::ZeroVector, TEXT("Smelter"));
		M.Outputs = { { TEXT("IronIngot"), 30.0 } };
		FAIDAFactorySnapshot Snap; Snap.Machines = { M };
		const FAIDABottleneckResult R = FAIDAFactoryAggregator::FindBottleneck(Snap, TEXT("Plutonium"));
		TestTrue(TEXT("unknown item"), R.Kind == EAIDABottleneck::UnknownItem);
	}

	// Power: inputs are satisfied, but the producer's circuit is over capacity.
	{
		FAIDAMachine Miner = AIDAMakeTestMachine(4, FVector::ZeroVector, TEXT("Miner"));
		Miner.Outputs = { { TEXT("IronOre"), 30.0 } };
		FAIDAMachine Smelter = AIDAMakeTestMachine(5, FVector::ZeroVector, TEXT("Smelter"));
		Smelter.Inputs = { { TEXT("IronOre"), 30.0 } };
		Smelter.Outputs = { { TEXT("IronIngot"), 30.0 } };
		Smelter.bProducing = true;
		Smelter.CircuitId = 1;
		Smelter.PowerMW = 10.0;
		FAIDAFactorySnapshot Snap;
		Snap.Machines = { Miner, Smelter };
		Snap.Circuits.Add({ 1, /*Produced*/ 5.0, /*Capacity*/ 5.0, /*Consumed*/ 10.0, /*Battery*/ 0.0, /*Drain*/ -1.0 });

		const FAIDABottleneckResult R = FAIDAFactoryAggregator::FindBottleneck(Snap, TEXT("IronIngot"));
		TestTrue(TEXT("power-limited"), R.Kind == EAIDABottleneck::Power);
		TestEqual(TEXT("overloaded circuit id"), R.LimitingDetail, FString(TEXT("1")));
	}
	return true;
}

static FAIDAResourceNode AIDAMakeTestNode(const FString& Resource, const FString& Purity, bool bOccupied)
{
	FAIDAResourceNode N;
	N.Resource = Resource;
	N.Purity = Purity;
	N.bOccupied = bOccupied;
	return N;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAMapToolsNodesTest, "AIDA.MapTools.ResourceNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAMapToolsNodesTest::RunTest(const FString&)
{
	TArray<FAIDAResourceNode> Nodes;
	Nodes.Add(AIDAMakeTestNode(TEXT("Iron Ore"), TEXT("Pure"), /*occupied*/ true));
	Nodes.Add(AIDAMakeTestNode(TEXT("Iron Ore"), TEXT("Pure"), false));
	Nodes.Add(AIDAMakeTestNode(TEXT("Iron Ore"), TEXT("Normal"), false));
	Nodes.Add(AIDAMakeTestNode(TEXT("Copper Ore"), TEXT("Impure"), false));

	// Unfiltered: 4 nodes, 3 free, grouped into 3 (resource,purity) buckets sorted by key.
	{
		const TSharedPtr<FJsonObject> Root = AIDAParseTestJson(AIDAMapTools::BuildResourceNodesJson(Nodes, FString(), false));
		if (!TestNotNull(TEXT("nodes json parses"), Root.Get())) { return false; }
		TestEqual(TEXT("total nodes"), Root->GetIntegerField(TEXT("nodes")), 4);
		TestEqual(TEXT("free nodes"), Root->GetIntegerField(TEXT("free")), 3);

		const TArray<TSharedPtr<FJsonValue>>& By = Root->GetArrayField(TEXT("byResource"));
		TestEqual(TEXT("three buckets"), By.Num(), 3);
		if (By.Num() == 3)
		{
			// Sorted keys: "Copper Ore|Impure", "Iron Ore|Normal", "Iron Ore|Pure".
			TestEqual(TEXT("first bucket resource"), By[0]->AsObject()->GetStringField(TEXT("resource")), FString(TEXT("Copper Ore")));
			const TSharedPtr<FJsonObject> IronPure = By[2]->AsObject();
			TestEqual(TEXT("iron/pure total"), IronPure->GetIntegerField(TEXT("total")), 2);
			TestEqual(TEXT("iron/pure free"), IronPure->GetIntegerField(TEXT("free")), 1);
		}
		TestFalse(TEXT("no freeNodes list unless untapped_only"), Root->HasField(TEXT("freeNodes")));
	}

	// Resource filter (case-insensitive substring): only the 3 iron nodes.
	{
		const TSharedPtr<FJsonObject> Root = AIDAParseTestJson(AIDAMapTools::BuildResourceNodesJson(Nodes, TEXT("iron"), false));
		TestEqual(TEXT("filtered to iron"), Root->GetIntegerField(TEXT("nodes")), 3);
	}

	// Untapped-only: 3 free nodes and a listed freeNodes array.
	{
		const TSharedPtr<FJsonObject> Root = AIDAParseTestJson(AIDAMapTools::BuildResourceNodesJson(Nodes, FString(), true));
		TestEqual(TEXT("untapped count"), Root->GetIntegerField(TEXT("nodes")), 3);
		TestTrue(TEXT("freeNodes listed"), Root->HasField(TEXT("freeNodes")));
		TestEqual(TEXT("freeNodes length"), Root->GetArrayField(TEXT("freeNodes")).Num(), 3);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAMapToolsNearestTest, "AIDA.MapTools.FindNearestUntapped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAMapToolsNearestTest::RunTest(const FString&)
{
	TArray<FAIDAResourceNode> Nodes;
	auto MakeAt = [](const FString& Res, const FString& Pur, bool bOcc, const FVector& Loc)
	{
		FAIDAResourceNode N; N.Resource = Res; N.Purity = Pur; N.bOccupied = bOcc; N.Location = Loc; return N;
	};
	Nodes.Add(MakeAt(TEXT("Iron Ore"), TEXT("Pure"),   true,  FVector(100, 0, 0))); // occupied — skipped
	Nodes.Add(MakeAt(TEXT("Iron Ore"), TEXT("Normal"), false, FVector(1000, 0, 0)));
	Nodes.Add(MakeAt(TEXT("Iron Ore"), TEXT("Pure"),   false, FVector(500, 0, 0)));
	Nodes.Add(MakeAt(TEXT("Copper Ore"), TEXT("Impure"), false, FVector(50, 0, 0)));

	// Nearest untapped iron to the origin is the Pure one at 500 (the occupied 100 is skipped).
	{
		const FAIDAResourceNode* N = AIDAMapTools::FindNearestUntapped(Nodes, TEXT("iron"), FString(), FVector::ZeroVector, true);
		if (!TestNotNull(TEXT("found a node"), N)) { return false; }
		TestEqual(TEXT("nearest untapped iron"), N->Location.X, 500.0);
	}

	// Purity filter forces the Normal node even though the Pure one is closer.
	{
		const FAIDAResourceNode* N = AIDAMapTools::FindNearestUntapped(Nodes, TEXT("iron"), TEXT("Normal"), FVector::ZeroVector, true);
		if (!TestNotNull(TEXT("found normal iron"), N)) { return false; }
		TestEqual(TEXT("normal iron picked"), N->Location.X, 1000.0);
	}

	// No origin → first matching untapped node in scan order (the Normal at 1000).
	{
		const FAIDAResourceNode* N = AIDAMapTools::FindNearestUntapped(Nodes, TEXT("iron"), FString(), FVector::ZeroVector, false);
		if (!TestNotNull(TEXT("found without origin"), N)) { return false; }
		TestEqual(TEXT("first untapped iron"), N->Location.X, 1000.0);
	}

	// No untapped match → nullptr.
	{
		const FAIDAResourceNode* N = AIDAMapTools::FindNearestUntapped(Nodes, TEXT("Bauxite"), FString(), FVector::ZeroVector, true);
		TestNull(TEXT("no bauxite"), N);
	}
	return true;
}

// A small synthetic catalog for the static-reference serializers (no game headers required).
static TArray<FAIDARecipeInfo> AIDAMakeTestRecipes()
{
	TArray<FAIDARecipeInfo> Recipes;

	FAIDARecipeInfo Plate;
	Plate.RecipeName = TEXT("Iron Plate");
	Plate.DurationSeconds = 6.0;                       // 20/min out from 30/min in
	Plate.Ingredients.Add({ TEXT("Iron Ingot"), 3.0 });
	Plate.Products.Add({ TEXT("Iron Plate"), 2.0 });
	Plate.ProducedIn.Add(TEXT("Constructor"));
	Recipes.Add(MoveTemp(Plate));

	FAIDARecipeInfo Rip;
	Rip.RecipeName = TEXT("Reinforced Iron Plate");
	Rip.DurationSeconds = 12.0;
	Rip.Ingredients.Add({ TEXT("Iron Plate"), 6.0 });
	Rip.Ingredients.Add({ TEXT("Screw"), 12.0 });
	Rip.Products.Add({ TEXT("Reinforced Iron Plate"), 1.0 });
	Rip.ProducedIn.Add(TEXT("Assembler"));
	Recipes.Add(MoveTemp(Rip));

	return Recipes;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDARecipeToolsRecipeTest, "AIDA.RecipeTools.LookupRecipe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDARecipeToolsRecipeTest::RunTest(const FString&)
{
	const TArray<FAIDARecipeInfo> Recipes = AIDAMakeTestRecipes();

	// Filter by product name (case-insensitive substring). "reinforced" -> just the RIP recipe.
	{
		const TSharedPtr<FJsonObject> Root = AIDAParseTestJson(AIDARecipeTools::BuildRecipeJson(Recipes, TEXT("reinforced")));
		if (!TestNotNull(TEXT("recipe json parses"), Root.Get())) { return false; }
		TestEqual(TEXT("one match"), Root->GetIntegerField(TEXT("matches")), 1);
		const TArray<TSharedPtr<FJsonValue>>& List = Root->GetArrayField(TEXT("recipes"));
		if (!TestEqual(TEXT("one recipe listed"), List.Num(), 1)) { return false; }
		const TSharedPtr<FJsonObject> R = List[0]->AsObject();
		TestEqual(TEXT("recipe name"), R->GetStringField(TEXT("recipe")), FString(TEXT("Reinforced Iron Plate")));
		TestEqual(TEXT("two inputs"), R->GetArrayField(TEXT("inputs")).Num(), 2);
		const TSharedPtr<FJsonObject> Out0 = R->GetArrayField(TEXT("outputs"))[0]->AsObject();
		// 1 per craft over 12 s -> 5/min.
		TestEqual(TEXT("output perMin"), Out0->GetNumberField(TEXT("perMin")), 5.0);
		TestEqual(TEXT("producedIn"), R->GetArrayField(TEXT("producedIn"))[0]->AsString(), FString(TEXT("Assembler")));
	}

	// A product-name substring shared by both recipes ("iron plate") matches both.
	{
		const TSharedPtr<FJsonObject> Root = AIDAParseTestJson(AIDARecipeTools::BuildRecipeJson(Recipes, TEXT("iron plate")));
		TestEqual(TEXT("two matches"), Root->GetIntegerField(TEXT("matches")), 2);
	}

	// Empty filter lists everything.
	{
		const TSharedPtr<FJsonObject> Root = AIDAParseTestJson(AIDARecipeTools::BuildRecipeJson(Recipes, FString()));
		TestEqual(TEXT("all recipes"), Root->GetIntegerField(TEXT("matches")), 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDARecipeToolsBuildingTest, "AIDA.RecipeTools.LookupBuilding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDARecipeToolsBuildingTest::RunTest(const FString&)
{
	TArray<FAIDABuildingInfo> Buildings;
	Buildings.Add({ TEXT("Constructor"), 4.0, false, 0.0, 0.0, 0.0 });
	FAIDABuildingInfo Miner; Miner.Name = TEXT("Miner Mk.1"); Miner.bVariablePower = true; Miner.MinPowerMW = 5.0; Miner.MaxPowerMW = 5.0;
	Buildings.Add(MoveTemp(Miner));
	FAIDABuildingInfo Gen; Gen.Name = TEXT("Coal Generator"); Gen.PowerProductionMW = 75.0;
	Buildings.Add(MoveTemp(Gen));

	// Fixed-power building: reports powerMW, not min/max.
	{
		const TSharedPtr<FJsonObject> Root = AIDAParseTestJson(AIDARecipeTools::BuildBuildingJson(Buildings, TEXT("construct")));
		if (!TestNotNull(TEXT("building json parses"), Root.Get())) { return false; }
		TestEqual(TEXT("one match"), Root->GetIntegerField(TEXT("matches")), 1);
		const TSharedPtr<FJsonObject> B = Root->GetArrayField(TEXT("buildings"))[0]->AsObject();
		TestEqual(TEXT("power"), B->GetNumberField(TEXT("powerMW")), 4.0);
		TestFalse(TEXT("no minPower for fixed"), B->HasField(TEXT("minPowerMW")));
	}

	// Generator: reports powerProductionMW.
	{
		const TSharedPtr<FJsonObject> Root = AIDAParseTestJson(AIDARecipeTools::BuildBuildingJson(Buildings, TEXT("generator")));
		const TSharedPtr<FJsonObject> B = Root->GetArrayField(TEXT("buildings"))[0]->AsObject();
		TestEqual(TEXT("production"), B->GetNumberField(TEXT("powerProductionMW")), 75.0);
	}

	// Empty filter lists all three.
	{
		const TSharedPtr<FJsonObject> Root = AIDAParseTestJson(AIDARecipeTools::BuildBuildingJson(Buildings, FString()));
		TestEqual(TEXT("all buildings"), Root->GetIntegerField(TEXT("matches")), 3);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDASnapshotCompareTest, "AIDA.Snapshot.Compare",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDASnapshotCompareTest::RunTest(const FString&)
{
	// MakeSnapshot flattens aggregates: per-item net + summed power.
	FAIDAFactoryAggregates Agg;
	Agg.Balance.Add({ TEXT("Iron Plate"), 120.0, 210.0 }); // net -90
	Agg.Balance.Add({ TEXT("Screw"), 100.0, 60.0 });       // net  40
	Agg.Power.Add({ /*CircuitId*/ 0, /*Produced*/ 5700.0, /*Capacity*/ 5700.0, /*Consumed*/ 2861.0 });
	const FAIDASnapshot Base = AIDASnapshotTools::MakeSnapshot(Agg, 1000, TEXT("before"));
	TestEqual(TEXT("snapshot items"), Base.ItemBalance.Num(), 2);
	TestEqual(TEXT("snapshot power consumed"), Base.PowerConsumedMW, 2861.0);

	// Current state: iron improved to net 0 (+90), screw unchanged, power up 100.
	FAIDAFactoryAggregates Now;
	Now.Balance.Add({ TEXT("Iron Plate"), 210.0, 210.0 }); // net 0
	Now.Balance.Add({ TEXT("Screw"), 100.0, 60.0 });       // net 40 (unchanged)
	Now.Power.Add({ 0, 5700.0, 5700.0, 2961.0 });
	const FAIDASnapshot Cur = AIDASnapshotTools::MakeSnapshot(Now, 2000, TEXT("now"));

	const TSharedPtr<FJsonObject> Root = AIDAParseTestJson(AIDASnapshotTools::BuildCompareJson(Base, Cur, FString(), 2000));
	if (!TestNotNull(TEXT("compare json parses"), Root.Get())) { return false; }
	// Only iron changed (screw delta 0 is dropped).
	TestEqual(TEXT("one changed item"), Root->GetIntegerField(TEXT("changedItems")), 1);
	const TArray<TSharedPtr<FJsonValue>>& Items = Root->GetArrayField(TEXT("items"));
	if (!TestEqual(TEXT("one item listed"), Items.Num(), 1)) { return false; }
	const TSharedPtr<FJsonObject> Iron = Items[0]->AsObject();
	TestEqual(TEXT("iron item"), Iron->GetStringField(TEXT("item")), FString(TEXT("Iron Plate")));
	TestEqual(TEXT("iron was -90"), Iron->GetNumberField(TEXT("was")), -90.0);
	TestEqual(TEXT("iron now 0"), Iron->GetNumberField(TEXT("now")), 0.0);
	TestEqual(TEXT("iron delta +90"), Iron->GetNumberField(TEXT("delta")), 90.0);
	TestEqual(TEXT("power consumed delta"), Root->GetObjectField(TEXT("power"))->GetNumberField(TEXT("consumedDeltaMW")), 100.0);

	// PickBaseline: no timestamp -> most recent; with timestamp -> newest at/or-before.
	TArray<FAIDASnapshot> Snaps;
	{ FAIDASnapshot S; S.TakenUtc = 100; Snaps.Add(S); }
	{ FAIDASnapshot S; S.TakenUtc = 200; Snaps.Add(S); }
	{ FAIDASnapshot S; S.TakenUtc = 300; Snaps.Add(S); }
	TestEqual(TEXT("no ts -> latest"), AIDASnapshotTools::PickBaseline(Snaps, 0, false)->TakenUtc, (int64)300);
	TestEqual(TEXT("ts=250 -> 200"), AIDASnapshotTools::PickBaseline(Snaps, 250, true)->TakenUtc, (int64)200);
	TestEqual(TEXT("ts before all -> earliest"), AIDASnapshotTools::PickBaseline(Snaps, 50, true)->TakenUtc, (int64)100);
	TestNull(TEXT("empty -> null"), AIDASnapshotTools::PickBaseline(TArray<FAIDASnapshot>(), 0, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDANotesSelectTest, "AIDA.Notes.Select",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDANotesSelectTest::RunTest(const FString&)
{
	TArray<FAIDANote> Notes;
	auto Make = [](const FString& Text, const FString& Region, int64 Utc, const FVector& Loc, const TArray<FString>& Tags)
	{
		FAIDANote N; N.Text = Text; N.Region = Region; N.CreatedUtc = Utc; N.Location = Loc; N.Tags = Tags; return N;
	};
	Notes.Add(Make(TEXT("build more power at the hub"), TEXT("Rocky Desert"), 100, FVector(1000, 0, 0), { TEXT("power") }));
	Notes.Add(Make(TEXT("expand steel here"), TEXT("Grass Fields"), 300, FVector(100, 0, 0), { TEXT("steel"), TEXT("todo") }));
	Notes.Add(Make(TEXT("oil outpost plan"), TEXT("Grass Fields"), 200, FVector(5000, 0, 0), {}));

	// Keyword matches text.
	{
		const TArray<const FAIDANote*> S = AIDANotesTools::SelectNotes(Notes, TEXT("power"), FString(), FVector::ZeroVector, false);
		if (!TestEqual(TEXT("power keyword -> 1"), S.Num(), 1)) { return false; }
		TestEqual(TEXT("matched note"), S[0]->Text, FString(TEXT("build more power at the hub")));
	}
	// Keyword matches a tag ("todo" is only a tag).
	{
		const TArray<const FAIDANote*> S = AIDANotesTools::SelectNotes(Notes, TEXT("todo"), FString(), FVector::ZeroVector, false);
		TestEqual(TEXT("todo tag -> 1"), S.Num(), 1);
	}
	// Region filter.
	{
		const TArray<const FAIDANote*> S = AIDANotesTools::SelectNotes(Notes, FString(), TEXT("grass"), FVector::ZeroVector, false);
		TestEqual(TEXT("grass fields -> 2"), S.Num(), 2);
	}
	// Default sort = newest first (Utc desc): 300, 200, 100.
	{
		const TArray<const FAIDANote*> S = AIDANotesTools::SelectNotes(Notes, FString(), FString(), FVector::ZeroVector, false);
		if (!TestEqual(TEXT("all -> 3"), S.Num(), 3)) { return false; }
		TestEqual(TEXT("newest first"), S[0]->CreatedUtc, (int64)300);
		TestEqual(TEXT("oldest last"), S[2]->CreatedUtc, (int64)100);
	}
	// near sort = nearest to origin first: steel(100) < power(1000) < oil(5000).
	{
		const TArray<const FAIDANote*> S = AIDANotesTools::SelectNotes(Notes, FString(), FString(), FVector::ZeroVector, true);
		if (!TestEqual(TEXT("all -> 3"), S.Num(), 3)) { return false; }
		TestEqual(TEXT("nearest first"), S[0]->Location.X, 100.0);
		TestEqual(TEXT("farthest last"), S[2]->Location.X, 5000.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDASidecarSnapshotTest, "AIDA.Memory.SidecarSnapshotRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDASidecarSnapshotTest::RunTest(const FString&)
{
	FAIDASnapshot Snap;
	Snap.TakenUtc = 1752600000;
	Snap.Label = TEXT("before boss");
	Snap.PowerConsumedMW = 2861.4;
	Snap.PowerCapacityMW = 5700.0;
	Snap.ItemBalance.Add({ TEXT("Iron Plate"), -90.0 });
	Snap.ItemBalance.Add({ TEXT("Screw"), 40.0 });

	const FString Json = FAIDASidecarStore::SnapshotToJson(Snap);
	TestTrue(TEXT("json is one line (no newline)"), !Json.Contains(TEXT("\n")));

	FAIDASnapshot Back;
	if (!TestTrue(TEXT("parses back"), FAIDASidecarStore::SnapshotFromJson(Json, Back))) { return false; }
	TestEqual(TEXT("t"), Back.TakenUtc, (int64)1752600000);
	TestEqual(TEXT("label"), Back.Label, FString(TEXT("before boss")));
	TestEqual(TEXT("power consumed"), Back.PowerConsumedMW, 2861.4);
	TestEqual(TEXT("items count"), Back.ItemBalance.Num(), 2);
	if (Back.ItemBalance.Num() == 2)
	{
		TestEqual(TEXT("item 0 name"), Back.ItemBalance[0].Item, FString(TEXT("Iron Plate")));
		TestEqual(TEXT("item 0 net"), Back.ItemBalance[0].Net, -90.0);
	}

	// Malformed input is rejected, not crashed.
	FAIDASnapshot Junk;
	TestFalse(TEXT("empty line rejected"), FAIDASidecarStore::SnapshotFromJson(TEXT("   "), Junk));
	TestFalse(TEXT("garbage rejected"), FAIDASidecarStore::SnapshotFromJson(TEXT("not json"), Junk));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDASidecarRingBufferTest, "AIDA.Memory.SidecarRingBuffer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDASidecarRingBufferTest::RunTest(const FString&)
{
	TArray<FString> Lines;
	for (int32 i = 1; i <= 5; ++i)
	{
		FAIDASidecarStore::AppendWithRingBuffer(Lines, FString::Printf(TEXT("line%d"), i), /*KeepMax*/ 3);
	}
	// Only the newest 3 survive, oldest-first.
	if (!TestEqual(TEXT("kept 3"), Lines.Num(), 3)) { return false; }
	TestEqual(TEXT("oldest kept is line3"), Lines[0], FString(TEXT("line3")));
	TestEqual(TEXT("newest is line5"), Lines[2], FString(TEXT("line5")));

	// KeepMax <= 0 is unbounded.
	TArray<FString> Unbounded;
	for (int32 i = 0; i < 10; ++i) { FAIDASidecarStore::AppendWithRingBuffer(Unbounded, TEXT("x"), 0); }
	TestEqual(TEXT("unbounded keeps all"), Unbounded.Num(), 10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAMarkdownTest, "AIDA.Markdown.Convert",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAMarkdownTest::RunTest(const FString&)
{
	TestEqual(TEXT("bold"), AIDAMarkdownToRichText(TEXT("a **b** c")), FString(TEXT("a <Bold>b</> c")));
	TestEqual(TEXT("italic"), AIDAMarkdownToRichText(TEXT("a *b* c")), FString(TEXT("a <Italic>b</> c")));
	TestEqual(TEXT("code span"), AIDAMarkdownToRichText(TEXT("run `x` now")), FString(TEXT("run <Code>x</> now")));
	TestEqual(TEXT("header strips hashes"), AIDAMarkdownToRichText(TEXT("## Title")), FString(TEXT("<Header>Title</>")));
	TestEqual(TEXT("bullet"), AIDAMarkdownToRichText(TEXT("- item")), FString(TEXT("  • item")));
	TestEqual(TEXT("unbalanced left as-is"), AIDAMarkdownToRichText(TEXT("2 ** 3")), FString(TEXT("2 ** 3")));
	// Code before bold: markers inside a code span are not treated as formatting.
	TestEqual(TEXT("code protects markers"), AIDAMarkdownToRichText(TEXT("`a**b`")), FString(TEXT("<Code>a**b</>")));
	// Two lines preserved.
	TestEqual(TEXT("newline preserved"), AIDAMarkdownToRichText(TEXT("a\nb")), FString(TEXT("a\nb")));

	// Table: separator row dropped, pipes removed, header bold, cell data preserved.
	{
		const FString T = AIDAMarkdownToRichText(TEXT("| Item | Net |\n|------|-----|\n| Iron | -90 |"));
		TestTrue(TEXT("table header styled"), T.Contains(TEXT("<MonoHeader>")));
		TestTrue(TEXT("table rows mono"), T.Contains(TEXT("<Mono>")));
		TestFalse(TEXT("table pipes removed"), T.Contains(TEXT("|")));
		TestFalse(TEXT("table separator dropped"), T.Contains(TEXT("---")));
		TestTrue(TEXT("table data kept"), T.Contains(TEXT("Iron")) && T.Contains(TEXT("-90")));
	}
	// Malformed table (bare separator, no header) still renders as a headerless mono table, no pipes.
	{
		const FString T = AIDAMarkdownToRichText(TEXT("|---|---|\n| Iron | -90 |\n| Wire | -360 |"));
		TestFalse(TEXT("bare table pipes removed"), T.Contains(TEXT("|")));
		TestTrue(TEXT("bare table rows mono"), T.Contains(TEXT("<Mono>")));
		TestFalse(TEXT("bare table has no header style"), T.Contains(TEXT("<MonoHeader>")));
	}
	return true;
}

#undef AIDA_TEST_NEAR

#endif // WITH_AUTOMATION_TESTS
