# Plots — charts in chat + an always-visible graph panel

Companion to PHASE8.md ("Foreman"): the display half of factory observability. Two surfaces, one
data spine — trends render as **inline charts in the chat transcript** and on a **persistent HUD
panel** (minimap-style, visible while playing, chat open or not).

Design constraints carried over: server is authoritative (clients render replicated views, same as
proposals/ghosts); the model never fabricates numbers (it quotes tool output; sparklines are
server-rendered so bucketing can't be mangled); everything is config-gated.

---

## 0. What already exists (grounded 2026-07-18)

| Piece | Where | Consequence |
|---|---|---|
| Power history, free | `UFGPowerCircuit::GetStats(FPowerCircuitStats&)` → `GetProducedPoints/GetConsumedPoints/GetProductionCapacityPoints/GetBatteryPower*Points` (`FGPowerCircuit.h:171-203`) | The power series needs NO sampler — the game already records it (this is the power-pole UI graph). Aggregate across circuit groups. |
| Rich-text transcript | `TranscriptRich = URichTextBlock` with a programmatic style set (`AIDAChatWidget.cpp:524-564`) | Custom `URichTextBlockDecorator` can inline REAL widgets in messages → true charts in chat, not just glyph art. |
| Point-in-time factory rates | P2/P3: FactoryIndex snapshot, `take_snapshot`/`compare_to` | Item-rate sampling reuses the cached index — never a fresh extraction per sample. |
| Widget lifecycle + viewport add | chat widget creation/registration in the module | The HUD panel follows the same recipe (create on world join, per-player, toggle key). |
| Server→client push | `AAIDAChatRelay` / `AAIDAProposalRelay` (SpawnOnServer_Replicate) | A third small relay (or a section on the proposal relay) replicates pinned-series data. |

## 1. Data spine — `FAIDAHistorySampler` (server)

- Ring buffer per series: `FAIDASeries { Name, Unit, float Samples[720] }` — 1 h at 5 s cadence,
  fixed cadence so timestamps are implicit. In-memory only in v1 (a save reload starts fresh;
  persistence via the P3 layer is a later nicety).
- Sampled on the orchestrator's existing timer (5 s accumulator over the action tick — no new
  timers). Sources:
  - **Power** (always on): produced / consumed / capacity, aggregated over all circuit groups.
    Current values sampled live; the panel's detailed power strip reads `FPowerCircuitStats`
    directly for its window.
  - **Item rates** (tracked series only): per-item production and consumption per minute from the
    cached FactoryIndex. Tracked = pinned series + a handful of top movers, capped by
    `history.maxSeries` (default 16) — NOT every item; sampling cost stays flat.
- **`get_history(series?, windowMinutes?)`** Query tool: omit `series` to list what's tracked
  (with current values); name one to get min/max/avg/now, trend % vs. window start, a downsampled
  point array, and a **server-rendered sparkline string** (`▁▂▄▆█▇▅`, fixed 24 buckets). Prompt
  guidance: any "trend / over time / lately" question calls this and quotes the sparkline line
  verbatim; `compare_to` stays the two-point diff tool.

## 2. Chat surface

- **Slice A (text, ships with the sampler):** replies carry sparkline rows —
  `Iron Plate  ▂▃▅▆▇█ 42/min (+18% over 30 m)`. Zero UI work; the transcript font is already
  monospace-adjacent and the glyphs render in the game font's fallback.
- **Slice C (real inline charts):** a custom `URichTextBlockDecorator` registered on
  `TranscriptRich` handles `<chart series="Iron Plate" window="30"/>` by inlining an `SAIDAPlot`
  widget (shared with the HUD panel). The model emits a fenced block (```chart Iron Plate```)
  which the markdown converter rewrites to the decorator tag — the converter is the ONLY place
  that writes the tag, so untrusted model text can't inject arbitrary decorators. CAUTION: the
  markdown converter has bitten before (the `---` infinite loop that froze the game) — new
  rewrite rules get converter unit tests first, same as that fix.
- Charts in chat render from the replicated series cache (§4), so clients see the same picture as
  the host without extra round trips.

## 3. HUD surface — `UAIDAGraphPanel` (the "minimap" widget)

- A compact, always-visible overlay (default top-right, below the compass), independent of the
  chat window; created per local player alongside the chat widget, toggled with **Ctrl+G**
  (same preprocessor that owns Ctrl+Enter/Ctrl+Wheel), `hud.enabled` config to kill it entirely.
- v1 contents, top to bottom:
  1. **Power strip** — produced vs. consumed vs. capacity as a mini area chart + headroom number,
     turning red under `hud.powerWarnPct` headroom (default 10%). This is the "am I about to
     trip" gauge and justifies the panel by itself. Data: `FPowerCircuitStats` window.
  2. **Pinned series** — up to `hud.maxPins` (default 3) rows: name, `SAIDAPlot` sparkline,
     current value, trend arrow.
  3. (later, P8 Slice 1) an alert badge fed by `get_alerts`.
- Collapsed state = a single power bar; click or Ctrl+G cycles collapsed/expanded/hidden.
- **Pinning:** `/aida pin <item|power>` and `/aida unpin <item>` chat commands, plus a
  **`pin_chart(series)`** tool so "keep an eye on screws" works conversationally. Pins are
  per-player (local UI state); the tool pins for the requesting player only. No approval flow —
  display-only, like `mark_location`'s ping.
- Rendering: one new Slate leaf widget **`SAIDAPlot`** — `OnPaint` with
  `FSlateDrawElement::MakeLines` (polyline + optional baseline fill), min/max/now labels. No
  textures, no material assets (CSP-of-the-mod-world: the plugin ships no content for this).
  Shared by the HUD panel and the chat decorator.

## 4. Replication

- Server tracks the union of all players' pins + power. A replicated array on a small new relay
  (`AAIDAGraphRelay`, same `SpawnOnServer_Replicate` recipe as the other two):
  `FAIDASeriesView { Name, Unit, float Samples[64], NowValue, TrendPct }`, refreshed at 0.5 Hz —
  64 downsampled points is plenty for a sparkline-sized chart and keeps the payload trivial.
- Late joiners get state for free (replicated array), same as proposals.
- Single-player/listen host reads the same path (no special case).

## 5. Slices & gates

| Slice | Contents | Gate |
|---|---|---|
| A | Sampler + `get_history` + sparklines in replies + prompt guidance | "how has iron plate production trended over 30 min?" → sparkline + numbers agree with `compare_to` over the same window |
| B | `SAIDAPlot`, HUD panel (power strip + pins), Ctrl+G, `/aida pin`, `pin_chart`, graph relay | 2-player: client pins Screws, sees the same curve as the host; power strip goes red before a staged fuse trip |
| C | Rich-text chart decorator + markdown fence rewrite (+ converter tests) | "show me a graph of screw production" renders an actual inline chart in the transcript on host AND client |

A ships inside P8 Slice 1 (same timer work as `get_alerts`); B and C are independent of P8's
mutation slices and can interleave anywhere.

## 6. Config

```jsonc
"history": { "enabled": true, "sampleSeconds": 5, "windowMinutes": 60, "maxSeries": 16 },
"hud":     { "enabled": true, "maxPins": 3, "powerWarnPct": 10 }
```

## 7. Risks / open questions

1. **Markdown converter changes** are the highest-regression-risk area in the mod's history —
   Slice C lands last, behind converter unit tests, and the fence→tag rewrite must be the only
   producer of decorator tags.
2. Sparkline glyphs (`▁▂▃…`) depend on the game font's fallback — verify in the packaged game
   during Slice A's gate; fallback is ASCII bars (`._-=#`).
3. Item-rate sampling fidelity: FactoryIndex refresh cadence vs. 5 s samples — if the index
   refreshes slower, samples repeat (acceptable for trends; note it in `get_history` output).
4. Panel placement vs. game HUD (compass, boombox, radiation warnings) — pick the corner in live
   verify; make the anchor a config string if it fights anything.
