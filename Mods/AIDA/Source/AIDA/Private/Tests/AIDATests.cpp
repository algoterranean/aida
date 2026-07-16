#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Algo/Reverse.h"

#include "Core/AIDAConfigLoader.h"
#include "Core/AIDASessionManager.h"
#include "Core/AIDARateLimiter.h"
#include "Core/AIDAPermissionService.h"
#include "Core/AIDAImageStore.h"
#include "Adapters/AnthropicAdapter.h"
#include "Adapters/OpenAICompatAdapter.h"
#include "Adapters/LLMClient.h"
#include "Adapters/SSEStream.h"
#include "Misc/Base64.h"
#include "Tools/AIDAToolRegistry.h"
#include "Factory/AIDAFactoryAggregator.h"
#include "Factory/AIDALogisticsGraph.h"
#include "Tools/AIDAFactoryTools.h"
#include "Tools/AIDAMapTools.h"
#include "Tools/AIDARecipeTools.h"
#include "Recipes/AIDAFactoryPlanner.h"
#include "Tools/AIDAPromptPack.h"
#include "Memory/AIDASidecarStore.h"
#include "Tools/AIDANotesTools.h"
#include "Tools/AIDASnapshotTools.h"
#include "Tools/AIDAToolJson.h"
#include "Actions/AIDAActionSpec.h"
#include "Actions/AIDAChatCommands.h"
#include "Actions/AIDAProposalStore.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDALogisticsGraphTest, "AIDA.Factory.LogisticsGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDALogisticsGraphTest::RunTest(const FString&)
{
	using namespace AIDALogisticsGraph;

	// Three-segment chain node1 → [270, 60, 270] → node2 collapses to one edge at the slowest rate.
	{
		TArray<FSegment> Segments;
		Segments.Add({ /*Id*/10, /*FromSeg*/0, /*FromNode*/1, /*ToSeg*/11, /*ToNode*/0, 270.0 });
		Segments.Add({ 11, 10, 0, 12, 0, 60.0 });
		Segments.Add({ 12, 11, 0, 0, 2, 270.0 });
		const TArray<FAIDAConveyorEdge> Edges = CollapseChains(Segments);
		TestEqual(TEXT("one edge"), Edges.Num(), 1);
		if (Edges.Num() == 1)
		{
			TestEqual(TEXT("from node 1"), Edges[0].FromMachine, 1);
			TestEqual(TEXT("to node 2"), Edges[0].ToMachine, 2);
			AIDA_TEST_NEAR("slowest segment wins", Edges[0].PerMinute, 60.0);
		}
	}

	// Splitter fan-out (two heads from node 3) + a dangling tail + a free-floating segment + a cycle.
	{
		TArray<FSegment> Segments;
		Segments.Add({ 20, 0, 3, 0, 4, 120.0 });  // node3 → node4
		Segments.Add({ 21, 0, 3, 0, 0, 120.0 });  // node3 → dangling
		Segments.Add({ 22, 0, 0, 0, 0, 60.0 });   // floats free → dropped
		Segments.Add({ 30, 31, 0, 31, 0, 60.0 }); // 30 ↔ 31 belt loop → dropped, must terminate
		Segments.Add({ 31, 30, 0, 30, 0, 60.0 });
		const TArray<FAIDAConveyorEdge> Edges = CollapseChains(Segments);
		TestEqual(TEXT("two anchored edges"), Edges.Num(), 2);
		const FAIDAConveyorEdge* Dangling = Edges.FindByPredicate(
			[](const FAIDAConveyorEdge& E) { return E.ToMachine == 0; });
		TestTrue(TEXT("dangling edge kept with To=0"), Dangling && Dangling->FromMachine == 3);
	}

	// Item attribution: pipes keep their fluid; belt edges take the source machine's main output;
	// edges out of logistics-only nodes stay unknown.
	{
		TArray<FSegment> Segments;
		Segments.Add({ 40, 0, 1, 0, 2, 300.0, /*bPipe*/true, TEXT("Water") });
		Segments.Add({ 41, 0, 1, 0, 3, 60.0 });
		Segments.Add({ 42, 0, 9, 0, 1, 60.0 });
		TArray<FAIDAConveyorEdge> Edges = CollapseChains(Segments);

		TArray<FAIDAMachine> Machines;
		FAIDAMachine Producer = AIDAMakeTestMachine(1, FVector::ZeroVector, TEXT("Smelter"));
		Producer.Outputs = { { TEXT("Iron Ingot"), 30.0 }, { TEXT("Slag"), 5.0 } };
		Machines.Add(Producer);
		FAIDAMachine Splitter = AIDAMakeTestMachine(9, FVector::ZeroVector, TEXT("Splitter"));
		Splitter.bLogisticsOnly = true;
		Machines.Add(Splitter);
		AttributeItems(Edges, Machines);

		const FAIDAConveyorEdge* PipeEdge = Edges.FindByPredicate([](const FAIDAConveyorEdge& E) { return E.bPipe; });
		TestTrue(TEXT("pipe keeps its fluid"), PipeEdge && PipeEdge->Item == TEXT("Water"));
		const FAIDAConveyorEdge* BeltEdge = Edges.FindByPredicate(
			[](const FAIDAConveyorEdge& E) { return !E.bPipe && E.FromMachine == 1; });
		TestTrue(TEXT("belt takes the source's main output"), BeltEdge && BeltEdge->Item == TEXT("Iron Ingot"));
		const FAIDAConveyorEdge* SplitterEdge = Edges.FindByPredicate(
			[](const FAIDAConveyorEdge& E) { return E.FromMachine == 9; });
		TestTrue(TEXT("logistics-only source stays unknown"), SplitterEdge && SplitterEdge->Item.IsEmpty());
	}

	// Cluster efficiency ignores logistics-only nodes.
	{
		TArray<FAIDAMachine> Machines;
		FAIDAMachine Slow = AIDAMakeTestMachine(1, FVector::ZeroVector, TEXT("Constructor"));
		Slow.Productivity = 0.5f;
		Machines.Add(Slow);
		FAIDAMachine Node = AIDAMakeTestMachine(2, FVector(100, 0, 0), TEXT("Splitter"));
		Node.bLogisticsOnly = true;
		Node.Productivity = 1.0f;
		Machines.Add(Node);
		const FAIDAAggregatorConfig Config;
		const TArray<int32> Labels = FAIDAFactoryAggregator::ClusterMachines(Machines, Config);
		const TArray<FAIDACluster> Clusters = FAIDAFactoryAggregator::BuildClusters(Machines, Labels, Config);
		TestEqual(TEXT("one cluster"), Clusters.Num(), 1);
		if (Clusters.Num() == 1)
		{
			AIDA_TEST_NEAR("efficiency from producers only", Clusters[0].Efficiency, 0.5);
			TestEqual(TEXT("census still counts the splitter"), Clusters[0].BuildingCensus[FString(TEXT("Splitter"))], 1);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAFactoryDiagnosticsTest, "AIDA.Factory.Diagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAFactoryDiagnosticsTest::RunTest(const FString&)
{
	FAIDAFactorySnapshot Snap;

	// 1: healthy constructor (all ports connected). 2: splitter feeding nothing. 3: idle assembler
	// with an open input. 4: pipe junction with one connection. 5: splitter fully wired.
	FAIDAMachine Healthy = AIDAMakeTestMachine(1, FVector::ZeroVector, TEXT("Constructor"));
	Healthy.Outputs = { { TEXT("Iron Plate"), 120.0 } };
	Healthy.InPortsTotal = 1; Healthy.InPortsConnected = 1;
	Healthy.OutPortsTotal = 1; Healthy.OutPortsConnected = 1;
	Snap.Machines.Add(Healthy);

	FAIDAMachine DeadSplitter = AIDAMakeTestMachine(2, FVector(1000, 0, 0), TEXT("Splitter"));
	DeadSplitter.bLogisticsOnly = true;
	DeadSplitter.InPortsTotal = 1; DeadSplitter.InPortsConnected = 1;
	DeadSplitter.OutPortsTotal = 3; DeadSplitter.OutPortsConnected = 0;
	Snap.Machines.Add(DeadSplitter);

	FAIDAMachine IdleAssembler = AIDAMakeTestMachine(3, FVector(2000, 0, 0), TEXT("Assembler"));
	IdleAssembler.bProducing = false;
	IdleAssembler.InPortsTotal = 2; IdleAssembler.InPortsConnected = 1;
	IdleAssembler.OutPortsTotal = 1; IdleAssembler.OutPortsConnected = 1;
	Snap.Machines.Add(IdleAssembler);

	FAIDAMachine LonelyJunction = AIDAMakeTestMachine(4, FVector(3000, 0, 0), TEXT("PipelineJunction"));
	LonelyJunction.bLogisticsOnly = true;
	LonelyJunction.AnyPortsTotal = 4; LonelyJunction.AnyPortsConnected = 1;
	Snap.Machines.Add(LonelyJunction);

	FAIDAMachine WiredSplitter = AIDAMakeTestMachine(5, FVector(4000, 0, 0), TEXT("Splitter"));
	WiredSplitter.bLogisticsOnly = true;
	WiredSplitter.InPortsTotal = 1; WiredSplitter.InPortsConnected = 1;
	WiredSplitter.OutPortsTotal = 3; WiredSplitter.OutPortsConnected = 2; // one spare out is normal
	Snap.Machines.Add(WiredSplitter);

	Snap.Edges.Add({ 1, 0, TEXT("Iron Plate"), 120.0 }); // belt from the constructor leads nowhere

	const FAIDADisconnectedReport R = FAIDAFactoryAggregator::FindDisconnected(Snap);
	TestEqual(TEXT("four findings"), R.Findings.Num(), 4);
	TestEqual(TEXT("three logistics nodes checked"), R.NodesChecked, 3);
	TestEqual(TEXT("two machines checked"), R.MachinesChecked, 2);
	int32 DanglingNodes = 0, DanglingEdges = 0, OpenPorts = 0;
	for (const FAIDADisconnectedFinding& F : R.Findings)
	{
		switch (F.Kind)
		{
		case EAIDADisconnectKind::DanglingNode: ++DanglingNodes; break;
		case EAIDADisconnectKind::DanglingEdge: ++DanglingEdges; break;
		case EAIDADisconnectKind::OpenPorts: ++OpenPorts; break;
		}
	}
	TestEqual(TEXT("dead splitter + lonely junction"), DanglingNodes, 2);
	TestEqual(TEXT("one dangling belt"), DanglingEdges, 1);
	TestEqual(TEXT("idle assembler's open input"), OpenPorts, 1);
	TestTrue(TEXT("definite breaks sort before advisories"),
		R.Findings.Num() == 4 && R.Findings[3].Kind == EAIDADisconnectKind::OpenPorts);
	const TSharedPtr<FJsonObject> J = AIDAParseTestJson(AIDAFactoryTools::BuildDisconnectedJson(R));
	TestTrue(TEXT("disconnected json parses"), J.IsValid() && J->GetIntegerField(TEXT("findings")) == 4);

	// Belt mismatch: 10 → 11 @270, 11 → 12 @60 (the choke), 12 → 13 @270; plus producer 14 making
	// 120/min onto a 60 belt to 15.
	{
		FAIDAFactorySnapshot Mismatch;
		for (int32 Id = 10; Id <= 15; ++Id)
		{
			FAIDAMachine Node = AIDAMakeTestMachine(Id, FVector(Id * 100, 0, 0),
				(Id == 11 || Id == 12) ? TEXT("Splitter") : TEXT("Machine"));
			Node.bLogisticsOnly = (Id == 11 || Id == 12);
			Mismatch.Machines.Add(Node);
		}
		Mismatch.Machines[4].Outputs = { { TEXT("Screw"), 120.0 } }; // id 14
		Mismatch.Edges.Add({ 10, 11, TEXT(""), 270.0 });
		Mismatch.Edges.Add({ 11, 12, TEXT(""), 60.0 });
		Mismatch.Edges.Add({ 12, 13, TEXT(""), 270.0 });
		Mismatch.Edges.Add({ 14, 15, TEXT(""), 60.0 });

		const TArray<FAIDABeltMismatch> Slow = FAIDAFactoryAggregator::FindBeltMismatch(Mismatch);
		TestEqual(TEXT("two mismatches"), Slow.Num(), 2);
		if (Slow.Num() == 2)
		{
			TestEqual(TEXT("sandwiched slow belt first (bigger choke)"), Slow[0].FromNode, 11);
			AIDA_TEST_NEAR("edge rate", Slow[0].EdgePerMin, 60.0);
			AIDA_TEST_NEAR("upstream rate", Slow[0].UpstreamPerMin, 270.0);
			TestEqual(TEXT("undersized producer belt second"), Slow[1].FromNode, 14);
			AIDA_TEST_NEAR("producer rate", Slow[1].ProducerPerMin, 120.0);
		}
		const TSharedPtr<FJsonObject> MJ = AIDAParseTestJson(AIDAFactoryTools::BuildBeltMismatchJson(Slow));
		TestTrue(TEXT("mismatch json parses"), MJ.IsValid() && MJ->GetIntegerField(TEXT("mismatches")) == 2);
	}
	return true;
}

namespace
{
	FAIDARecipeInfo AIDAMakeTestRecipe(const FString& Name, double Duration, const FString& Building,
		std::initializer_list<FAIDAItemAmount> Ins, std::initializer_list<FAIDAItemAmount> Outs)
	{
		FAIDARecipeInfo R;
		R.RecipeName = Name;
		R.DurationSeconds = Duration;
		R.ProducedIn = { Building };
		R.Ingredients = Ins;
		R.Products = Outs;
		return R;
	}

	/** Screw chain: Ore -> Ingot (Smelter) -> Rod (Constructor) -> Screw ×4 (Constructor), plus belts. */
	void AIDAMakeTestCatalog(TArray<FAIDARecipeInfo>& Recipes, TArray<FAIDABuildingInfo>& Buildings)
	{
		Recipes = {
			AIDAMakeTestRecipe(TEXT("Iron Ingot"), 2.0, TEXT("Smelter"),
				{ { TEXT("Iron Ore"), 1.0 } }, { { TEXT("Iron Ingot"), 1.0 } }),
			AIDAMakeTestRecipe(TEXT("Iron Rod"), 4.0, TEXT("Constructor"),
				{ { TEXT("Iron Ingot"), 1.0 } }, { { TEXT("Iron Rod"), 1.0 } }),
			AIDAMakeTestRecipe(TEXT("Screw"), 6.0, TEXT("Constructor"),
				{ { TEXT("Iron Rod"), 1.0 } }, { { TEXT("Screw"), 4.0 } }),
			AIDAMakeTestRecipe(TEXT("Alternate: Cast Screw"), 24.0, TEXT("Constructor"),
				{ { TEXT("Iron Ingot"), 5.0 } }, { { TEXT("Screw"), 20.0 } }),
		};

		FAIDABuildingInfo Smelter; Smelter.Name = TEXT("Smelter");
		Smelter.PowerConsumptionMW = 4.0; Smelter.PowerExponent = 1.321928;
		Smelter.FootprintXM = 6.0; Smelter.FootprintYM = 9.0;
		FAIDABuildingInfo Ctor; Ctor.Name = TEXT("Constructor");
		Ctor.PowerConsumptionMW = 4.0; Ctor.PowerExponent = 1.321928;
		Ctor.FootprintXM = 8.0; Ctor.FootprintYM = 10.0;
		FAIDABuildingInfo Mk1; Mk1.Name = TEXT("Conveyor Belt Mk.1"); Mk1.BeltItemsPerMin = 60.0;
		FAIDABuildingInfo Mk3; Mk3.Name = TEXT("Conveyor Belt Mk.3"); Mk3.BeltItemsPerMin = 270.0;
		FAIDABuildingInfo Pipe; Pipe.Name = TEXT("Pipeline"); Pipe.PipeM3PerMin = 300.0;
		Buildings = { Smelter, Ctor, Mk1, Mk3, Pipe };
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAFactoryPlannerTest, "AIDA.Planner.PlanFactory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAFactoryPlannerTest::RunTest(const FString&)
{
	TArray<FAIDARecipeInfo> Recipes;
	TArray<FAIDABuildingInfo> Buildings;
	AIDAMakeTestCatalog(Recipes, Buildings);

	// 240 screws/min: 6 Screw constructors @100% (40/min each), 60 rods -> 4 @100%, 60 ingots -> 2 @100%.
	{
		const FAIDAFactoryPlan P = FAIDAFactoryPlanner::Plan(TEXT("Screw"), 240.0, Recipes, Buildings);
		TestTrue(TEXT("no error"), P.Error.IsEmpty());
		TestEqual(TEXT("three steps"), P.Steps.Num(), 3);
		TestEqual(TEXT("twelve machines total"), P.TotalMachines, 12);
		AIDA_TEST_NEAR("power at 100% is 12 x 4 MW", P.TotalPowerMW, 48.0);
		if (P.Steps.Num() == 3)
		{
			TestEqual(TEXT("target step first"), P.Steps[0].Item, FString(TEXT("Screw")));
			TestEqual(TEXT("screw machines"), P.Steps[0].Machines, 6);
			AIDA_TEST_NEAR("screw clock 100%", P.Steps[0].Clock, 1.0);
			TestEqual(TEXT("standard recipe chosen"), P.Steps[0].Recipe, FString(TEXT("Screw")));
			TestEqual(TEXT("alternate listed"), P.Steps[0].AlternateRecipes.Num(), 1);
			TestEqual(TEXT("screws ride a Mk.3"), P.Steps[0].Transport, FString(TEXT("Conveyor Belt Mk.3")));
			TestEqual(TEXT("rod step"), P.Steps[1].Item, FString(TEXT("Iron Rod")));
			TestEqual(TEXT("rod machines"), P.Steps[1].Machines, 4);
			TestEqual(TEXT("rods fit a Mk.1"), P.Steps[1].Transport, FString(TEXT("Conveyor Belt Mk.1")));
			TestEqual(TEXT("ingot machines"), P.Steps[2].Machines, 2);
		}
		TestEqual(TEXT("one raw input"), P.RawInputs.Num(), 1);
		if (P.RawInputs.Num() == 1)
		{
			TestEqual(TEXT("raw is ore"), P.RawInputs[0].Item, FString(TEXT("Iron Ore")));
			AIDA_TEST_NEAR("ore rate", P.RawInputs[0].RatePerMin, 60.0);
		}

		const TSharedPtr<FJsonObject> J = AIDAParseTestJson(FAIDAFactoryPlanner::BuildPlanJson(P));
		if (TestNotNull(TEXT("plan json parses"), J.Get()))
		{
			TestEqual(TEXT("json machines"), J->GetIntegerField(TEXT("totalMachines")), 12);
			const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
			TestTrue(TEXT("json steps"), J->TryGetArrayField(TEXT("steps"), Steps) && Steps && Steps->Num() == 3);
		}
	}

	// Non-round rate: 250 screws -> 7 machines, clock 250/280, power scales sub-linearly.
	{
		const FAIDAFactoryPlan P = FAIDAFactoryPlanner::Plan(TEXT("Screw"), 250.0, Recipes, Buildings);
		if (TestTrue(TEXT("plans"), P.Error.IsEmpty() && P.Steps.Num() == 3))
		{
			TestEqual(TEXT("seven screw machines"), P.Steps[0].Machines, 7);
			AIDA_TEST_NEAR("exact clock", P.Steps[0].Clock, 250.0 / 280.0);
			const double Expected = 7.0 * 4.0 * FMath::Pow(250.0 / 280.0, 1.321928);
			TestTrue(TEXT("clock^exponent power"), FMath::IsNearlyEqual(P.Steps[0].PowerMW, Expected, 1e-6));
		}
	}

	// Fluids ride pipes; fluid raws flagged. One-recipe catalog: Water + Ore -> Slurry (fluid out).
	{
		TArray<FAIDARecipeInfo> FluidRecipes = {
			AIDAMakeTestRecipe(TEXT("Slurry"), 3.0, TEXT("Refinery"),
				{ { TEXT("Ore"), 2.0 }, { TEXT("Water"), 2.5, true } }, { { TEXT("Slurry"), 3.0, true } }),
		};
		const FAIDAFactoryPlan P = FAIDAFactoryPlanner::Plan(TEXT("Slurry"), 120.0, FluidRecipes, Buildings);
		if (TestTrue(TEXT("fluid plan works"), P.Error.IsEmpty() && P.Steps.Num() == 1))
		{
			TestEqual(TEXT("slurry rides a pipeline"), P.Steps[0].Transport, FString(TEXT("Pipeline")));
			const FAIDAPlanResource* Water = P.RawInputs.FindByPredicate(
				[](const FAIDAPlanResource& R) { return R.Item == TEXT("Water"); });
			TestTrue(TEXT("water raw is fluid"), Water && Water->bFluid);
			AIDA_TEST_NEAR("water rate", Water ? Water->RatePerMin : -1.0, 100.0);
		}
	}

	// Unknown item -> error; recipe cycle -> truncation note, no hang.
	{
		const FAIDAFactoryPlan P = FAIDAFactoryPlanner::Plan(TEXT("Unobtainium"), 10.0, Recipes, Buildings);
		TestTrue(TEXT("unknown item errors"), !P.Error.IsEmpty());

		TArray<FAIDARecipeInfo> Cyclic = {
			AIDAMakeTestRecipe(TEXT("A"), 1.0, TEXT("Constructor"), { { TEXT("B"), 1.0 } }, { { TEXT("A"), 1.0 } }),
			AIDAMakeTestRecipe(TEXT("B"), 1.0, TEXT("Constructor"), { { TEXT("A"), 1.0 } }, { { TEXT("B"), 1.0 } }),
		};
		const FAIDAFactoryPlan C = FAIDAFactoryPlanner::Plan(TEXT("A"), 10.0, Cyclic, Buildings);
		TestTrue(TEXT("cycle terminates with a note"), C.Error.IsEmpty() && C.Notes.Num() > 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAFactoryClockAdviceTest, "AIDA.Factory.ClockAdvice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAFactoryClockAdviceTest::RunTest(const FString&)
{
	TArray<FAIDAMachine> Machines;

	// Half-idle constructor at 100%: suggest 50%, saving 30 - 30*(0.5^log2(2.5)) = 30 - 30*0.4 = 18 MW.
	FAIDAMachine HalfIdle = AIDAMakeTestMachine(1, FVector::ZeroVector, TEXT("Constructor"));
	HalfIdle.Outputs = { { TEXT("IronPlate"), 20.0 } };
	HalfIdle.Clock = 1.0f; HalfIdle.Productivity = 0.5f; HalfIdle.PowerMW = 30.0;
	Machines.Add(HalfIdle);

	// Slightly-idle smelter (97%): above the 90% threshold -> no advice.
	FAIDAMachine Busy = AIDAMakeTestMachine(2, FVector::ZeroVector, TEXT("Smelter"));
	Busy.Outputs = { { TEXT("IronIngot"), 30.0 } };
	Busy.Clock = 1.0f; Busy.Productivity = 0.97f; Busy.PowerMW = 4.0;
	Machines.Add(Busy);

	// Stopped assembler: zero productivity is a bottleneck, not an underclock candidate.
	FAIDAMachine Stopped = AIDAMakeTestMachine(3, FVector::ZeroVector, TEXT("Assembler"));
	Stopped.Outputs = { { TEXT("Screw"), 40.0 } };
	Stopped.Clock = 1.0f; Stopped.Productivity = 0.0f; Stopped.PowerMW = 15.0;
	Machines.Add(Stopped);

	// Generator (negative PowerMW) and a mostly-idle machine with a small draw: generator skipped,
	// small machine advised but sorted below the big saving.
	FAIDAMachine Gen = AIDAMakeTestMachine(4, FVector::ZeroVector, TEXT("CoalGenerator"));
	Gen.Outputs = { { TEXT("Power"), 0.0 } };
	Gen.Clock = 1.0f; Gen.Productivity = 0.5f; Gen.PowerMW = -75.0;
	Machines.Add(Gen);

	FAIDAMachine Small = AIDAMakeTestMachine(5, FVector::ZeroVector, TEXT("Constructor"));
	Small.Outputs = { { TEXT("Wire"), 30.0 } };
	Small.Clock = 0.5f; Small.Productivity = 0.503f; Small.PowerMW = 2.0;
	Machines.Add(Small);

	const FAIDAClockAdviceReport R = FAIDAFactoryAggregator::BuildClockAdvice(Machines);

	TestEqual(TEXT("two machines advised"), R.Advice.Num(), 2);
	TestEqual(TEXT("one stopped machine counted"), R.StoppedMachines, 1);
	if (R.Advice.Num() == 2)
	{
		TestEqual(TEXT("biggest saving first"), R.Advice[0].MachineId, 1);
		AIDA_TEST_NEAR("suggested clock is productivity", R.Advice[0].SuggestedClock, 0.5);
		TestTrue(TEXT("saved ~18 MW"), FMath::IsNearlyEqual(R.Advice[0].SavedMW, 18.0, 0.05));

		// 0.5 * 0.503 = 0.2515 -> rounded UP to 26%.
		TestEqual(TEXT("small machine second"), R.Advice[1].MachineId, 5);
		AIDA_TEST_NEAR("suggestion rounds up to a whole percent", R.Advice[1].SuggestedClock, 0.26);
	}
	TestTrue(TEXT("total is the sum of savings"),
		FMath::IsNearlyEqual(R.TotalSavableMW, R.Advice.Num() == 2 ? R.Advice[0].SavedMW + R.Advice[1].SavedMW : -1.0, 1e-9));

	const TSharedPtr<FJsonObject> J = AIDAParseTestJson(AIDAFactoryTools::BuildClockAdviceJson(R));
	if (TestNotNull(TEXT("clock advice json parses"), J.Get()))
	{
		TestEqual(TEXT("advised count"), J->GetIntegerField(TEXT("advised")), 2);
		TestEqual(TEXT("stopped machines surfaced"), J->GetIntegerField(TEXT("stoppedMachines")), 1);
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (TestTrue(TEXT("advice array present"), J->TryGetArrayField(TEXT("advice"), Arr) && Arr && Arr->Num() == 2))
		{
			const TSharedPtr<FJsonObject> First = (*Arr)[0]->AsObject();
			TestEqual(TEXT("clock_pct"), First->GetIntegerField(TEXT("clock_pct")), 100);
			TestEqual(TEXT("suggested_pct"), First->GetIntegerField(TEXT("suggested_pct")), 50);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAFactoryContainerContentsTest, "AIDA.Factory.ContainerContents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAFactoryContainerContentsTest::RunTest(const FString&)
{
	TArray<FAIDAContainerInfo> Containers;

	FAIDAContainerInfo Near;
	Near.Id = 1; Near.BuildingClass = TEXT("StorageContainerMk1");
	Near.Location = FVector(1000, 0, 0); // 10 m out
	Near.SlotsUsed = 2; Near.SlotsTotal = 24;
	Near.Contents = { { TEXT("Iron Plate"), 200 }, { TEXT("Screw"), 50 } };
	Containers.Add(Near);

	FAIDAContainerInfo Far;
	Far.Id = 2; Far.BuildingClass = TEXT("StorageContainerMk2");
	Far.Location = FVector(50000, 0, 0); // 500 m out
	Far.SlotsUsed = 1; Far.SlotsTotal = 48;
	Far.Contents = { { TEXT("Copper Sheet"), 75 } };
	Containers.Add(Far);

	const FVector Player = FVector::ZeroVector;

	// No filters: both, nearest first.
	{
		const TSharedPtr<FJsonObject> J = AIDAParseTestJson(
			AIDAFactoryTools::BuildContainerContentsJson(Containers, FString(), Player, true, 0.0));
		if (TestNotNull(TEXT("json parses"), J.Get()))
		{
			TestEqual(TEXT("both containers"), J->GetIntegerField(TEXT("containers")), 2);
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (TestTrue(TEXT("list present"), J->TryGetArrayField(TEXT("containerList"), Arr) && Arr && Arr->Num() == 2))
			{
				const TSharedPtr<FJsonObject> First = (*Arr)[0]->AsObject();
				TestEqual(TEXT("nearest first"), First->GetIntegerField(TEXT("id")), 1);
				TestEqual(TEXT("distance humanized to metres"), First->GetIntegerField(TEXT("distance_m")), 10);
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				if (TestTrue(TEXT("contents present"), First->TryGetArrayField(TEXT("contents"), Items) && Items && Items->Num() == 2))
				{
					TestEqual(TEXT("dominant item first"), (*Items)[0]->AsObject()->GetStringField(TEXT("item")), FString(TEXT("Iron Plate")));
				}
			}
		}
	}

	// Item filter matches case-insensitively on a substring.
	{
		const TSharedPtr<FJsonObject> J = AIDAParseTestJson(
			AIDAFactoryTools::BuildContainerContentsJson(Containers, TEXT("copper"), Player, true, 0.0));
		TestTrue(TEXT("item filter narrows to the mk2 box"),
			J.IsValid() && J->GetIntegerField(TEXT("containers")) == 1);
	}

	// Radius filter drops the far container.
	{
		const TSharedPtr<FJsonObject> J = AIDAParseTestJson(
			AIDAFactoryTools::BuildContainerContentsJson(Containers, FString(), Player, true, 100.0));
		TestTrue(TEXT("radius filter keeps only the near box"),
			J.IsValid() && J->GetIntegerField(TEXT("containers")) == 1);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAActionsConfigTest, "AIDA.Actions.ConfigParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAActionsConfigTest::RunTest(const FString&)
{
	// String-mode approvalPolicy + overrides.
	{
		const FString Jsonc = TEXT(R"({
  "provider": { "type": "anthropic", "baseUrl": "x", "model": "y" },
  "actions": { "enabled": false, "ttlSeconds": 120, "approvalPolicy": "requester",
               "maxProposalItems": 50, "maxPendingProposals": 2, "batchPerTick": 5,
               "undoWindow": 10, "costMode": "free" }
})");
		FAIDAConfig Config;
		FString Error;
		TestTrue(FString::Printf(TEXT("parses (%s)"), *Error), FAIDAConfigLoader::LoadFromString(Jsonc, Config, Error));
		TestFalse(TEXT("enabled"), Config.Actions.bEnabled);
		TestEqual(TEXT("ttl"), Config.Actions.TtlSeconds, 120);
		TestEqual(TEXT("approvalPolicy"), Config.Actions.ApprovalPolicy, TEXT("requester"));
		TestEqual(TEXT("maxProposalItems"), Config.Actions.MaxProposalItems, 50);
		TestEqual(TEXT("maxPendingProposals"), Config.Actions.MaxPendingProposals, 2);
		TestEqual(TEXT("batchPerTick"), Config.Actions.BatchPerTick, 5);
		TestEqual(TEXT("undoWindow"), Config.Actions.UndoWindow, 10);
		TestEqual(TEXT("costMode"), Config.Actions.CostMode, TEXT("free"));
	}

	// Array-mode approvalPolicy becomes "list" + ids.
	{
		const FString Jsonc = TEXT(R"({
  "provider": { "type": "anthropic", "baseUrl": "x", "model": "y" },
  "actions": { "approvalPolicy": ["id-1", "id-2"] }
})");
		FAIDAConfig Config;
		FString Error;
		TestTrue(FString::Printf(TEXT("parses (%s)"), *Error), FAIDAConfigLoader::LoadFromString(Jsonc, Config, Error));
		TestEqual(TEXT("list mode"), Config.Actions.ApprovalPolicy, TEXT("list"));
		TestEqual(TEXT("two ids"), Config.Actions.ApprovalIds.Num(), 2);
	}

	// Absent block keeps documented defaults; bad values are rejected.
	{
		const FString Jsonc = TEXT(R"({ "provider": { "type": "anthropic", "baseUrl": "x", "model": "y" } })");
		FAIDAConfig Config;
		FString Error;
		TestTrue(TEXT("defaults parse"), FAIDAConfigLoader::LoadFromString(Jsonc, Config, Error));
		TestTrue(TEXT("default enabled"), Config.Actions.bEnabled);
		TestEqual(TEXT("default ttl"), Config.Actions.TtlSeconds, 600);
		TestEqual(TEXT("default policy"), Config.Actions.ApprovalPolicy, TEXT("any-act"));
		TestEqual(TEXT("default costMode"), Config.Actions.CostMode, TEXT("central"));

		const FString Bad = TEXT(R"({ "provider": { "type": "anthropic", "baseUrl": "x", "model": "y" },
  "actions": { "costMode": "creative" } })");
		TestFalse(TEXT("rejects unknown costMode"), FAIDAConfigLoader::LoadFromString(Bad, Config, Error));
	}
	return true;
}

namespace
{
	TSharedPtr<FJsonObject> AIDATestParseJson(const FString& Json)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAActionsSpecParseTest, "AIDA.Actions.SpecParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAActionsSpecParseTest::RunTest(const FString&)
{
	FString Error;

	// A valid 10x10 build spec; yaw snaps to the nearest 90°.
	{
		FAIDABuildSpec Spec;
		const TSharedPtr<FJsonObject> Json = AIDATestParseJson(TEXT(
			R"({ "version": 1, "buildable": "Foundation 8m x 2m", "origin": { "x": -120, "y": 45, "z": 30 },
			     "yawDeg": 93, "grid": { "countX": 10, "countY": 10, "stepX": 8, "stepY": 8 } })"));
		TestTrue(FString::Printf(TEXT("parses (%s)"), *Error), AIDAActionSpec::ParseBuildSpec(Json, 200, Spec, Error));
		TestEqual(TEXT("buildable"), Spec.Buildable, TEXT("Foundation 8m x 2m"));
		TestEqual(TEXT("origin x (m)"), Spec.OriginM.X, -120.0);
		TestEqual(TEXT("yaw snapped"), Spec.YawDeg, 90);
		TestEqual(TEXT("countX"), Spec.Grid.CountX, 10);
	}

	// Negative yaw normalizes into [0, 270].
	{
		FAIDABuildSpec Spec;
		const TSharedPtr<FJsonObject> Json = AIDATestParseJson(TEXT(
			R"({ "version": 1, "buildable": "Smelter", "origin": { "x": 0, "y": 0 }, "yawDeg": -90 })"));
		TestTrue(TEXT("parses default grid"), AIDAActionSpec::ParseBuildSpec(Json, 200, Spec, Error));
		TestEqual(TEXT("yaw -90 -> 270"), Spec.YawDeg, 270);
		TestEqual(TEXT("default count 1x1"), Spec.Grid.CountX * Spec.Grid.CountY, 1);
	}

	// Rejections: wrong version, missing buildable, malformed origin, over-cap grid.
	{
		FAIDABuildSpec Spec;
		TestFalse(TEXT("rejects version 3"), AIDAActionSpec::ParseBuildSpec(
			AIDATestParseJson(TEXT(R"({ "version": 3, "buildable": "x", "origin": { "x": 0, "y": 0 } })")), 200, Spec, Error));
		TestFalse(TEXT("version 2 without parts rejected"), AIDAActionSpec::ParseBuildSpec(
			AIDATestParseJson(TEXT(R"({ "version": 2, "buildable": "x", "origin": { "x": 0, "y": 0 } })")), 200, Spec, Error));
		TestFalse(TEXT("rejects missing buildable"), AIDAActionSpec::ParseBuildSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "origin": { "x": 0, "y": 0 } })")), 200, Spec, Error));
		TestFalse(TEXT("rejects malformed origin"), AIDAActionSpec::ParseBuildSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "buildable": "x", "origin": { "x": "here" } })")), 200, Spec, Error));
		TestFalse(TEXT("rejects over-cap"), AIDAActionSpec::ParseBuildSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "buildable": "x", "origin": { "x": 0, "y": 0 },
			                            "grid": { "countX": 20, "countY": 20 } })")), 200, Spec, Error));
		TestTrue(TEXT("cap error names the cap"), Error.Contains(TEXT("200")));
		TestTrue(TEXT("cap 0 = unlimited"), AIDAActionSpec::ParseBuildSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "buildable": "x", "origin": { "x": 0, "y": 0 },
			                            "grid": { "countX": 50, "countY": 50 } })")), 0, Spec, Error));
	}

	// Omitted origin/center = "at the player" (the tool fills it from the requester's location).
	{
		FAIDABuildSpec Spec;
		TestTrue(TEXT("origin optional"), AIDAActionSpec::ParseBuildSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "buildable": "x" })")), 200, Spec, Error));
		TestFalse(TEXT("omitted origin flagged"), Spec.bHasOrigin);
		TestFalse(TEXT("flat by default"), Spec.bFollowTerrain);

		TestTrue(TEXT("followTerrain parses"), AIDAActionSpec::ParseBuildSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "buildable": "x", "followTerrain": true })")), 200, Spec, Error));
		TestTrue(TEXT("followTerrain set"), Spec.bFollowTerrain);

		FAIDADismantleSpec Sel;
		TestTrue(TEXT("center optional"), AIDAActionSpec::ParseDismantleSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "radiusM": 10 })")), 200, Sel, Error));
		TestFalse(TEXT("omitted center flagged"), Sel.bHasCenter);
	}

	// Dismantle selector: valid, maxCount clamped to the cap, radius required.
	{
		FAIDADismantleSpec Sel;
		TestTrue(TEXT("selector parses"), AIDAActionSpec::ParseDismantleSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "buildable": "Smelter", "center": { "x": 1, "y": 2 },
			                            "radiusM": 50, "maxCount": 500 })")), 200, Sel, Error));
		TestEqual(TEXT("maxCount clamped"), Sel.MaxCount, 200);
		TestFalse(TEXT("rejects missing radius"), AIDAActionSpec::ParseDismantleSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "center": { "x": 1, "y": 2 } })")), 200, Sel, Error));
	}

	// Power spec (propose_power): everything optional but the version; planner chunks rows.
	{
		FAIDAPowerSpec Power;
		TestTrue(TEXT("minimal power spec parses"), AIDAActionSpec::ParsePowerSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1 })")), 200, Power, Error));
		TestFalse(TEXT("omitted center flagged"), Power.bHasCenter);
		TestEqual(TEXT("default radius 30"), Power.RadiusM, 30.0);
		TestEqual(TEXT("maxCount 0 clamps to cap"), Power.MaxCount, 200);

		TestTrue(TEXT("full power spec parses"), AIDAActionSpec::ParsePowerSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "buildable": "Refinery", "pole": "Power Pole Mk.2",
			                            "center": { "x": 1, "y": 2 }, "radiusM": 15, "maxCount": 4 })")), 200, Power, Error));
		TestEqual(TEXT("buildable filter"), Power.Buildable, TEXT("Refinery"));
		TestEqual(TEXT("pole override"), Power.Pole, TEXT("Power Pole Mk.2"));
		TestEqual(TEXT("maxCount kept"), Power.MaxCount, 4);

		// 5 machines in a row along X, 2 per pole -> 3 poles offset in Y, chained, every machine wired.
		TArray<FVector> Row;
		for (int32 i = 0; i < 5; ++i) { Row.Add(FVector(i * 1000.0, 0.0, 0.0)); }
		const FAIDAPowerPlan Plan = AIDAActionSpec::PlanPowerForPoints(Row, 2, 300.0);
		TestTrue(TEXT("plan ok"), Plan.Error.IsEmpty());
		TestEqual(TEXT("three poles"), Plan.Poles.Num(), 3);
		TestEqual(TEXT("five machine wires"), Plan.MachineWires.Num(), 5);
		TestEqual(TEXT("two chain wires"), Plan.ChainWires.Num(), 2);
		if (Plan.Poles.Num() == 3)
		{
			AIDA_TEST_NEAR("first pole at first chunk centroid X", Plan.Poles[0].GetLocation().X, 500.0);
			AIDA_TEST_NEAR("pole offset perpendicular", Plan.Poles[0].GetLocation().Y, 300.0);
			AIDA_TEST_NEAR("last pole alone at the tail", Plan.Poles[2].GetLocation().X, 4000.0);
		}
		const FAIDAPowerPlan Empty = AIDAActionSpec::PlanPowerForPoints({}, 2, 300.0);
		TestTrue(TEXT("empty input errors"), !Empty.Error.IsEmpty());
	}

	// Label spec (P7 Slice 3): everything optional but the version; defaults hold; bad radius rejects.
	{
		FAIDALabelSpec Label;
		TestTrue(TEXT("minimal label spec parses"), AIDAActionSpec::ParseLabelSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1 })")), 200, Label, Error));
		TestFalse(TEXT("omitted center flagged"), Label.bHasCenter);
		TestEqual(TEXT("default radius 30"), Label.RadiusM, 30.0);
		TestEqual(TEXT("default maxCount 20"), Label.MaxCount, 20);

		TestTrue(TEXT("full label spec parses"), AIDAActionSpec::ParseLabelSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "sign": "Label Sign", "center": { "x": 5, "y": -3 },
			                            "radiusM": 12, "maxCount": 500, "item": "Iron Plate" })")), 200, Label, Error));
		TestTrue(TEXT("center flagged"), Label.bHasCenter);
		TestEqual(TEXT("sign override"), Label.Sign, TEXT("Label Sign"));
		TestEqual(TEXT("item filter"), Label.ItemFilter, TEXT("Iron Plate"));
		TestEqual(TEXT("maxCount clamped to cap"), Label.MaxCount, 200);

		TestFalse(TEXT("rejects bad radius"), AIDAActionSpec::ParseLabelSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "radiusM": -4 })")), 200, Label, Error));

		const FString Summary = AIDAActionSpec::SummarizeLabel(Label, TEXT("Label Sign"), 6);
		TestTrue(TEXT("summary names the count and sign"),
			Summary.Contains(TEXT("6 container(s)")) && Summary.Contains(TEXT("Label Sign")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAActionsSpecParseV2Test, "AIDA.Actions.SpecParseV2",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAActionsSpecParseV2Test::RunTest(const FString&)
{
	FString Error;

	// A valid composite: two parts with offsets, per-part yaw + grid; auto-power forced off.
	{
		FAIDABuildSpec Spec;
		const TSharedPtr<FJsonObject> Json = AIDATestParseJson(TEXT(
			R"json({ "version": 2, "origin": { "x": 10, "y": 20, "z": 5 }, "yawDeg": 90, "parts": [
			     { "buildable": "Foundation (2 m)", "grid": { "countX": 3, "countY": 2 } },
			     { "buildable": "Big Concrete Pillar", "at": { "x": 4, "y": 8, "z": 2 }, "yawDeg": -90 } ] })json"));
		TestTrue(FString::Printf(TEXT("parses (%s)"), *Error), AIDAActionSpec::ParseBuildSpec(Json, 200, Spec, Error));
		TestEqual(TEXT("two parts"), Spec.Parts.Num(), 2);
		TestEqual(TEXT("composite yaw"), Spec.YawDeg, 90);
		TestEqual(TEXT("origin z"), Spec.OriginM.Z, 5.0);
		if (Spec.Parts.Num() == 2)
		{
			TestEqual(TEXT("part 0 grid"), Spec.Parts[0].Grid.CountX * Spec.Parts[0].Grid.CountY, 6);
			TestEqual(TEXT("part 0 offset default"), Spec.Parts[0].OffsetM, FVector::ZeroVector);
			TestEqual(TEXT("part 1 offset"), Spec.Parts[1].OffsetM, FVector(4, 8, 2));
			TestEqual(TEXT("part 1 yaw normalized"), Spec.Parts[1].YawDeg, 270);
		}
		TestFalse(TEXT("v2 disables auto-power"), Spec.bPower);
	}

	// Rejections: missing part buildable, cap across ALL parts, belts/wires not yet supported.
	{
		FAIDABuildSpec Spec;
		TestFalse(TEXT("part without buildable rejected"), AIDAActionSpec::ParseBuildSpec(
			AIDATestParseJson(TEXT(R"({ "version": 2, "parts": [ { "at": { "x": 0, "y": 0 } } ] })")), 200, Spec, Error));
		TestTrue(TEXT("error names the part"), Error.Contains(TEXT("parts[0]")));

		TestFalse(TEXT("cap spans parts"), AIDAActionSpec::ParseBuildSpec(
			AIDATestParseJson(TEXT(R"({ "version": 2, "parts": [
				{ "buildable": "a", "grid": { "countX": 10, "countY": 10 } },
				{ "buildable": "b", "grid": { "countX": 10, "countY": 11 } } ] })")), 200, Spec, Error));

		TestFalse(TEXT("belts rejected for now"), AIDAActionSpec::ParseBuildSpec(
			AIDATestParseJson(TEXT(R"({ "version": 2, "parts": [ { "buildable": "a" } ], "belts": [] })")), 200, Spec, Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAActionsExpandPartsTest, "AIDA.Actions.ExpandParts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAActionsExpandPartsTest::RunTest(const FString&)
{
	// Composite at (10, 20, 5) m rotated 90°: part offsets must rotate with the composite yaw, and
	// per-part yaw adds on top. Part 0 = a 2x1 grid of 8 m tiles at the origin; part 1 = one piece
	// offset (+4, 0, +2) m — which the 90° composite yaw carries to world (+Y).
	FAIDABuildSpec Spec;
	Spec.OriginM = FVector(10, 20, 5);
	Spec.YawDeg = 90;

	FAIDABuildPart Floor;
	Floor.Buildable = TEXT("floor");
	Floor.Grid.CountX = 2;
	Spec.Parts.Add(Floor);

	FAIDABuildPart Pillar;
	Pillar.Buildable = TEXT("pillar");
	Pillar.OffsetM = FVector(4, 0, 2);
	Pillar.YawDeg = 90;
	Spec.Parts.Add(Pillar);

	TArray<FVector2D> Footprints = { FVector2D(8, 8), FVector2D(2, 2) };
	TArray<int32> PartIndex;
	const TArray<FTransform> Out = AIDAActionSpec::ExpandParts(Spec, Footprints, PartIndex);

	if (!TestEqual(TEXT("3 placements"), Out.Num(), 3) || !TestEqual(TEXT("part map parallel"), PartIndex.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("grouped by part"), PartIndex[0] == 0 && PartIndex[1] == 0 && PartIndex[2] == 1);

	// Part 0, tile 0: at the composite origin (cm).
	TestTrue(TEXT("floor tile 0 at origin"), Out[0].GetLocation().Equals(FVector(1000, 2000, 500), 0.1));
	// Part 0, tile 1: +X footprint step rotated 90° -> world +Y, 8 m = 800 cm.
	TestTrue(TEXT("floor tile 1 rotated step"), Out[1].GetLocation().Equals(FVector(1000, 2800, 500), 0.1));
	TestTrue(TEXT("floor yaw = composite"), FMath::IsNearlyEqual(Out[0].Rotator().Yaw, 90.0, 0.1));
	// Part 1: offset (+4, 0, +2) m rotated 90° -> world (0, +4, +2) m from the origin.
	TestTrue(TEXT("pillar offset rotated"), Out[2].GetLocation().Equals(FVector(1000, 2400, 700), 0.1));
	// Part yaw stacks on the composite yaw: 90 + 90 = 180.
	TestTrue(TEXT("pillar yaw stacks"), FMath::IsNearlyEqual(FMath::UnwindDegrees(Out[2].Rotator().Yaw), 180.0, 0.1)
		|| FMath::IsNearlyEqual(Out[2].Rotator().Yaw, 180.0, 0.1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAActionsGridExpandTest, "AIDA.Actions.GridExpand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAActionsGridExpandTest::RunTest(const FString&)
{
	FAIDABuildSpec Spec;
	Spec.Buildable = TEXT("Foundation");
	Spec.OriginM = FVector(10.0, 20.0, 0.0);
	Spec.Grid.CountX = 3;
	Spec.Grid.CountY = 2;
	// Steps left 0 — the caller-supplied footprint (8 m) applies.

	// Unrotated: row-major, metre steps become cm.
	{
		const TArray<FTransform> T = AIDAActionSpec::ExpandGrid(Spec, 8.0, 8.0);
		if (!TestEqual(TEXT("count"), T.Num(), 6)) { return false; }
		TestEqual(TEXT("first at origin (cm)"), T[0].GetLocation(), FVector(1000.0, 2000.0, 0.0));
		TestEqual(TEXT("second steps +X 8 m"), T[1].GetLocation(), FVector(1800.0, 2000.0, 0.0));
		TestEqual(TEXT("second row steps +Y 8 m"), T[3].GetLocation(), FVector(1000.0, 2800.0, 0.0));
	}

	// Yaw 90: the step axes rotate with the grid (X becomes +Y).
	{
		Spec.YawDeg = 90;
		const TArray<FTransform> T = AIDAActionSpec::ExpandGrid(Spec, 8.0, 8.0);
		if (!TestEqual(TEXT("rotated count"), T.Num(), 6)) { return false; }
		TestTrue(TEXT("rotated step goes +Y"), T[1].GetLocation().Equals(FVector(1000.0, 2800.0, 0.0), 0.01));
		TestTrue(TEXT("rotated row goes -X"), T[3].GetLocation().Equals(FVector(200.0, 2000.0, 0.0), 0.01));
		TestTrue(TEXT("rotation applied"), FMath::IsNearlyEqual(T[0].Rotator().Yaw, 90.0, 0.01));
	}

	// Explicit steps: gaps (larger than the footprint) are honored; sub-footprint steps clamp UP —
	// they would stack tiles on top of each other (live-verify: "Foundation (1 m)" is 8 m wide).
	{
		Spec.YawDeg = 0;
		Spec.Grid.StepXM = 12.0;
		const TArray<FTransform> T = AIDAActionSpec::ExpandGrid(Spec, 8.0, 8.0);
		TestEqual(TEXT("gap stepX honored"), T[1].GetLocation().X, 2200.0);

		Spec.Grid.StepXM = 1.0;
		const TArray<FTransform> Clamped = AIDAActionSpec::ExpandGrid(Spec, 8.0, 8.0);
		TestEqual(TEXT("sub-footprint stepX clamps to footprint"), Clamped[1].GetLocation().X, 1800.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAActionsManifoldTest, "AIDA.Actions.Manifold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAActionsManifoldTest::RunTest(const FString&)
{
	FString Error;

	// Spec parse: defaults (belt/in, radius 30, maxCount 0=all→cap, standoff 4).
	{
		FAIDAManifoldSpec Spec;
		const TSharedPtr<FJsonObject> Json = AIDATestParseJson(TEXT(
			R"({ "version": 1, "transport": "Conveyor Belt Mk.2", "machines": { "buildable": "Smelter" } })"));
		TestTrue(FString::Printf(TEXT("parses (%s)"), *Error), AIDAActionSpec::ParseManifoldSpec(Json, 200, Spec, Error));
		TestFalse(TEXT("defaults to belt"), Spec.bPipe);
		TestFalse(TEXT("defaults to in"), Spec.bOutput);
		TestEqual(TEXT("default radius"), Spec.Machines.RadiusM, 30.0);
		TestEqual(TEXT("maxCount 0 clamps to cap"), Spec.Machines.MaxCount, 200);
		TestEqual(TEXT("default standoff"), Spec.StandoffM, 4.0);
		TestFalse(TEXT("omitted center flagged"), Spec.Machines.bHasCenter);
	}

	// Spec parse: explicit pipe/out + rejections.
	{
		FAIDAManifoldSpec Spec;
		TestTrue(TEXT("pipe/out parses"), AIDAActionSpec::ParseManifoldSpec(AIDATestParseJson(TEXT(
			R"({ "version": 1, "kind": "pipe", "direction": "out", "transport": "Pipeline",
			     "machines": { "buildable": "Refinery", "center": { "x": 1, "y": 2 }, "radiusM": 50 }, "port": 1 })")),
			200, Spec, Error));
		TestTrue(TEXT("pipe set"), Spec.bPipe);
		TestTrue(TEXT("out set"), Spec.bOutput);
		TestTrue(TEXT("center kept"), Spec.Machines.bHasCenter);
		TestEqual(TEXT("port index"), Spec.PortIndex, 1);

		TestFalse(TEXT("rejects missing transport"), AIDAActionSpec::ParseManifoldSpec(AIDATestParseJson(TEXT(
			R"({ "version": 1, "machines": { "buildable": "Smelter" } })")), 200, Spec, Error));
		TestFalse(TEXT("rejects missing machines"), AIDAActionSpec::ParseManifoldSpec(AIDATestParseJson(TEXT(
			R"({ "version": 1, "transport": "Belt" })")), 200, Spec, Error));
		TestFalse(TEXT("rejects bad kind"), AIDAActionSpec::ParseManifoldSpec(AIDATestParseJson(TEXT(
			R"({ "version": 1, "kind": "train", "transport": "Belt", "machines": { "buildable": "Smelter" } })")), 200, Spec, Error));
		TestFalse(TEXT("rejects bad direction"), AIDAActionSpec::ParseManifoldSpec(AIDATestParseJson(TEXT(
			R"({ "version": 1, "direction": "sideways", "transport": "Belt", "machines": { "buildable": "Smelter" } })")), 200, Spec, Error));
	}

	// Planner: three machines facing +X, ports fed SHUFFLED — sorted along the row axis (+Y), one
	// straight trunk line at standoff, splitter yaw along the axis, drop direction back at the machines.
	{
		TArray<FAIDAManifoldPortPoint> Ports;
		Ports.Add({ FVector(0.0, 1600.0, 100.0), FVector(1.0, 0.0, 0.0) });
		Ports.Add({ FVector(0.0, 0.0, 100.0), FVector(1.0, 0.0, 0.0) });
		Ports.Add({ FVector(0.0, 800.0, 100.0), FVector(1.0, 0.0, 0.0) });

		const FAIDAManifoldPlan Plan = AIDAActionSpec::PlanManifold(Ports, /*bOutput*/ false, /*bPipe*/ false,
			/*StandoffM*/ 4.0, /*FootprintM*/ 4.0, /*MaxRunM*/ 56.0);
		TestTrue(FString::Printf(TEXT("plans (%s)"), *Plan.Error), Plan.Error.IsEmpty());
		if (!TestEqual(TEXT("attachment count"), Plan.Attachments.Num(), 3)) { return false; }
		TestTrue(TEXT("row axis +Y"), Plan.RowAxis.Equals(FVector(0.0, 1.0, 0.0), 0.01));
		TestTrue(TEXT("drop dir -X (back at the machines)"), Plan.DropDir.Equals(FVector(-1.0, 0.0, 0.0), 0.01));
		TestEqual(TEXT("sorted: first is port 1"), Plan.PortOrder[0], 1);
		TestEqual(TEXT("sorted: last is port 0"), Plan.PortOrder[2], 0);
		TestTrue(TEXT("first attachment on the trunk line"), Plan.Attachments[0].GetLocation().Equals(FVector(400.0, 0.0, 100.0), 0.1));
		TestTrue(TEXT("straight trunk (same X)"), Plan.Attachments[2].GetLocation().Equals(FVector(400.0, 1600.0, 100.0), 0.1));
		TestEqual(TEXT("splitter yaw along axis"), Plan.YawDeg, 90);
		TestEqual(TEXT("open end faces north (-Y)"), AIDAActionSpec::CompassName(-Plan.RowAxis), TEXT("north"));

		// Mergers flip 180° so the collection end stays at index 0.
		const FAIDAManifoldPlan Mergers = AIDAActionSpec::PlanManifold(Ports, /*bOutput*/ true, /*bPipe*/ false, 4.0, 4.0, 56.0);
		TestEqual(TEXT("merger yaw flipped"), Mergers.YawDeg, 270);
	}

	// Planner rejections: mixed facing, too close, hop too long.
	{
		TArray<FAIDAManifoldPortPoint> Opposed;
		Opposed.Add({ FVector(0.0, 0.0, 0.0), FVector(1.0, 0.0, 0.0) });
		Opposed.Add({ FVector(0.0, 800.0, 0.0), FVector(-1.0, 0.0, 0.0) });
		TestTrue(TEXT("rejects opposing normals"),
			!AIDAActionSpec::PlanManifold(Opposed, false, false, 4.0, 4.0, 56.0).Error.IsEmpty());

		TArray<FAIDAManifoldPortPoint> Cramped;
		Cramped.Add({ FVector(0.0, 0.0, 0.0), FVector(1.0, 0.0, 0.0) });
		Cramped.Add({ FVector(0.0, 200.0, 0.0), FVector(1.0, 0.0, 0.0) });
		TestTrue(TEXT("rejects sub-footprint spacing"),
			!AIDAActionSpec::PlanManifold(Cramped, false, false, 4.0, 4.0, 56.0).Error.IsEmpty());

		TArray<FAIDAManifoldPortPoint> Sparse;
		Sparse.Add({ FVector(0.0, 0.0, 0.0), FVector(1.0, 0.0, 0.0) });
		Sparse.Add({ FVector(0.0, 6000.0, 0.0), FVector(1.0, 0.0, 0.0) });
		TestTrue(TEXT("rejects over-long trunk hop"),
			!AIDAActionSpec::PlanManifold(Sparse, false, false, 4.0, 4.0, 56.0).Error.IsEmpty());

		TestTrue(TEXT("single machine plans (drop only)"),
			AIDAActionSpec::PlanManifold({ { FVector::ZeroVector, FVector(1.0, 0.0, 0.0) } }, false, false, 4.0, 4.0, 56.0).Error.IsEmpty());
	}

	// Summary line carries the counts and the open end.
	{
		FAIDAManifoldSpec Spec;
		Spec.Machines.Buildable = TEXT("Smelter");
		const FString Summary = AIDAActionSpec::SummarizeManifold(Spec, TEXT("Conveyor Splitter"), TEXT("Conveyor Belt Mk.2"), 10, 19, TEXT("west"));
		TestTrue(TEXT("summary names attachments"), Summary.Contains(TEXT("10 x Conveyor Splitter")));
		TestTrue(TEXT("summary names runs"), Summary.Contains(TEXT("19 x Conveyor Belt Mk.2")));
		TestTrue(TEXT("summary names the open end"), Summary.Contains(TEXT("west end")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAActionsPowerPlanTest, "AIDA.Actions.PowerPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAActionsPowerPlanTest::RunTest(const FString&)
{
	// 4x2 grid, 8 m steps, 2 machines per pole (mk.1) → 2 poles per row, every machine wired,
	// consecutive chain. Row 0 poles sit half a step BELOW+ (between the rows); the last row folds
	// back onto the same gap, staggered half a step along X so nothing overlaps exactly.
	{
		const FAIDAPowerPlan Plan = AIDAActionSpec::PlanPower(4, 2, 800.0, 800.0, 0, FVector::ZeroVector, 2);
		TestTrue(FString::Printf(TEXT("plans (%s)"), *Plan.Error), Plan.Error.IsEmpty());
		if (!TestEqual(TEXT("pole count"), Plan.Poles.Num(), 4)) { return false; }
		TestEqual(TEXT("every machine wired"), Plan.MachineWires.Num(), 8);
		TestEqual(TEXT("chain links"), Plan.ChainWires.Num(), 3);
		TestTrue(TEXT("row-0 pole between machines 0+1, off the row"),
			Plan.Poles[0].GetLocation().Equals(FVector(400.0, 400.0, 0.0), 0.1));
		TestTrue(TEXT("last row folds back staggered"),
			Plan.Poles[2].GetLocation().Equals(FVector(800.0, 400.0, 0.0), 0.1));
		TestEqual(TEXT("machine 0 -> pole 0"), Plan.MachineWires[0], FIntPoint(0, 0));
		TestEqual(TEXT("machine 3 -> pole 1"), Plan.MachineWires[3], FIntPoint(3, 1));
		TestEqual(TEXT("chain 0-1"), Plan.ChainWires[0], FIntPoint(0, 1));
	}

	// Single row, big group: one pole at the row's middle, offset to the side.
	{
		const FAIDAPowerPlan Plan = AIDAActionSpec::PlanPower(3, 1, 800.0, 800.0, 0, FVector::ZeroVector, 8);
		if (!TestEqual(TEXT("single pole"), Plan.Poles.Num(), 1)) { return false; }
		TestTrue(TEXT("pole at row middle, offset"),
			Plan.Poles[0].GetLocation().Equals(FVector(800.0, 400.0, 0.0), 0.1));
		TestEqual(TEXT("no chain"), Plan.ChainWires.Num(), 0);
	}

	// Yaw rotates the pole lane with the grid.
	{
		const FAIDAPowerPlan Plan = AIDAActionSpec::PlanPower(2, 1, 800.0, 800.0, 90, FVector::ZeroVector, 2);
		if (!TestEqual(TEXT("rotated pole count"), Plan.Poles.Num(), 1)) { return false; }
		TestTrue(TEXT("rotated pole position"),
			Plan.Poles[0].GetLocation().Equals(FVector(-400.0, 400.0, 0.0), 0.1));
	}

	// Spec parse: power defaults true; explicit opt-out + pole override.
	{
		FAIDABuildSpec Spec;
		FString Error;
		TestTrue(TEXT("parses"), AIDAActionSpec::ParseBuildSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "buildable": "Smelter" })")), 200, Spec, Error));
		TestTrue(TEXT("power defaults on"), Spec.bPower);
		TestTrue(TEXT("no pole override"), Spec.Pole.IsEmpty());

		TestTrue(TEXT("opt-out parses"), AIDAActionSpec::ParseBuildSpec(
			AIDATestParseJson(TEXT(R"({ "version": 1, "buildable": "Smelter", "power": false, "pole": "Power Pole Mk.3" })")), 200, Spec, Error));
		TestFalse(TEXT("power off"), Spec.bPower);
		TestEqual(TEXT("pole override kept"), Spec.Pole, TEXT("Power Pole Mk.3"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAActionsProposalStoreTest, "AIDA.Actions.ProposalStore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAActionsProposalStoreTest::RunTest(const FString&)
{
	FAIDAProposalStore Store;
	FString Error;
	const int64 T0 = 1000;

	// Pending cap: a third Add at cap 2 is refused with a model-facing reason.
	FAIDAProposal A; A.Summary = TEXT("a");
	FAIDAProposal B; B.Summary = TEXT("b");
	FAIDAProposal C; C.Summary = TEXT("c");
	TestTrue(TEXT("add A"), Store.Add(A, T0, 2, Error));
	TestTrue(TEXT("add B"), Store.Add(B, T0, 2, Error));
	TestFalse(TEXT("cap refuses C"), Store.Add(C, T0, 2, Error));
	TestTrue(TEXT("cap error is model-facing"), Error.Contains(TEXT("awaiting approval")));
	TestEqual(TEXT("two pending"), Store.NumPending(), 2);

	// The full legal path, and illegal jumps along the way.
	const TArray<FAIDAProposal> All = Store.All();
	if (!TestEqual(TEXT("stored two"), All.Num(), 2)) { return false; }
	const FGuid Id = All[0].Id;
	TestFalse(TEXT("Pending!->Executing"), Store.Transition(Id, EAIDAProposalState::Executing));
	TestFalse(TEXT("Pending!->Undone"), Store.Transition(Id, EAIDAProposalState::Undone));
	TestTrue(TEXT("Pending->Approved"), Store.Transition(Id, EAIDAProposalState::Approved));
	TestFalse(TEXT("Approved!->Executed"), Store.Transition(Id, EAIDAProposalState::Executed));
	TestTrue(TEXT("Approved->Executing"), Store.Transition(Id, EAIDAProposalState::Executing));
	TestTrue(TEXT("Executing->Executed"), Store.Transition(Id, EAIDAProposalState::Executed));
	TestTrue(TEXT("Executed->Undone"), Store.Transition(Id, EAIDAProposalState::Undone));
	TestFalse(TEXT("Undone is terminal"), Store.Transition(Id, EAIDAProposalState::Pending));

	// Approving A freed a pending slot; TTL sweep expires only stale Pending proposals.
	TestTrue(TEXT("slot freed, add C"), Store.Add(C, T0 + 100, 2, Error));
	const TArray<FGuid> Expired = Store.SweepExpired(T0 + 600, /*Ttl*/ 600);
	TestEqual(TEXT("only B expired (C is younger)"), Expired.Num(), 1);
	if (Expired.Num() == 1)
	{
		TestEqual(TEXT("expired state set"), static_cast<int32>(Store.Find(Expired[0])->State), static_cast<int32>(EAIDAProposalState::Expired));
	}
	TestEqual(TEXT("C still pending"), Store.NumPending(), 1);

	// Terminal classification + ResolvedUtc stamping (drives the UI linger/retire window).
	TestTrue(TEXT("Expired is terminal"), FAIDAProposalStore::IsTerminal(EAIDAProposalState::Expired));
	TestFalse(TEXT("Executing is not terminal"), FAIDAProposalStore::IsTerminal(EAIDAProposalState::Executing));
	if (Expired.Num() == 1)
	{
		TestEqual(TEXT("expiry stamps ResolvedUtc"), Store.Find(Expired[0])->ResolvedUtc, static_cast<int64>(T0 + 600));
	}
	FAIDAProposal D; D.Summary = TEXT("d");
	TestTrue(TEXT("add D"), Store.Add(D, T0 + 700, 2, Error));
	{
		const FGuid DId = Store.All().FindByPredicate([](const FAIDAProposal& P) { return P.Summary == TEXT("d"); })->Id;
		Store.Transition(DId, EAIDAProposalState::Rejected, T0 + 800);
		TestEqual(TEXT("terminal transition stamps ResolvedUtc"), Store.Find(DId)->ResolvedUtc, static_cast<int64>(T0 + 800));
	}
	Store.Remove(Id);
	TestTrue(TEXT("removed"), Store.Find(Id) == nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAActionsEntityIdCodecTest, "AIDA.Actions.EntityIdCodec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAActionsEntityIdCodecTest::RunTest(const FString&)
{
	// Lightweight round-trip.
	{
		FAIDAEntityId In;
		In.Type = TEXT("lw");
		In.ClassPath = TEXT("/Game/Buildable/Build_Foundation8x2_01.Build_Foundation8x2_01_C");
		In.Index = 4127;
		In.Pos = FVector(-12000.0, 4500.0, 3000.0);
		In.YawDeg = 90;

		FAIDAEntityId Out;
		TestTrue(TEXT("lw decodes"), AIDAActionSpec::DecodeEntityId(AIDAActionSpec::EncodeEntityId(In), Out));
		TestEqual(TEXT("lw type"), Out.Type, TEXT("lw"));
		TestEqual(TEXT("lw class"), Out.ClassPath, In.ClassPath);
		TestEqual(TEXT("lw index"), Out.Index, 4127);
		TestTrue(TEXT("lw pos"), Out.Pos.Equals(In.Pos, 0.01));
		TestEqual(TEXT("lw yaw"), Out.YawDeg, 90);
	}

	// Actor round-trip carries the recipe.
	{
		FAIDAEntityId In;
		In.Type = TEXT("actor");
		In.ClassPath = TEXT("/Game/Buildable/Build_SmelterMk1.Build_SmelterMk1_C");
		In.RecipePath = TEXT("/Game/Recipes/Buildings/Recipe_SmelterMk1.Recipe_SmelterMk1_C");
		In.Pos = FVector(100.0, 200.0, 300.0);

		FAIDAEntityId Out;
		TestTrue(TEXT("actor decodes"), AIDAActionSpec::DecodeEntityId(AIDAActionSpec::EncodeEntityId(In), Out));
		TestEqual(TEXT("actor recipe"), Out.RecipePath, In.RecipePath);
		TestEqual(TEXT("actor index unset"), Out.Index, static_cast<int32>(INDEX_NONE));
	}

	// Garbage and unknown kinds are refused.
	{
		FAIDAEntityId Out;
		TestFalse(TEXT("rejects non-JSON"), AIDAActionSpec::DecodeEntityId(TEXT("not json"), Out));
		TestFalse(TEXT("rejects unknown t"), AIDAActionSpec::DecodeEntityId(TEXT(R"({"t":"ghost","class":"x","pos":[0,0,0]})"), Out));
		TestFalse(TEXT("rejects missing pos"), AIDAActionSpec::DecodeEntityId(TEXT(R"({"t":"lw","class":"x","idx":1})"), Out));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAActionsReportJsonTest, "AIDA.Actions.ReportJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAActionsReportJsonTest::RunTest(const FString&)
{
	// Dry-run success report carries the §2b contract fields.
	FAIDAProposal P;
	P.Id = FGuid::NewGuid();
	P.Summary = TEXT("place 100 x Foundation 8m x 2m in a 10x10 grid");
	P.Placements.SetNum(100);
	P.Cost.Add({ TEXT("Concrete"), 300 });
	{
		const TSharedPtr<FJsonObject> O = AIDATestParseJson(AIDAActionSpec::BuildDryRunJson(P, 600, true, 0.0));
		if (!TestTrue(TEXT("dry-run is JSON"), O.IsValid())) { return false; }
		TestEqual(TEXT("count"), static_cast<int32>(O->GetNumberField(TEXT("count"))), 100);
		TestEqual(TEXT("status"), O->GetStringField(TEXT("status")), TEXT("awaiting approval"));
		TestTrue(TEXT("affordable present"), O->GetBoolField(TEXT("affordable")));
		TestEqual(TEXT("cost items"), O->GetArrayField(TEXT("cost")).Num(), 1);
		TestEqual(TEXT("expiresInSec"), static_cast<int32>(O->GetNumberField(TEXT("expiresInSec"))), 600);
	}

	// Error report bounds the failure list and counts the omitted rest.
	{
		TArray<FAIDAPlacementFailure> Failures;
		for (int32 i = 0; i < 12; ++i) { Failures.Add({ i, FVector(i, 0.0, 0.0), TEXT("Encroaching other building") }); }
		const TSharedPtr<FJsonObject> O = AIDATestParseJson(AIDAActionSpec::BuildErrorJson(TEXT("12 of 100 placements invalid"), Failures));
		if (!TestTrue(TEXT("error is JSON"), O.IsValid())) { return false; }
		TestEqual(TEXT("bounded to 5"), O->GetArrayField(TEXT("firstFailures")).Num(), 5);
		TestEqual(TEXT("omitted counted"), static_cast<int32>(O->GetNumberField(TEXT("omitted"))), 7);
		TestTrue(TEXT("reason preserved"), AIDAToCompactJson(O.ToSharedRef()).Contains(TEXT("Encroaching")));
	}

	// Blocked placements are ADVISORY on a stored proposal (user rule: validity never blocks the
	// ghost) — the success report carries invalidCount + bounded firstFailures + a nudge note.
	{
		TArray<FAIDAPlacementFailure> Failures;
		for (int32 i = 0; i < 7; ++i) { Failures.Add({ i, FVector(i, 0.0, 0.0), TEXT("Invalid floor!") }); }
		const TSharedPtr<FJsonObject> O = AIDATestParseJson(
			AIDAActionSpec::BuildDryRunJson(P, 600, true, 0.0, nullptr, &Failures));
		if (!TestTrue(TEXT("advisory dry-run is JSON"), O.IsValid())) { return false; }
		TestEqual(TEXT("still awaiting approval"), O->GetStringField(TEXT("status")), TEXT("awaiting approval"));
		TestEqual(TEXT("invalidCount"), static_cast<int32>(O->GetNumberField(TEXT("invalidCount"))), 7);
		TestEqual(TEXT("advisory failures bounded to 5"), O->GetArrayField(TEXT("firstFailures")).Num(), 5);
		TestEqual(TEXT("advisory omitted counted"), static_cast<int32>(O->GetNumberField(TEXT("omitted"))), 2);
		TestTrue(TEXT("build note says nudge"), O->GetStringField(TEXT("note")).Contains(TEXT("nudge")));

		FAIDAProposal M = P;
		M.bManifold = true;
		const TSharedPtr<FJsonObject> OM = AIDATestParseJson(
			AIDAActionSpec::BuildDryRunJson(M, 600, true, 0.0, nullptr, &Failures));
		if (!TestTrue(TEXT("manifold advisory is JSON"), OM.IsValid())) { return false; }
		TestTrue(TEXT("manifold note says standoffM (not nudge)"), OM->GetStringField(TEXT("note")).Contains(TEXT("standoffM")));

		// No failures = no advisory fields at all.
		const TSharedPtr<FJsonObject> Clean = AIDATestParseJson(AIDAActionSpec::BuildDryRunJson(P, 600, true, 0.0));
		TestFalse(TEXT("clean report has no invalidCount"), Clean->HasField(TEXT("invalidCount")));
		TestFalse(TEXT("clean report has no note"), Clean->HasField(TEXT("note")));
	}

	// Status report: filter narrows to one; pending entries carry a countdown.
	{
		FAIDAProposal Q;
		Q.Id = FGuid::NewGuid();
		Q.Summary = TEXT("q");
		Q.State = EAIDAProposalState::Pending;
		Q.ProposedUtc = 1000;
		Q.InvalidCount = 3; // advisory rides into the status report while pending
		const TSharedPtr<FJsonObject> All = AIDATestParseJson(AIDAActionSpec::BuildStatusJson({ P, Q }, FGuid(), /*Now*/ 1100, /*Ttl*/ 600));
		TestEqual(TEXT("status lists both"), static_cast<int32>(All->GetNumberField(TEXT("count"))), 2);
		const TSharedPtr<FJsonObject> One = AIDATestParseJson(AIDAActionSpec::BuildStatusJson({ P, Q }, Q.Id, 1100, 600));
		TestEqual(TEXT("filter narrows"), static_cast<int32>(One->GetNumberField(TEXT("count"))), 1);
		const TArray<TSharedPtr<FJsonValue>>& List = One->GetArrayField(TEXT("proposals"));
		if (TestEqual(TEXT("one entry"), List.Num(), 1))
		{
			TestEqual(TEXT("pending countdown"), static_cast<int32>(List[0]->AsObject()->GetNumberField(TEXT("expiresInSec"))), 500);
			TestEqual(TEXT("pending invalidCount advisory"), static_cast<int32>(List[0]->AsObject()->GetNumberField(TEXT("invalidCount"))), 3);
		}
		TestEqual(TEXT("filtered pendingCount"), static_cast<int32>(One->GetNumberField(TEXT("pendingCount"))), 1);

		// An empty/none-pending status leads with pendingCount 0 and a do-not-fabricate note.
		const TSharedPtr<FJsonObject> None = AIDATestParseJson(AIDAActionSpec::BuildStatusJson({}, FGuid(), 1100, 600));
		TestEqual(TEXT("no pending"), static_cast<int32>(None->GetNumberField(TEXT("pendingCount"))), 0);
		TestTrue(TEXT("none-pending note present"), None->HasField(TEXT("note")));
	}

	// RefundJson codec: items round-trip with their descriptor class paths.
	{
		TArray<FAIDACostItem> Items;
		Items.Add({ TEXT("Concrete"), 300, TEXT("/Game/Resource/Parts/Cement/Desc_Cement.Desc_Cement_C") });
		const FString Json = AIDAActionSpec::CostItemsToJson(Items);
		TestTrue(TEXT("refund json has item"), Json.Contains(TEXT("\"Concrete\"")));
		TestTrue(TEXT("refund json has amount"), Json.Contains(TEXT("300")));
		TestTrue(TEXT("refund json has class"), Json.Contains(TEXT("Desc_Cement_C")));

		const TArray<FAIDACostItem> Back = AIDAActionSpec::ParseCostItems(Json);
		if (TestEqual(TEXT("refund json parses back"), Back.Num(), 1))
		{
			TestEqual(TEXT("round-trip item"), Back[0].Item, TEXT("Concrete"));
			TestEqual(TEXT("round-trip amount"), Back[0].Amount, 300);
			TestEqual(TEXT("round-trip class"), Back[0].ClassPath, Items[0].ClassPath);
		}
		TestEqual(TEXT("garbage refund json -> empty"), AIDAActionSpec::ParseCostItems(TEXT("not json")).Num(), 0);
	}

	// Summaries read like the doc examples.
	FAIDABuildSpec Spec;
	Spec.Buildable = TEXT("Foundation 8m x 2m");
	Spec.Grid.CountX = 10; Spec.Grid.CountY = 10;
	TestEqual(TEXT("build summary"), AIDAActionSpec::SummarizeBuild(Spec), TEXT("place 100 x Foundation 8m x 2m in a 10x10 grid"));
	FAIDADismantleSpec Sel;
	Sel.Buildable = TEXT("Smelter"); Sel.CenterM = FVector(-120.0, 45.0, 0.0); Sel.RadiusM = 50.0; Sel.MaxCount = 20;
	TestEqual(TEXT("dismantle summary"), AIDAActionSpec::SummarizeDismantle(Sel), TEXT("dismantle up to 20 x Smelter within 50 m of (-120, 45)"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAChatCommandsTest, "AIDA.Actions.ChatCommands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAChatCommandsTest::RunTest(const FString&)
{
	FAIDAChatCommand Cmd;
	FString Error;

	// Plain chat passes through untouched.
	TestFalse(TEXT("plain chat"), AIDAChatCommands::TryParse(TEXT("build me a smelter"), Cmd, Error));
	TestFalse(TEXT("aida mid-sentence"), AIDAChatCommands::TryParse(TEXT("hey /aida undo"), Cmd, Error));
	TestFalse(TEXT("prefix but different token"), AIDAChatCommands::TryParse(TEXT("/aidafoo undo"), Cmd, Error));

	// /aida undo variants.
	TestTrue(TEXT("undo default"), AIDAChatCommands::TryParse(TEXT("/aida undo"), Cmd, Error));
	TestTrue(TEXT("undo kind"), Cmd.Kind == FAIDAChatCommand::EKind::Undo);
	TestEqual(TEXT("undo default count"), Cmd.Count, 1);

	TestTrue(TEXT("undo n"), AIDAChatCommands::TryParse(TEXT("  /AIDA UNDO 3  "), Cmd, Error));
	TestTrue(TEXT("undo n kind"), Cmd.Kind == FAIDAChatCommand::EKind::Undo);
	TestEqual(TEXT("undo n count"), Cmd.Count, 3);

	// /aida approve / reject, with and without an explicit proposal id.
	TestTrue(TEXT("approve"), AIDAChatCommands::TryParse(TEXT("/aida approve"), Cmd, Error));
	TestTrue(TEXT("approve kind"), Cmd.Kind == FAIDAChatCommand::EKind::Approve);
	TestFalse(TEXT("approve default id unset"), Cmd.ProposalId.IsValid());

	const FGuid KnownId = FGuid::NewGuid();
	TestTrue(TEXT("reject with id"), AIDAChatCommands::TryParse(
		FString::Printf(TEXT("/aida reject %s"), *KnownId.ToString(EGuidFormats::DigitsWithHyphens)), Cmd, Error));
	TestTrue(TEXT("reject kind"), Cmd.Kind == FAIDAChatCommand::EKind::Reject);
	TestEqual(TEXT("reject id parsed"), Cmd.ProposalId, KnownId);

	TestTrue(TEXT("approve bad id intercepts"), AIDAChatCommands::TryParse(TEXT("/aida approve nonsense"), Cmd, Error));
	TestTrue(TEXT("approve bad id -> None"), Cmd.Kind == FAIDAChatCommand::EKind::None);
	TestFalse(TEXT("approve bad id has usage"), Error.IsEmpty());

	// /aida nudge and /aida rotate (the ghost-adjust commands).
	TestTrue(TEXT("nudge north"), AIDAChatCommands::TryParse(TEXT("/aida nudge north"), Cmd, Error));
	TestTrue(TEXT("nudge kind"), Cmd.Kind == FAIDAChatCommand::EKind::Nudge);
	TestTrue(TEXT("north = -Y"), Cmd.NudgeDir.Equals(FVector(0, -1, 0)));
	TestEqual(TEXT("nudge default 8 m"), Cmd.NudgeDistM, 8.0);

	TestTrue(TEXT("nudge east 2.5"), AIDAChatCommands::TryParse(TEXT("/aida nudge east 2.5"), Cmd, Error));
	TestTrue(TEXT("east = +X"), Cmd.NudgeDir.Equals(FVector(1, 0, 0)));
	TestEqual(TEXT("nudge dist parsed"), Cmd.NudgeDistM, 2.5);

	TestTrue(TEXT("nudge nonsense intercepts"), AIDAChatCommands::TryParse(TEXT("/aida nudge sideways"), Cmd, Error));
	TestTrue(TEXT("nudge nonsense -> None"), Cmd.Kind == FAIDAChatCommand::EKind::None);

	TestTrue(TEXT("rotate default"), AIDAChatCommands::TryParse(TEXT("/aida rotate"), Cmd, Error));
	TestTrue(TEXT("rotate kind"), Cmd.Kind == FAIDAChatCommand::EKind::Rotate);
	TestEqual(TEXT("rotate default 90"), Cmd.RotateDeg, 90);
	TestTrue(TEXT("rotate -90"), AIDAChatCommands::TryParse(TEXT("/aida rotate -90"), Cmd, Error));
	TestEqual(TEXT("rotate parsed"), Cmd.RotateDeg, -90);

	// Malformed/unknown commands still short-circuit, carrying usage text.
	TestTrue(TEXT("bad count intercepts"), AIDAChatCommands::TryParse(TEXT("/aida undo zero"), Cmd, Error));
	TestTrue(TEXT("bad count -> None"), Cmd.Kind == FAIDAChatCommand::EKind::None);
	TestFalse(TEXT("bad count has usage"), Error.IsEmpty());

	TestTrue(TEXT("unknown subcommand intercepts"), AIDAChatCommands::TryParse(TEXT("/aida dance"), Cmd, Error));
	TestTrue(TEXT("unknown -> None"), Cmd.Kind == FAIDAChatCommand::EKind::None);
	TestTrue(TEXT("bare /aida intercepts"), AIDAChatCommands::TryParse(TEXT("/aida"), Cmd, Error));
	TestFalse(TEXT("bare /aida has usage"), Error.IsEmpty());
	return true;
}

// The generated game data pack (docs/PROMPT.md §2): section routing, per-minute conversion,
// generator fuel/water lines, and the name-sorted (cache-stable) ordering.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAPromptPackTest, "AIDA.PromptPack.Build",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAPromptPackTest::RunTest(const FString&)
{
	TArray<FAIDABuildingInfo> Buildings;

	FAIDABuildingInfo Assembler;
	Assembler.Name = TEXT("Assembler");
	Assembler.PowerConsumptionMW = 15.0;
	Assembler.PowerExponent = 1.321929;
	Assembler.FootprintXM = 10.0; Assembler.FootprintYM = 15.0; Assembler.HeightM = 11.0;
	Buildings.Add(Assembler);

	FAIDABuildingInfo CoalGen;
	CoalGen.Name = TEXT("Coal-Powered Generator");
	CoalGen.PowerProductionMW = 75.0;
	CoalGen.Fuels.Add({ TEXT("Coal"), 15.0 });
	CoalGen.Fuels.Add({ TEXT("Compacted Coal"), 7.14 });
	CoalGen.SupplementalM3PerMin = 45.0;
	Buildings.Add(CoalGen);

	FAIDABuildingInfo Belt;
	Belt.Name = TEXT("Conveyor Belt Mk.2");
	Belt.BeltItemsPerMin = 120.0;
	Buildings.Add(Belt);

	FAIDABuildingInfo Miner;
	Miner.Name = TEXT("Miner Mk.1");
	Miner.PowerConsumptionMW = 5.0;
	Miner.ExtractPerMinNormal = 60.0;
	Buildings.Add(Miner);

	FAIDABuildingInfo Foundation;
	Foundation.Name = TEXT("Foundation (2 m)");
	Foundation.FootprintXM = 8.0; Foundation.FootprintYM = 8.0; Foundation.HeightM = 2.0;
	Buildings.Add(Foundation);

	const FString Pack = AIDAPromptPack::Build(AIDAMakeTestRecipes(), Buildings);

	// Section routing: each synthetic building lands in its section with its numbers formatted.
	TestTrue(TEXT("producer line"), Pack.Contains(TEXT("Assembler: 15 MW, exp 1.32, 10x15x11 m")));
	TestTrue(TEXT("generator line"), Pack.Contains(
		TEXT("Coal-Powered Generator: 75 MW out; burns/min: Coal 15 or Compacted Coal 7.14; water 45 m3/min")));
	TestTrue(TEXT("belt line"), Pack.Contains(TEXT("Conveyor Belt Mk.2: 120 items/min")));
	TestTrue(TEXT("miner line"), Pack.Contains(TEXT("Miner Mk.1: 60/min on a normal node, 5 MW")));
	TestTrue(TEXT("structure line"), Pack.Contains(TEXT("Foundation (2 m): 8x8x2 m")));

	// Recipes converted to per-minute: Iron Plate is 3-in/2-out per 6 s craft -> 30 -> 20.
	TestTrue(TEXT("recipe per-min"), Pack.Contains(TEXT("Iron Plate [Constructor, 6s]: Iron Ingot 30 -> Iron Plate 20")));
	TestTrue(TEXT("multi-input joined"), Pack.Contains(TEXT("Iron Plate 30 + Screw 60 -> Reinforced Iron Plate 5")));

	// Cache stability: same inputs in a different order produce byte-identical output.
	Algo::Reverse(Buildings);
	TestEqual(TEXT("order-independent bytes"), AIDAPromptPack::Build(AIDAMakeTestRecipes(), Buildings), Pack);

	// Static guidance is always present.
	TestTrue(TEXT("clock rules"), Pack.Contains(TEXT("## Clock speed rules")));
	TestTrue(TEXT("authority header"), Pack.Contains(TEXT("GAME DATA PACK")));
	return true;
}

// ───────────────────────────── Phase 5 "Imagination" (docs/PHASE5.md) ─────────────────────────────

namespace
{
	FAIDACompletionRequest AIDAMakeImageRequest()
	{
		FAIDACompletionRequest Req;
		Req.Model = TEXT("test-model");
		Req.MaxTokens = 128;

		FAIDAChatMessage Msg;
		Msg.Role = TEXT("user");
		Msg.Content = TEXT("build something like this");
		Msg.Images.Add({ TEXT("image/jpeg"), TEXT("QUJD") }); // "ABC"
		Req.Messages.Add(MoveTemp(Msg));
		return Req;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAAnthropicImageWireTest, "AIDA.Adapters.AnthropicImageWireFormat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAAnthropicImageWireTest::RunTest(const FString&)
{
	const TSharedPtr<FJsonObject> Json = ParseJsonObject(FAnthropicAdapter::BuildRequestBody(AIDAMakeImageRequest()));
	if (!TestTrue(TEXT("valid JSON"), Json.IsValid())) { return false; }

	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	Json->TryGetArrayField(TEXT("messages"), Messages);
	if (!Messages || Messages->Num() != 1) { AddError(TEXT("expected 1 message")); return false; }

	// Content becomes a block array: image (base64 source) first, then the text block.
	const TSharedPtr<FJsonObject> M = (*Messages)[0]->AsObject();
	const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
	if (!TestTrue(TEXT("content is a 2-block array"),
		M->TryGetArrayField(TEXT("content"), Blocks) && Blocks && Blocks->Num() == 2)) { return false; }

	const TSharedPtr<FJsonObject> Img = (*Blocks)[0]->AsObject();
	TestEqual(TEXT("block[0].type"), Img->GetStringField(TEXT("type")), TEXT("image"));
	const TSharedPtr<FJsonObject>* Src = nullptr;
	if (TestTrue(TEXT("image.source object"), Img->TryGetObjectField(TEXT("source"), Src) && Src))
	{
		TestEqual(TEXT("source.type"), (*Src)->GetStringField(TEXT("type")), TEXT("base64"));
		TestEqual(TEXT("source.media_type"), (*Src)->GetStringField(TEXT("media_type")), TEXT("image/jpeg"));
		TestEqual(TEXT("source.data"), (*Src)->GetStringField(TEXT("data")), TEXT("QUJD"));
	}

	const TSharedPtr<FJsonObject> Text = (*Blocks)[1]->AsObject();
	TestEqual(TEXT("block[1].type"), Text->GetStringField(TEXT("type")), TEXT("text"));
	TestEqual(TEXT("block[1].text"), Text->GetStringField(TEXT("text")), TEXT("build something like this"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAOpenAIImageWireTest, "AIDA.Adapters.OpenAIImageWireFormat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAOpenAIImageWireTest::RunTest(const FString&)
{
	const TSharedPtr<FJsonObject> Json = ParseJsonObject(FOpenAICompatAdapter::BuildRequestBody(AIDAMakeImageRequest()));
	if (!TestTrue(TEXT("valid JSON"), Json.IsValid())) { return false; }

	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	Json->TryGetArrayField(TEXT("messages"), Messages);
	if (!Messages || Messages->Num() != 1) { AddError(TEXT("expected 1 message")); return false; }

	const TSharedPtr<FJsonObject> M = (*Messages)[0]->AsObject();
	const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
	if (!TestTrue(TEXT("content is a 2-part array"),
		M->TryGetArrayField(TEXT("content"), Parts) && Parts && Parts->Num() == 2)) { return false; }

	const TSharedPtr<FJsonObject> Img = (*Parts)[0]->AsObject();
	TestEqual(TEXT("part[0].type"), Img->GetStringField(TEXT("type")), TEXT("image_url"));
	const TSharedPtr<FJsonObject>* Url = nullptr;
	if (TestTrue(TEXT("image_url object"), Img->TryGetObjectField(TEXT("image_url"), Url) && Url))
	{
		TestEqual(TEXT("data URL"), (*Url)->GetStringField(TEXT("url")), TEXT("data:image/jpeg;base64,QUJD"));
	}

	const TSharedPtr<FJsonObject> Text = (*Parts)[1]->AsObject();
	TestEqual(TEXT("part[1].type"), Text->GetStringField(TEXT("type")), TEXT("text"));
	TestEqual(TEXT("part[1].text"), Text->GetStringField(TEXT("text")), TEXT("build something like this"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAVisionModelSwitchTest, "AIDA.Adapters.VisionModelSwitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAVisionModelSwitchTest::RunTest(const FString&)
{
	TArray<FAIDAChatMessage> Plain;
	Plain.Add({ TEXT("user"), TEXT("hi") });

	TArray<FAIDAChatMessage> WithImage = Plain;
	WithImage[0].Images.Add({ TEXT("image/jpeg"), TEXT("QUJD") });

	TestEqual(TEXT("text-only -> default"), FLLMClient::ChooseModel(TEXT("m"), TEXT("v"), Plain), TEXT("m"));
	TestEqual(TEXT("images -> vision model"), FLLMClient::ChooseModel(TEXT("m"), TEXT("v"), WithImage), TEXT("v"));
	TestEqual(TEXT("images, no vision model -> default"), FLLMClient::ChooseModel(TEXT("m"), TEXT(""), WithImage), TEXT("m"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAUploadsConfigTest, "AIDA.Config.ParsesUploads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAUploadsConfigTest::RunTest(const FString&)
{
	const FString Base = TEXT(R"("provider": { "type": "anthropic", "baseUrl": "https://x", "model": "m" })");

	FAIDAConfig Config;
	FString Error;
	TestTrue(TEXT("defaults without uploads block"),
		FAIDAConfigLoader::LoadFromString(FString::Printf(TEXT("{ %s }"), *Base), Config, Error));
	TestTrue(TEXT("default enabled"), Config.Uploads.bEnabled);
	TestEqual(TEXT("default maxDimension"), Config.Uploads.MaxDimension, 1568);
	TestEqual(TEXT("default maxImagesPerMessage"), Config.Uploads.MaxImagesPerMessage, 4);

	TestTrue(TEXT("parses uploads block"), FAIDAConfigLoader::LoadFromString(FString::Printf(
		TEXT("{ %s, \"uploads\": { \"enabled\": false, \"maxImageBytes\": 65536, \"maxImagesPerMessage\": 2, \"maxImagesPerRequest\": 3, \"maxDimension\": 800, \"maxStoredImages\": 5, \"ttlSeconds\": 60 } }"),
		*Base), Config, Error));
	TestFalse(TEXT("enabled=false"), Config.Uploads.bEnabled);
	TestEqual(TEXT("maxImageBytes"), Config.Uploads.MaxImageBytes, 65536);
	TestEqual(TEXT("maxImagesPerRequest"), Config.Uploads.MaxImagesPerRequest, 3);
	TestEqual(TEXT("maxStoredImages"), Config.Uploads.MaxStoredImages, 5);
	TestEqual(TEXT("ttlSeconds"), Config.Uploads.TtlSeconds, 60);

	TestFalse(TEXT("rejects out-of-range limits"), FAIDAConfigLoader::LoadFromString(FString::Printf(
		TEXT("{ %s, \"uploads\": { \"maxImageBytes\": 10 } }"), *Base), Config, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAUploadChunkReassemblyTest, "AIDA.Uploads.ChunkReassembly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAUploadChunkReassemblyTest::RunTest(const FString&)
{
	// 40000 bytes = 2 full 16 KiB chunks + a 7232-byte tail.
	TArray<uint8> Payload;
	Payload.SetNum(40000);
	for (int32 i = 0; i < Payload.Num(); ++i) { Payload[i] = static_cast<uint8>(i * 31 + 7); }
	const uint32 Crc = FCrc::MemCrc32(Payload.GetData(), Payload.Num());
	const int32 ChunkBytes = FAIDAImageUploadAssembler::kChunkBytes;

	FAIDAImageUploadAssembler Assembler;
	FString Error;
	TestTrue(TEXT("begin"), Assembler.Begin(TEXT("p1"), TEXT("image/jpeg"), Payload.Num(), 3, 1 << 20, 100, Error));

	for (int32 Seq = 0; Seq < 3; ++Seq)
	{
		const int32 Offset = Seq * ChunkBytes;
		TArray<uint8> Chunk(Payload.GetData() + Offset, FMath::Min(ChunkBytes, Payload.Num() - Offset));
		TestTrue(*FString::Printf(TEXT("chunk %d"), Seq), Assembler.AddChunk(TEXT("p1"), Seq, Chunk, 100, Error));
	}

	TArray<uint8> Out;
	FString MediaType;
	TestTrue(TEXT("commit"), Assembler.Commit(TEXT("p1"), Crc, Out, MediaType, Error));
	TestTrue(TEXT("bytes round-trip"), Out == Payload);
	TestEqual(TEXT("media type carried"), MediaType, TEXT("image/jpeg"));
	TestFalse(TEXT("session consumed"), Assembler.HasSession(TEXT("p1")));

	// Out-of-order chunk kills the session.
	TestTrue(TEXT("begin 2"), Assembler.Begin(TEXT("p1"), TEXT("image/jpeg"), Payload.Num(), 3, 1 << 20, 100, Error));
	TArray<uint8> First(Payload.GetData(), ChunkBytes);
	TestTrue(TEXT("chunk 0"), Assembler.AddChunk(TEXT("p1"), 0, First, 100, Error));
	TArray<uint8> Wrong(Payload.GetData(), 8);
	TestFalse(TEXT("out-of-order rejected"), Assembler.AddChunk(TEXT("p1"), 2, Wrong, 100, Error));
	TestFalse(TEXT("session dead after violation"), Assembler.HasSession(TEXT("p1")));

	// CRC mismatch fails the commit.
	TestTrue(TEXT("begin 3"), Assembler.Begin(TEXT("p1"), TEXT("image/jpeg"), 8, 1, 1 << 20, 100, Error));
	TArray<uint8> Small(Payload.GetData(), 8);
	TestTrue(TEXT("small chunk"), Assembler.AddChunk(TEXT("p1"), 0, Small, 100, Error));
	TestFalse(TEXT("bad crc rejected"), Assembler.Commit(TEXT("p1"), Crc + 1, Out, MediaType, Error));

	// Stale sessions sweep away.
	TestTrue(TEXT("begin 4"), Assembler.Begin(TEXT("p1"), TEXT("image/jpeg"), 8, 1, 1 << 20, 100, Error));
	Assembler.Sweep(100 + FAIDAImageUploadAssembler::kSessionTimeoutSec + 1);
	TestFalse(TEXT("timed-out session swept"), Assembler.HasSession(TEXT("p1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAImageStoreLimitsTest, "AIDA.Uploads.StoreLimits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAImageStoreLimitsTest::RunTest(const FString&)
{
	FAIDAUploadsConfig Config;
	Config.MaxImageBytes = 1024;
	Config.MaxStoredImages = 2;
	Config.TtlSeconds = 60;

	FAIDAImageStore Store;
	Store.Configure(Config);

	TArray<uint8> Bytes = { 1, 2, 3, 4 };
	FString Error;

	// Per-image cap.
	TArray<uint8> Huge;
	Huge.SetNumZeroed(2048);
	TestFalse(TEXT("oversize rejected"), Store.Add(TEXT("image/jpeg"), Huge, TEXT("p1"), 100, Error).IsValid());

	const FGuid A = Store.Add(TEXT("image/jpeg"), Bytes, TEXT("p1"), 100, Error);
	const FGuid B = Store.Add(TEXT("image/jpeg"), Bytes, TEXT("p1"), 101, Error);
	TestTrue(TEXT("A+B stored"), A.IsValid() && B.IsValid() && Store.Num() == 2);
	TestEqual(TEXT("base64 encoded once"), Store.Find(A)->Base64Data, FBase64::Encode(Bytes.GetData(), Bytes.Num()));

	// Count budget: adding a third evicts the oldest (A).
	const FGuid C = Store.Add(TEXT("image/jpeg"), Bytes, TEXT("p1"), 102, Error);
	TestTrue(TEXT("C stored"), C.IsValid());
	TestEqual(TEXT("count budget held"), Store.Num(), 2);
	TestTrue(TEXT("oldest evicted"), Store.Find(A) == nullptr && Store.Find(B) != nullptr);

	// Ownership: only the uploader may reference.
	TestFalse(TEXT("wrong owner refused"), Store.MarkReferenced(B, TEXT("p2")));
	TestTrue(TEXT("owner references"), Store.MarkReferenced(B, TEXT("p1")));

	// TTL: unreferenced C expires, referenced B survives.
	Store.Sweep(102 + Config.TtlSeconds + 1);
	TestTrue(TEXT("unreferenced swept"), Store.Find(C) == nullptr);
	TestTrue(TEXT("referenced survives"), Store.Find(B) != nullptr);

	// Eviction falls back to referenced images when everything evictable is referenced.
	const FGuid D = Store.Add(TEXT("image/jpeg"), Bytes, TEXT("p1"), 200, Error);
	TestTrue(TEXT("D stored"), D.IsValid() && Store.Num() == 2);
	TestTrue(TEXT("D referenced"), Store.MarkReferenced(D, TEXT("p1")));
	const FGuid E = Store.Add(TEXT("image/jpeg"), Bytes, TEXT("p1"), 201, Error);
	TestTrue(TEXT("oldest referenced evicted when all are referenced"),
		E.IsValid() && Store.Num() == 2 && Store.Find(B) == nullptr && Store.Find(D) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDASessionImageRefsTest, "AIDA.Session.ImageRefs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDASessionImageRefsTest::RunTest(const FString&)
{
	FAIDASessionManager Session(8);
	TArray<FGuid> Ids = { FGuid::NewGuid(), FGuid::NewGuid() };

	const FGuid MsgId = Session.PostPlayerMessage(TEXT("Player"), TEXT("like this"), AIDADefaultConversationId(), Ids);

	FAIDATranscriptEntry Entry;
	if (!TestTrue(TEXT("entry stored"), Session.GetMessageBody(MsgId, Entry))) { return false; }
	TestTrue(TEXT("image ids stored"), Entry.ImageIds == Ids);
	TestEqual(TEXT("header carries the count only"), Entry.Header.ImageCount, 2);

	const FGuid PlainId = Session.PostPlayerMessage(TEXT("Player"), TEXT("no images"), AIDADefaultConversationId());
	if (TestTrue(TEXT("plain entry stored"), Session.GetMessageBody(PlainId, Entry)))
	{
		TestEqual(TEXT("no ids by default"), Entry.ImageIds.Num(), 0);
		TestEqual(TEXT("count 0 by default"), Entry.Header.ImageCount, 0);
	}
	return true;
}

// ──────────────────── P5 reconstruction aids (docs/PHASE5-RECONSTRUCTION.md) ────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAPromptPackPaletteTest, "AIDA.PromptPack.ArchitecturePalette",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAPromptPackPaletteTest::RunTest(const FString&)
{
	// The palette is static guidance — present even with no unlocked buildings at all.
	const FString Pack = AIDAPromptPack::Build({}, {});
	TestTrue(TEXT("palette section"), Pack.Contains(TEXT("## Architecture palette")));
	TestTrue(TEXT("material mapping"), Pack.Contains(TEXT("Photo material -> buildable")));
	TestTrue(TEXT("substitution caveat"), Pack.Contains(TEXT("not unlocked")));
	TestTrue(TEXT("worked example is v2"), Pack.Contains(TEXT("\"version\":2")));
	TestTrue(TEXT("module rule"), Pack.Contains(TEXT("storeys at 4 m")));

	// The worked example must be VALID JSON — the model copies its shape into tool args.
	const int32 SpecStart = Pack.Find(TEXT("{\"version\":2"));
	const int32 SpecEnd = Pack.Find(TEXT("]}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SpecStart);
	if (TestTrue(TEXT("example found"), SpecStart != INDEX_NONE && SpecEnd != INDEX_NONE))
	{
		const TSharedPtr<FJsonObject> Spec = AIDATestParseJson(Pack.Mid(SpecStart, SpecEnd - SpecStart + 2));
		if (TestTrue(TEXT("example parses"), Spec.IsValid()))
		{
			FAIDABuildSpec Parsed;
			FString Error;
			TestTrue(TEXT("example passes ParseBuildSpec"), AIDAActionSpec::ParseBuildSpec(Spec, 100, Parsed, Error));
			TestEqual(TEXT("example part count"), Parsed.Parts.Num(), 4);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAMapToolsTerrainProbeTest, "AIDA.MapTools.TerrainProbeJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAMapToolsTerrainProbeTest::RunTest(const FString&)
{
	// 3x3 row-major grid: flat north row, a rise, one probe miss at the southwest corner.
	const TArray<double> HeightsM = {
		10.0, 10.0, 10.0,
		10.0, 12.0, 12.0,
		AIDAMapTools::AIDATerrainNoHit, 14.0, 14.5 };
	const TSharedPtr<FJsonObject> O = AIDATestParseJson(
		AIDAMapTools::BuildTerrainProbeJson(HeightsM, 3, 3, 100.0, 200.0, 8.0));
	if (!TestTrue(TEXT("terrain probe is JSON"), O.IsValid())) { return false; }

	TestEqual(TEXT("cols"), static_cast<int32>(O->GetNumberField(TEXT("cols"))), 3);
	TestEqual(TEXT("rows"), static_cast<int32>(O->GetNumberField(TEXT("rows"))), 3);
	TestEqual(TEXT("centerX"), O->GetNumberField(TEXT("centerX")), 100.0);
	TestEqual(TEXT("stepM"), O->GetNumberField(TEXT("stepM")), 8.0);
	TestEqual(TEXT("minZ"), O->GetNumberField(TEXT("minZ")), 10.0);
	TestEqual(TEXT("maxZ"), O->GetNumberField(TEXT("maxZ")), 14.5);
	TestEqual(TEXT("spreadZ"), O->GetNumberField(TEXT("spreadZ")), 4.5);
	TestEqual(TEXT("noHit counted"), static_cast<int32>(O->GetNumberField(TEXT("noHit"))), 1);

	const TArray<TSharedPtr<FJsonValue>>& Rows = O->GetArrayField(TEXT("heights"));
	if (TestEqual(TEXT("three rows"), Rows.Num(), 3))
	{
		const TArray<TSharedPtr<FJsonValue>>& South = Rows[2]->AsArray();
		if (TestEqual(TEXT("three cells"), South.Num(), 3))
		{
			TestTrue(TEXT("miss is null"), South[0]->IsNull());
			TestEqual(TEXT("row-major order"), South[1]->AsNumber(), 14.0);
		}
	}
	TestTrue(TEXT("legend orients the grid"), O->GetStringField(TEXT("legend")).Contains(TEXT("north")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIDAActionsStatusAsBuiltTest, "AIDA.Actions.StatusAsBuilt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FAIDAActionsStatusAsBuiltTest::RunTest(const FString&)
{
	// An executed 2-part composite: two foundations along +X, one wall a metre up.
	FAIDAProposal P;
	P.Id = FGuid::NewGuid();
	P.Summary = TEXT("place a 2-part composite");
	P.State = EAIDAProposalState::Executed;
	P.SpecJson = TEXT(R"json({"version":2,"parts":[{"buildable":"Foundation (1 m)","grid":{"countX":2,"countY":1}},{"buildable":"Wall"}]})json");
	P.Placements.Emplace(FRotator::ZeroRotator, FVector(0.0, 0.0, 0.0));
	P.Placements.Emplace(FRotator::ZeroRotator, FVector(800.0, 0.0, 0.0));
	P.Placements.Emplace(FRotator::ZeroRotator, FVector(0.0, 0.0, 100.0));
	P.PlacementPartIndex = { 0, 0, 1 };
	P.PartRecipePaths = { TEXT("recipe-a"), TEXT("recipe-b") };

	const TSharedPtr<FJsonObject> Status = AIDATestParseJson(AIDAActionSpec::BuildStatusJson({ P }, FGuid(), 0, 600));
	if (!TestTrue(TEXT("status is JSON"), Status.IsValid())) { return false; }
	const TArray<TSharedPtr<FJsonValue>>& List = Status->GetArrayField(TEXT("proposals"));
	if (!TestEqual(TEXT("one entry"), List.Num(), 1)) { return false; }

	const TSharedPtr<FJsonObject>* AsBuilt = nullptr;
	if (!TestTrue(TEXT("asBuilt present"), List[0]->AsObject()->TryGetObjectField(TEXT("asBuilt"), AsBuilt))) { return false; }
	TestEqual(TEXT("placed"), static_cast<int32>((*AsBuilt)->GetNumberField(TEXT("placed"))), 3);
	TestFalse(TEXT("no planned when complete"), (*AsBuilt)->HasField(TEXT("planned")));
	TestEqual(TEXT("bbox max x in metres"), (*AsBuilt)->GetObjectField(TEXT("max"))->GetNumberField(TEXT("x")), 8.0);

	const TArray<TSharedPtr<FJsonValue>>& Parts = (*AsBuilt)->GetArrayField(TEXT("parts"));
	if (TestEqual(TEXT("two part runs"), Parts.Num(), 2))
	{
		const TSharedPtr<FJsonObject> Slab = Parts[0]->AsObject();
		TestEqual(TEXT("part 0 name from spec"), Slab->GetStringField(TEXT("buildable")), TEXT("Foundation (1 m)"));
		TestEqual(TEXT("part 0 count"), static_cast<int32>(Slab->GetNumberField(TEXT("count"))), 2);
		TestEqual(TEXT("part 0 run max"), Slab->GetObjectField(TEXT("max"))->GetNumberField(TEXT("x")), 8.0);
		const TSharedPtr<FJsonObject> Wall = Parts[1]->AsObject();
		TestEqual(TEXT("part 1 name"), Wall->GetStringField(TEXT("buildable")), TEXT("Wall"));
		TestEqual(TEXT("part 1 z in metres"), Wall->GetObjectField(TEXT("first"))->GetNumberField(TEXT("z")), 1.0);
		TestFalse(TEXT("single placement skips bbox"), Wall->HasField(TEXT("max")));
	}

	// Failed partway: only the first Cursor placements are as-built; planned shows the shortfall.
	P.State = EAIDAProposalState::Failed;
	P.Cursor = 1;
	const TSharedPtr<FJsonObject> Failed = AIDATestParseJson(AIDAActionSpec::BuildStatusJson({ P }, FGuid(), 0, 600));
	const TSharedPtr<FJsonObject>* PartialBuilt = nullptr;
	if (TestTrue(TEXT("failed carries asBuilt"),
		Failed->GetArrayField(TEXT("proposals"))[0]->AsObject()->TryGetObjectField(TEXT("asBuilt"), PartialBuilt)))
	{
		TestEqual(TEXT("partial placed"), static_cast<int32>((*PartialBuilt)->GetNumberField(TEXT("placed"))), 1);
		TestEqual(TEXT("planned total"), static_cast<int32>((*PartialBuilt)->GetNumberField(TEXT("planned"))), 3);
	}

	// Pending proposals never claim as-built geometry; neither do manifold/label/power-only shapes.
	P.State = EAIDAProposalState::Pending;
	const TSharedPtr<FJsonObject> Pending = AIDATestParseJson(AIDAActionSpec::BuildStatusJson({ P }, FGuid(), 0, 600));
	TestFalse(TEXT("pending has no asBuilt"),
		Pending->GetArrayField(TEXT("proposals"))[0]->AsObject()->HasField(TEXT("asBuilt")));
	P.State = EAIDAProposalState::Executed;
	P.bManifold = true;
	const TSharedPtr<FJsonObject> Manifold = AIDATestParseJson(AIDAActionSpec::BuildStatusJson({ P }, FGuid(), 0, 600));
	TestFalse(TEXT("manifold has no asBuilt"),
		Manifold->GetArrayField(TEXT("proposals"))[0]->AsObject()->HasField(TEXT("asBuilt")));
	return true;
}

#undef AIDA_TEST_NEAR

#endif // WITH_AUTOMATION_TESTS
