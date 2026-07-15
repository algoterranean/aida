# [mod] AIDA: AI Design Assistant

This is a proof of concept for integrating AI into games, with Satisfactory as the test victim. It is single and multiplayer compatible. 

You'll need an API key from Anthropic, OpenAI, etc., but otherwise it works like any other mod. Set the key in `config` and use `Ctrl-Enter` to open chat. **Do NOT put this on a public server without API spending caps!**

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


## Usage



## Capabilities

For the technically curious, the following tool calls are implemented and exposed to the LLM:
- 
