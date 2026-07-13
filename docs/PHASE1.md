# Phase 1 "Voice" — implementation notes & verification

This covers the streaming chat round-trip delivered in Phase 1 (Slices 1–4). The C++ is complete;
the only remaining work is **authoring the ChatWidget Blueprint** (Slice 2's view) and running the
two-client verification passes in-editor.

## What shipped (C++)

| Area | Files | Role |
|------|-------|------|
| Chunk protocol | `Net/AIDANetTypes.h` | `FAIDAMessageHeader`, `FAIDAChunk`, `FAIDATranscriptEntry` (replicated events, not a growing string) |
| Server→all relay | `Net/AIDAChatRelay.{h,cpp}` | `AModSubsystem` (SpawnOnServer_Replicate); batches deltas (~6 Hz) → `Multicast_MsgBegin/Chunk/End`; client-side assembly + `OnMsgBegin/Chunk/End` delegates |
| Client→server surface | `Net/AIDARemoteCallObject.{h,cpp}` | `UFGRemoteCallObject`: `ServerSendChat`, `ServerRequestMessageBody`, `ServerRequestRecentTranscript`; registered on `GameModeInitializedEvent` |
| Transcript owner | `Core/AIDASessionManager.{h,cpp}` | Ring buffer (200), id assignment, authoritative bodies, fan-out driver |
| Orchestration | `Core/AIDAOrchestrator.{h,cpp}` | `HandleChatRequest` → permission → rate-limit → post → `StartAIDAReply` (privacy-filtered context → `LLMClient::CompleteChat` streaming out) |
| Policy | `Core/AIDARateLimiter.{h,cpp}`, `Core/AIDAPermissionService.{h,cpp}` | Token bucket (per-player + global); chat/query/act tiers |
| Client view base | `UI/AIDAChatWidget.{h,cpp}` | `UUserWidget` base: binds the relay, drives late-join sync, sends chat; BP subclass is a pure view |

Unit tests (`Tests/AIDATests.cpp`): `AIDA.Session.*`, `AIDA.Policy.RateLimiter`, `AIDA.Policy.Permissions`.

## Slice 1 verification — WITHOUT a widget (do this first)

The relay logs everything, and `AIDA.Say` injects a chat through the full relay path, so the streaming
round-trip is verifiable before any UMG work. (`AIDA.Ping` does NOT — it calls the LLM directly and
bypasses the relay/replication; it only proves the server-side LLM stream.)

1. Build `FactoryEditor`, open the editor. In the toolbar next to Play: set **Net Mode = Play As Listen
   Server** and **Number of Players = 2**. Press Play — you get two game windows (window 1 = host/server,
   window 2 = a remote client).
2. Open the Output Log (Window → Output Log). Raise verbosity so the relay traces show: in the console
   type `Log LogAIDA Verbose`. Do this for whichever window(s) you want to watch — Output Log is shared
   in PIE, but each client's `[relay]` lines are prefixed by its own world, so watch the log while
   focusing each window, or add `-LogCmds="LogAIDA Verbose"`.
3. In the **host window** (window 1) press `~` for the console and run: `AIDA.Say hello there`.
   - This posts the "DebugPlayer" message (fans out Begin+Chunk+End) and, if an LLM is configured,
     streams the AIDA reply out too. Even with no LLM configured you still see the player message and a
     System notice replicate — enough to prove the relay.
4. Expected in the log for the **second client** (window 2): `[relay] MsgBegin id=… author=DebugPlayer`,
   then `[relay] MsgChunk id=… seq=0 …` (and more seqs as the AIDA reply streams), then
   `[relay] MsgEnd … ok`. Seeing those on the non-host client is the P1 core: two clients watching one
   answer stream in live. If the LLM is configured you'll watch the AIDA reply arrive in incremental
   chunks rather than one blob.

## Slice 2 — author the ChatWidget Blueprint (editor, GUI)

The C++ base `UAIDAChatWidget` does all the wiring; the BP only renders.

1. **Create the widget BP:** Content Browser → `Mods/AIDA/Content/UI/` → *Widget Blueprint* →
   **pick `AIDAChatWidget` as the parent class** (not plain UserWidget). Name it `WBP_AIDAChat`.
2. **Lay out the view:** a scroll box / `ListView` for messages + an `EditableTextBox` + a Send button.
3. **Wire input:** on text committed / Send clicked → call **`SendChat`** (inherited) with the box text,
   then clear the box.
4. **Wire the view to the three events** (override the BlueprintImplementableEvents):
   - `OnMessageBegin(Header)` — **upsert by `Header.Id`**: create the row if new, otherwise **reset its
     text to empty**. (This idempotency is what makes late-join/recovery converge — the same message may
     be re-begun when the authoritative body is refetched.)
   - `OnMessageDelta(Id, Delta)` — **append** `Delta` to that row's text.
   - `OnMessageEnd(Id)` — finalize the row (stop any typing indicator).
   Use `Header.Kind` (Player/AIDA/System) and `Header.Author` for styling/badges.
5. **Show it:** inject `WBP_AIDAChat` into a HUD panel via SML's `UWidgetBlueprintHookManager`
   (Direct/Indirect_Child into a named panel of an existing game widget), **or** add it to the viewport
   from a client-side input action for a first pass. Injection is the shippable path; viewport-add is
   fine for testing.

Because `OnMessageBegin` is an upsert-reset, the widget needs no other dedupe logic — replaying the
existing transcript on open and receiving a recovered body both funnel through the same three events.

## Slice 3 verification — late join + recovery (P1 exit gate)

1. Two-client listen-server PIE. Player A chats; AIDA answers (both see it stream).
2. **Late join:** connect Player B *after* an answer completed. On the widget binding, B calls
   `ServerRequestRecentTranscript` and should populate the full prior transcript. Log line on B:
   `[relay] recovered body …` per message.
3. **Gap/hash:** a mid-stream joiner that misses `MsgBegin`/early chunks will log
   `[relay] MsgEnd hash mismatch/gap … requesting recovery`, then `[relay] recovered body …`, and end
   with the correct full text. That closes the P1 exit gate ("a late-joiner recovers the transcript").

## Slice 4 verification — policy

- **Rate limit:** set `limits.perPlayerPerMinute: 2` in `Configs/AIDA/config.jsonc`; send 3 quickly →
  the 3rd returns an in-chat System notice and logs `Chat THROTTLED … [id]`. No LLM call is made.
- **Permission:** set `permissions.chat` to anything other than `"everyone"` with your id absent from
  `permissions.act` → chat is refused with a System notice and logs `Chat DENIED (permission) … [id]`.
- **Privacy:** `privacy.sendPlayerNames: false` drops author names from the LLM context;
  `privacy.sendChatHistoryDepth: N` bounds how many prior messages are sent. Both are applied in
  `AIDAOrchestrator::BuildChatContext` — the transcript shown in-game is unaffected; only what leaves
  the server changes.

## Notes / follow-ups

- `PlayerId` is the player's unique-net-id string (used for rate buckets and the `act` allowlist). Verify
  it matches the Epic account id format admins put in `permissions.act` on a real dedicated server.
- System prompt / tool context is still empty (`FLLMClient::SystemPrompt`) — that's Phase 2 (Eyes) work.
- Two-client + late-join is the mandated manual pass for any chunk-protocol change (docs/DEV.md §6).
- **Denial/throttle notices currently post to the shared transcript** (all clients see them), matching
  the architecture's "friendly in-chat errors." If per-player privacy is wanted, route those through an
  owner-only `Client` RPC on the RCO instead of `SessionManager::PostSystemMessage` — a small follow-up.
