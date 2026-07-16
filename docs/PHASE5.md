# Phase 5 — "Imagination"

Players attach **reference images from their own disk** (a photo of Fallingwater, a screenshot of
someone else's factory, a napkin sketch) and AIDA studies them and **reconstructs something resembling
them in-game** through the existing P4 action pipeline. The image is a reference *input* supplied by
the player — **not** a game-viewport capture (ARCHITECTURE.md §10 row 5's "screenshot→description"
wording is superseded by this doc).

**P5 exit gate (packaged game):** attach a real photo in the chat widget, send "build something like
this", and AIDA's reply demonstrably references the image content and produces a `propose_build`-family
proposal; approve builds it. Follow-up turns in the same conversation still see the image.

**Decisions locked up front:**
- **Client normalizes, server re-validates.** The client decodes, downscales to ≤ `maxDimension`
  (default 1568 px long edge — Anthropic's sweet spot), re-encodes JPEG q85. The server never trusts
  the client: it re-decodes, re-checks dimensions/bytes/count, and only then stores.
- **No new LLM tools.** Images ride on the *user turn* as content blocks; the model uses the existing
  P2 eyes + P4 hands tools. (A `describe_image` tool would put the image behind a text bottleneck —
  wrong direction.)
- **No DesktopPlatform dependency.** `IDesktopPlatform` lives in `Source/Developer/` and does not link
  in the Shipping game target. The file picker is Win32 `GetOpenFileNameW` behind `#if PLATFORM_WINDOWS`
  (clients are Steam/Windows; the dialog is client-side only). `/aida attach <path>` is the
  keyboard/universal fallback.
- **Images are ephemeral, server-RAM only.** Never written to the save or sidecar; a TTL'd in-memory
  store with byte budgets. A server restart forgets attachments — acceptable, like pending proposals.
- **Other clients see a marker, not pixels.** Transcript entries carry image *IDs*; bytes are never
  replicated to non-owning clients in v1 (no thumbnail fan-out).

---

## 1. Pipeline

```
[client] attach button (Win32 dialog)  or  /aida attach <path>   ← intercepted IN THE WIDGET
  ├─ load file → decode (ImageWrapper: png/jpg/bmp) → downscale ≤1568 → re-encode JPEG q85
  ├─ ServerBeginImageUpload(mediaType, totalBytes, chunkCount)
  ├─ ServerImageUploadChunk(seq, bytes[16 KB])   ── ack-paced window (§3, reliable-buffer safety)
  └─ ServerCommitImageUpload(crc32)
[server] re-decode + validate (uploads.* caps, gate) → FAIDAImageStore add (TTL, budgets)
  └─ ClientImageUploadResult(ok, imageId, error)      → widget shows 📎 chip (pending attachment)
[client] player types message → ServerSendChatWithImages(text, convId, [imageId…])
[server] HandleChatRequest: validate IDs (owner = requester, live) → PostPlayerMessage(…, imageIds)
  └─ transcript entry carries ImageIds → BuildChatContext attaches FAIDAImagePart to that user turn
       └─ adapters emit image content blocks; LLMClient picks provider.visionModel when images present
            └─ model sees pixels + full tool catalog → propose_build/… → normal P4 approval flow
```

`/aida attach` **must** be parsed client-side (deviation from the server-side `AIDAChatCommands`
pattern): the argument is a path on the *client's* disk. The widget intercepts the prefix before
`SubmitChat`; everything else still round-trips to the server parser.

## 2. Data shapes

```cpp
// Public/Adapters/AIDALLMTypes.h
struct FAIDAImagePart {                 // travels inside a chat message to the adapters
    FString MediaType;                  // always "image/jpeg" after client normalization
    FString Base64Data;                 // encoded once at store-commit, reused every request
};
// FAIDAChatMessage gains: TArray<FAIDAImagePart> Images;   (empty = today's behavior, bit-for-bit)
```

**Anthropic** (`BuildAnthropicMessage`, plain-string `else` branch): when `Images` is non-empty the
content becomes a block array — image blocks first, then the text block:
`[{ "type":"image", "source":{ "type":"base64", "media_type":"image/jpeg", "data":"…" } }, { "type":"text", "text":"…" }]`

**OpenAI-compat** (`AppendOpenAIMessage`): content array of
`{ "type":"image_url", "image_url":{ "url":"data:image/jpeg;base64,…" } }` + `{ "type":"text", … }`.

**Model switch:** `FLLMClient` keeps `VisionModel` from config; a pure static
`FLLMClient::ChooseModel(bDefaultModel, visionModel, bAnyImages)` picks `visionModel` iff non-empty and
the request carries images (haiku 4.5 is already vision-capable, so an empty `visionModel` still works).

```cpp
// Public/Core/AIDAImageStore.h — plain C++, game-header-free, unit-testable
struct FAIDAStoredImage { FString MediaType, Base64Data, OwnerPlayerId; int64 CreatedUtc; int32 SourceBytes; };
class FAIDAImageStore {   // TMap<FGuid, FAIDAStoredImage>; lazy TTL sweep (proposal-store pattern)
    // Add() enforces: per-image bytes, MaxStoredImages, TotalByteBudget (evict-oldest-unreferenced)
    // MarkReferenced(id) — images cited by a sent chat message survive TTL until ring-buffer eviction
};
class FAIDAImageUploadAssembler {       // one in-flight upload per player
    // Begin(mediaType,totalBytes,chunkCount) / AddChunk(seq,bytes) — strict in-order seq
    // Commit(crc32) → TArray<uint8> or error (crc mismatch, short/overlong, timeout 60 s)
};
```

`FAIDATranscriptEntry` gains `TArray<FGuid> ImageIds` (IDs only — cheap to replicate in the existing
body/transcript RPCs; other clients render a `📎 image` marker).

**BuildChatContext:** for user turns with `ImageIds`, resolve against the store and fill
`FAIDAChatMessage::Images`. Only the **newest `maxImagesPerRequest` (default 4)** images across the
whole window are attached (older ones degrade to an appended `[attached image no longer available]`
note, same as expired/evicted IDs) — bounds token cost on long conversations.

## 3. Upload transport (the part with sharp edges)

New `UAIDARemoteCallObject` UFUNCTIONs (all existing conventions: `Server, Reliable, WithValidation`;
requester via `ResolveRequester()`, empty-net-id listen-host rule applies):

```cpp
void ServerBeginImageUpload(const FString& MediaType, int32 TotalBytes, int32 ChunkCount);
void ServerImageUploadChunk(int32 Seq, const TArray<uint8>& Data);      // ≤ kAIDAUploadChunkBytes
void ServerCommitImageUpload(uint32 Crc32);
void ServerSendChatWithImages(const FString& Text, const FGuid& ConversationId, const TArray<FGuid>& ImageIds);
UFUNCTION(Client, Reliable) void ClientImageUploadAck(int32 UpToSeq);   // window pacing
UFUNCTION(Client, Reliable) void ClientImageUploadResult(bool bOk, const FGuid& ImageId, const FString& Error);
```

**Why chunked + ack-paced:** UE reliable bunches queue per channel; a burst of large reliable RPCs
overflows the reliable buffer (`RELIABLE_BUFFER = 256` partial bunches) and **disconnects the client**.
Constants chosen conservatively: `kAIDAUploadChunkBytes = 16384`, in-flight window = **2 chunks**;
the client sends the next chunk only on `ClientImageUploadAck`. A typical normalized image
(300–600 KB) is 20–40 chunks ≈ 1–2 s on LAN RTTs; the listen host is effectively instant (local call).
`Validate` rejects oversize chunks/counts outright (`kMaxChunkCount = 256` → hard 4 MB wire ceiling).

Upload sessions are keyed by requester `PlayerId` (one in-flight per player; a new `Begin` discards the
old session). A successful `Begin` answers `ClientImageUploadAck(-1)` ("session open — send the first
window"). Out-of-order `Seq`, byte-count mismatch, or CRC failure aborts with a
`ClientImageUploadResult(false, …)` — the widget paints the chip red with the error text.
Listen-host caveat baked into the client: local RPCs run synchronously, so the ack handler re-enters
mid-send — the chunk index is claimed *before* the send call.

## 4. Config (`uploads` block, `FAIDAUploadsConfig`, gated like `actions.enabled`)

```jsonc
"uploads": {
  "enabled": true,             // master gate; off → upload RPCs refuse (the chip shows the error)
  "maxImageBytes": 3145728,    // post-normalization JPEG ceiling (server-checked)
  "maxImagesPerMessage": 4,
  "maxImagesPerRequest": 4,    // history window cap fed to the LLM (§2)
  "maxDimension": 1568,        // client downscale target; server rejects > 2× this
  "maxStoredImages": 16,       // + total budget 32 MiB, evict oldest unreferenced
  "ttlSeconds": 600            // unreferenced uploads only; referenced follow the transcript ring
}
```

Privacy note (README/config comments): attached images are sent to the configured LLM provider —
that's the entire point — and `uploads.enabled=false` is the server admin's off switch.

## 5. Widget UX (all C++-constructed, no Blueprint changes — TabBar/ProposalUI pattern)

- **📎 button** left of the input box → Win32 open-file dialog (filter: `*.jpg;*.jpeg;*.png;*.bmp`),
  multi-select up to `maxImagesPerMessage`. Hidden on non-Windows.
- `/aida attach <path>` typed in chat = same code path (client-side intercept).
- Pending attachments render as **chips** above the input box (`filename (412 KB) ✕`), per
  conversation; ✕ removes. Sending a message consumes the chips (their IDs go in
  `ServerSendChatWithImages`); switching tabs preserves them.
- Upload progress: chip shows `⏳ n%` until `ClientImageUploadResult`; failure turns the chip red with
  the error tooltip and a local System line.
- Transcript: own + others' messages with images get a dimmed `📎 image ×N` prefix line.

**System prompt** (when `uploads.enabled`): one added paragraph — reference images may be attached to
player messages; study composition/massing/materials and translate them into buildable proposals with
the existing tools; never claim to see an image when none is attached.

## 6. Slices

- **Slice 0 — wire format (pure).** `FAIDAImagePart` + `FAIDAChatMessage::Images`; both adapters emit
  image content blocks; `ChooseModel` vision switch; `FAIDAUploadsConfig` + loader. Tests:
  `AIDA.Adapters.AnthropicImageWireFormat`, `AIDA.Adapters.OpenAIImageWireFormat`,
  `AIDA.Adapters.VisionModelSwitch`, `AIDA.Uploads.ConfigParse`.
- **Slice 1 — server upload path.** `FAIDAImageStore` + `FAIDAImageUploadAssembler`; RCO RPCs +
  orchestrator handlers; `ServerSendChatWithImages` → `PostPlayerMessage(…, imageIds)`;
  `FAIDATranscriptEntry::ImageIds`; `BuildChatContext` attachment + history cap. Tests:
  `AIDA.Uploads.ChunkReassembly`, `AIDA.Uploads.SizeLimit`, `AIDA.Uploads.StoreTtl`,
  `AIDA.Uploads.HistoryCap`, `AIDA.Session.ImageRefs`.
- **Slice 2 — client.** ImageWrapper normalization helper (test: `AIDA.Uploads.NormalizeRoundTrip` on a
  generated bitmap); Win32 picker; `/aida attach` intercept; chips; paced chunk sender; prompt line.
- **Slice 3 — live verify (packaged, SML-not-in-PIE rule).** Exit gate above + multi-image message +
  `uploads.enabled=false` refusal + oversized-file rejection + second-client marker rendering.

**Highest-risk assumptions (verify first in Slice 3):** (a) ack-paced 16 KB reliable chunks never trip
the reliable buffer on a real connection; (b) `ImageWrapper` decode/encode is available and fast enough
on the Shipping client *and* the Linux dedicated server; (c) haiku-tier vision output is good enough to
drive `propose_build` — if not, `provider.visionModel` points at a stronger model for image turns only.
