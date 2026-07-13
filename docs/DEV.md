# AIDA — Developer Setup & Guidelines

This document covers the development environment for AIDA (Windows build machine + optional macOS remote workflow), the day-to-day dev loop, and the engineering conventions for the codebase. For system design, see `docs/architecture.md`.

> **Version lock warning.** The Satisfactory modding toolchain is version-locked to the game. Do **not** grab "latest" of anything below — always follow the exact versions listed in the current SML docs at <https://docs.ficsit.app>. Toolchain mismatch is the single most common setup failure. When the game receives a major patch, expect the toolchain to lag by days-to-weeks; pin your local game version during development.

### Current version pins (verified working — last confirmed 2026-07-13)

Snapshot of the exact versions in use on the current dev setup. Re-verify against the SML docs after any game update; these are a reference, not a substitute for that warning.

| Component | Pinned version | Notes |
|-----------|----------------|-------|
| Satisfactory (game) | **1.2.3.1**, CL **495413**, branch `++FactoryGame+rel-main-1.2.0` | UE 5.6.1. In Steam, set "Only update this game when I launch it" to hold the pin. |
| Unreal Engine fork | **5.6.1-CSS** (prebuilt) | Registered engine id `5.6.1-CSS`; installed at a short path (e.g. `C:\UE_CSS`). |
| SML (SatisfactoryModLoader) | **3.12.0** | `GameVersion >=491125`; branch `master`. |
| Wwise | **2023.1.14** | Integration `.3555`, SDK `.8770`. Deploy platforms: Windows `vc170` (VS 2022) + Linux. Source of truth: SML CI `.github/workflows/build.yml`. |
| Linux cross-compile toolchain | **v25_clang-18.1.0-rockylinux8** | From engine `Config/Linux/Linux_SDK.json` (min==max, exact). Installer sets `LINUX_MULTIARCH_ROOT`. |
| MSVC toolset | prefer **14.38.33130** | UE 5.6.1's preferred v143 minor. Newer toolsets (e.g. 14.44) build with a "not a preferred version" warning; add 14.38 via the VS Installer if you hit compiler issues. |

---

## 1. Windows Machine Setup (required)

All compiling, packaging, and game/editor testing happens on Windows. Work through these in order.

### 1.1 Game

Install Satisfactory (Steam or Epic). Note your install path and Changelist (CL) number — you'll need both. Disable auto-updates if your launcher allows it, or be prepared to hold back mod testing after game patches.

### 1.2 Visual Studio 2022

Install with these workloads/components (cross-check the exact list in the SML "Dependencies" docs page):

- Game development with C++
- .NET desktop development
- Windows 10/11 SDK
- MSVC v143 toolset and C++ profiling/CMake tools as listed by the SML docs

### 1.3 Custom Unreal Engine (Coffee Stain fork)

Satisfactory uses a modified UE5. You must build mods against Coffee Stain's fork, not stock UE.

1. Link your Epic Games account to GitHub (grants access to UE source — required for the fork's releases).
2. Join the `satisfactorymodding` GitHub org flow described in the SML docs and download the **prebuilt engine installer** for the current game version.
3. Install it side-by-side with any stock UE you may have; never open the modding project with stock UE.

### 1.4 Wwise

The modding project requires Audiokinetic Wwise integration to build. Register a free Audiokinetic account, install the Wwise launcher, and integrate the exact Wwise version specified by the SML docs into the project (the docs have a step-by-step page; follow it precisely — this is the fiddliest step).

### 1.5 SML Starter Project + Alpakit

1. Install Git and Git LFS (`git lfs install`).
2. Clone the SatisfactoryModLoader repository (this is the mod development project; AIDA lives inside it as a plugin under `Mods/AIDA/`).
3. Generate VS project files, build the `FactoryGameEditor` target, open the editor once to verify.
4. Alpakit (bundled) packages the mod: use **Alpakit Dev** for fast local install to your game, **Alpakit Release** for shippable multi-target builds.

### 1.6 Linux Cross-Compile Toolchain

Install the UE Linux cross-compile toolchain (clang; exact version in the SML docs). Verify by producing a `LinuxServer` build of the project. All server-affecting changes must build for this target — CI enforces it, but catch it locally first.

### 1.7 Docker Desktop + WSL2

Used to run a real Linux dedicated server for the multiplayer test loop.

- Install Docker Desktop (accept the WSL2 backend prompts).
- Use the community Satisfactory dedicated server image; mount a volume for `FactoryGame/Mods` and `Configs/` so you can drop in Alpakit output and AIDA config.
- Keep a `docker-compose.yml` for the test server in `tools/devserver/` in this repo.

### 1.8 Satisfactory Mod Manager (SMM)

Install SMM to test the end-user installation path before releases.

### 1.9 Remote Access Stack (for the Mac workflow)

- **OpenSSH Server**: Windows Settings → Optional Features → OpenSSH Server; set the service to start automatically.
- **Sunshine** (streaming host) — pairs with Moonlight on the Mac. Configure a virtual display or use a dummy HDMI plug if the machine runs headless.
- **Tailscale** for private network access. Never port-forward RDP or SSH to the open internet.
- **Wake-on-LAN**: enable in BIOS/UEFI and on the NIC (Device Manager → adapter → Power Management + Advanced → "Wake on Magic Packet").
- Power settings: never sleep while remote sessions are expected.

---

## 2. macOS Machine Setup (optional cockpit)

The Mac edits and orchestrates; it never compiles or runs the game.

1. **VS Code + Remote-SSH extension**, or JetBrains Gateway with **Rider** (recommended for Unreal C++ if you have a license). All editing happens directly on the Windows filesystem over SSH.
2. **Moonlight** client — use for the Unreal editor and any in-game testing. RDP is acceptable for light editor work; Moonlight for anything interactive.
3. **Tailscale**.
4. Homebrew conveniences: `brew install wakeonlan` and an SSH config entry, e.g.:

```
# ~/.ssh/config
Host devbox
  HostName <tailscale-name-or-ip>
  User <windows-user>
```

```bash
# wake + connect
wakeonlan <MAC-address> && sleep 20 && ssh devbox
```

Typical session: wake the box → VS Code Remote-SSH for code → build via terminal (`Build.bat` / Alpakit commandlet) → Moonlight when you need the editor or the game.

---

## 3. The Dev Loop

C++ hot-reload in Unreal is unreliable; the standard loop is:

1. Edit code (Mac over SSH, or locally on Windows).
2. Build from the command line: `Engine\Build\BatchFiles\Build.bat FactoryEditor Win64 Development -project=<path>\FactoryGame.uproject` (wrap this in `tools/build.ps1`). The editor target is `FactoryEditor` (not `FactoryGameEditor`).
3. For gameplay testing: **Alpakit Dev** → launch the game with mods.
4. For multiplayer/server testing: Alpakit the `LinuxServer` target → copy to the Docker volume → restart the container → connect with the game client.
5. Blueprint/UMG work happens inside the streamed editor session.

Fast checks that don't need the game: keep provider adapters, chunk-assembly logic, aggregation math, and config parsing engine-independent where possible so they're exercised by automation (see §5) rather than manual play.

### Two-client testing

Late-joiner sync, streaming fan-out, and proposal approval need two real clients. Options: a second machine, a second account (Steam + Epic copies count as two), or Steam Family Sharing on an alt. One client + dedicated server covers most day-to-day testing; do a two-client pass before any release and for every change to the chunk protocol or ProposalUI.

---

## 4. Repository Layout & Conventions

```
Mods/AIDA/
  Source/AIDA/            # C++ module
    Adapters/             # LLMClient, OpenAICompatAdapter, AnthropicAdapter
    Core/                 # Orchestrator, SessionManager, Permission, RateLimiter
    Index/                # FactoryIndex, MapService (ONLY place game headers appear
                          #   besides Actions/)
    Actions/              # ActionEngine (Phase 4+)
    Data/                 # Persistence, NotesStore, SnapshotService
    Net/                  # RPC surfaces, chunk protocol structs
  Content/                # Blueprints, UMG widgets, icons
  Configs/                # default config, prompts/system.md, tools.json
docs/                     # architecture.md, this file, ops guide
tools/                    # build.ps1, devserver/ (docker-compose), scripts
```

### Coding rules

1. **Authority discipline.** World mutation and LLM/network egress happen only on the server. Any function that must not run on a client asserts `HasAuthority()`. UI code never appears in server-path modules.
2. **Headless safety.** No code reachable on a dedicated server may touch local players, viewports, or widgets. `GetFirstLocalPlayerController()` and friends are banned outside `Content/` and client-only classes. The Linux server build in CI is the enforcement backstop, but review for it too.
3. **Game-API isolation.** Only `Index/` and `Actions/` may include FactoryGame headers. Everything else consumes their normalized types. This is the patch-resilience seam — treat violations as build breaks in review.
4. **Secrets.** The API key exists only in server config and `LLMClient` memory. It must never appear in replicated structs, logs, client-reachable code paths, crash dumps you can avoid, or test fixtures. Redact it in all logging.
5. **No blocking on the game thread.** All HTTP is async via `FHttpModule` callbacks. Long server work (extraction on huge saves, batch execution) is time-sliced.
6. **Unreal style.** Follow Epic's coding standard (PascalCase, `F`/`U`/`A`/`I` prefixes, `TArray`/`FString`, UPROPERTY for anything the GC must see). No STL containers in engine-facing code.
7. **Prompts and tool schemas are data.** They live in `Configs/AIDA/`, are versioned in git, and load at runtime. Never hardcode prompt text in C++. Changes to `tools.json` schemas require a version bump in the file header and a changelog entry.
8. **Bounded everything.** Tool results, transcript context, chunk sizes, tool round-trips, and journal windows all have explicit configurable caps with sane defaults. New tools must state their output bound in review.

### Git conventions

- `main` is always releasable; feature branches + PRs, squash-merge.
- Conventional-commit style prefixes (`feat:`, `fix:`, `docs:`, `net:`, `index:`).
- Git LFS for all binary assets (`.uasset`, `.umap`, images). Never commit `Saved/`, `Intermediate/`, `Binaries/`, or any file containing an API key (`Configs/**/secrets*` is gitignored; ship `config.example.jsonc`).
- Tag releases `vMAJOR.MINOR.PATCH` matching the `.uplugin` version.

---

## 5. CI (GitHub Actions)

Every PR must produce: a Win64 (client) build, a LinuxServer build, and Alpakit packages for both. Additionally:

- Engine-independent unit tests (adapter wire-format translation incl. Anthropic mapping, SSE parsing, chunk reassembly incl. gap/hash-mismatch cases, aggregation math on fixture data, config parsing/validation).
- A lint step that greps for banned patterns (`GetFirstLocalPlayerController` outside allowed paths, FactoryGame includes outside `Index/`/`Actions/`, `apiKey` in any replicated struct).
- Release workflow: on tag, build multi-target Alpakit package and attach artifacts; upload to ficsit.app is a manual, human step.

The custom engine on CI runners is the hard part; the modding community maintains actions/images for this — use theirs rather than rolling our own, and cache the engine aggressively.

---

## 6. Testing Checklist per Change Type

| Change touches… | Minimum verification |
|---|---|
| Chunk protocol / SessionManager | Unit tests + two-client manual pass with a late joiner |
| FactoryIndex / aggregation | Fixture-save unit tests + manual run on a large (40 h+) save |
| Adapters / LLMClient | Wire-format unit tests + live round-trip against one real endpoint and Ollama |
| Permissions / RateLimiter | Denied-path tests; verify denials are logged with player IDs |
| ActionEngine | Dry-run failure cases, approve-by-ID only, undo on a modified world, two-client approval flow |
| Config | Parse/validation tests incl. malformed admin overrides; server boots to a clear error, never a crash |
| Anything server-reachable | LinuxServer build passes; smoke test in the Docker dedicated server |

---

## 7. Release Procedure

1. Verify the toolchain/game version pin; two-client pass on the release candidate; dedicated-server smoke test from a clean Docker volume.
2. Update `.uplugin` version, changelog, and compatibility notes (game CL range, SML version).
3. Tag; CI produces the multi-target package; upload to ficsit.app (mod reference `AIDA`).
4. Post-release: watch the modding Discord and GitHub issues; after any game patch, run the FactoryIndex self-tests before declaring compatibility.

---

## 8. First-Week Order of Operations (new contributor / fresh machine)

1. SSH + Tailscale + Moonlight working Mac→Windows (if using the remote workflow).
2. SML Getting Started guide top-to-bottom: VS 2022 → engine fork (start the download early; it's large) → Wwise → starter project builds → editor opens.
3. Clone AIDA into `Mods/`, build, Alpakit Dev, see the mod load in-game.
4. Docker dedicated server up via `tools/devserver/`; confirm the LinuxServer build of AIDA loads on it.
5. Copy `config.example.jsonc` → configure a provider (Ollama is fine) → run the Phase 0 round-trip test.

If any step fights you for more than an hour, ask in the Satisfactory Modding Discord — toolchain issues are almost always known, version-specific, and already answered.
