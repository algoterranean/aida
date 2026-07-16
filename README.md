# [mod] AIDA: AI Design Assistant

This is a proof of concept for integrating AI into games, with Satisfactory as the test victim. It is single and multiplayer compatible. 

You'll need an API key from Anthropic, OpenAI, etc., but otherwise it works like any other mod. Set the key in `Satisfactory\FactoryGame\Configs\AIDA\config.jsonc` and use `Ctrl-Enter` to open chat. **Do NOT put this on a public server without API spending caps!**

This is an early POC/WIP and likely has bugs--feedback is more than welcome. 

Examples of things that you can chat with the AI about and ask it to do:
- Where are my most significant bottlenecks?
- What has changed in the past few hours? What about since I last logged in? 
- Upgrade all nearly belts to MK5.
- Where is the nearest unused pure iron node? 
- Design a new building for making heavy modular frames. It needs to output 10/min. Make it tall instead of wide and with as many floors as necessary. Then build it.
- Where am I wasting energy? Where should I underclock?
- If I need to produce 1GW of coal power, how many water pumps and generators do I need? What are the miner and node requirements? 
- See the attached picture. Build it.
- Plop down a manifold of assemblers making screws and route them to the nearest train depot. 
- Undo what was just built.
- Do I have any splitters or machines that are not connected due to a misplaced belt? Where are they? And do I have any slow belts mixed in with fast belts?
- Label all of these containers with their contents. Check every ten minutes to make sure they're still consistent with their contents.
- Build me a hypertube from here that will launch me so that I land precisely in the middle of my other base in the desert (near the river, not the coastal base).

## Installation

Just ask Claude to do it for you. :) 


## Capabilities

For the technically curious, the following tool calls are implemented and exposed to the LLM:

### Factory inspection (read-only)

- `get_factory_overview` — summarize the whole factory: machine clusters (id, size, location, main output), the biggest item deficits, and power per circuit.
- `get_item_balance` — net production vs consumption per item across the factory, deficits first; optionally focused on one item.
- `inspect_cluster` — drill into one machine cluster: building census, net inputs/outputs, efficiency, and machine ids.
- `find_bottleneck` — diagnose why an item's production is limited: starved upstream, throttled by overloaded power, or output backing up.
- `find_disconnected` — find logistics breaks: splitters/mergers with an unconnected side, belts/pipes attached to nothing, and machines with open ports.
- `find_belt_mismatch` — find slow links: belts/pipes slower than their neighbors or the machine feeding them, biggest choke first.
- `get_clock_advice` — find machines worth underclocking, with per-machine current vs suggested clock and the MW saved.
- `get_container_contents` — list storage containers and what they hold, optionally filtered by item or radius around the player.
- `get_resource_nodes` — resource nodes on the map grouped by resource and purity, optionally only untapped ones.

### Reference & planning (static game data)

- `lookup_recipe` — static recipe reference: inputs/outputs with per-minute rates, craft time, and the building that makes it.
- `lookup_building` — static building reference: power draw (and, for generators, power output).
- `plan_factory` — deterministic production planner: per-step machine counts, exact clocks, belt/pipe mark per edge, power, and raw-resource needs for a target rate.

### Map markers

- `tag_node` — place a labeled map marker on the nearest untapped resource node of a given resource/purity.
- `mark_location` — place a labeled map marker at any coordinates, plus a 3D attention ping visible through walls.

### Memory & history

- `add_note` — save a persistent note (survives across sessions), tagged with the player's location and region.
- `get_notes` — recall saved notes, filtered by keyword or region, or sorted by distance to the player.
- `take_snapshot` — capture a timestamped snapshot of production balance + power (also taken automatically every 30 minutes).
- `compare_to` — diff the factory now against an earlier snapshot: per-item production/consumption changes and power delta.

### Actions (proposal-gated — nothing executes until a player approves)

- `propose_build` — propose placing buildables: a single grid (v1) or a multi-part composite structure (v2, up to 32 parts, e.g. reference-image reconstructions). Returns a dry-run report (count, cost, validity); powered machines are auto-wired with poles and lines.
- `propose_dismantle` — propose removing buildables near a point, with matched count and refund tally.
- `propose_manifold` — propose the belt/pipe plumbing for a row of machines: one splitter/merger per machine on a trunk line plus all connecting runs.
- `propose_power` — propose wiring existing machines to electricity: poles, power lines, a pole chain, and a tie-in to the nearest powered grid.
- `propose_label_containers` — propose labeling storage containers with one small sign each, set to the container's dominant item.
- `get_proposal_status` — status of build/dismantle proposals (pending, approved, executed, rejected, expired).

### Diagnostics

- `echo` — trivial echo tool used to verify the model→tool→model round-trip.

The action tools can be disabled entirely via the `actions.enabled` kill-switch, in which case the model never sees the `propose_*` tools.
