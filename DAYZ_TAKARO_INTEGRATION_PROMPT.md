# DayZ ↔ Takaro Integration — Implementation Prompt

You are picking up a project to integrate a modded **DayZ** server with **Takaro** (a multi-game server management platform). The DayZ server is already running. The job is to design and build the connector that lets Takaro send commands to the server and receive events from it.

This prompt is intentionally exhaustive so a fresh session has full context and doesn't have to re-discover anything.

---

## 1. Server snapshot

- **Server hostname:** `Double Tap Bloodlines`
- **Public IP:port:** `46.110.80.49:2302` (game), `46.110.80.49:27016` (Steam query)
- **LAN IP:** `192.168.1.27:2302`
- **Server box:** Windows machine reachable as `\\SERVER`. Files live at `\\SERVER\GameServers\dayz\`.
- **Map:** Sakhal (`dayzOffline.sakhal` mission)
- **Server config:** `\\SERVER\GameServers\dayz\serverDZ.cfg`
- **Profiles dir** (where mods write logs / configs / persistence): `\\SERVER\GameServers\dayz\profiles\`
- **BattlEye:** enabled, RCON config at `\\SERVER\GameServers\dayz\battleye\beserver_x64.cfg`
  - `RConPassword madRcon`
  - `RConPort 2306`
  - `RestrictRCon 0`

### Loaded mods (8 total — load order matters):

| # | Folder | Workshop ID | Purpose |
|---|---|---|---|
| 1 | `@CF` | `1559212036` | Community Framework — base modding API & event hooks. Required by everything below. |
| 2 | `@DabsFramework` | `2545327648` | Secondary framework, dependency of Expansion. |
| 3 | `@DayZ-Expansion-Licensed` | `2116157322` | Bohemia-licensed BIS assets used by the bundle. |
| 4 | `@DayZ-Expansion-Bundle` | `2572331007` | All Expansion modules monolithic — vehicles, navigation, groups, chat, book, spawn-selection, market (unused), basebuilding (unused), quests, hardline, AI, etc. |
| 5 | `@BaseBuildingPlus` | `1710977250` | Active base-building system (chosen over Expansion's). |
| 6 | `@BBP_ExpansionCompat` | `2448237424` | BBP ↔ Expansion compatibility shim. |
| 7 | `@TraderPlus` | `2458896948` | Active economy / NPC traders (chosen over Expansion's market). |
| 8 | `@VPPAdminTools` | `1828439124` | Primary admin tool. F1 in-game opens the GUI; password `madAdministrator`; super-admins listed in `profiles\VPPAdminTools\Permissions\SuperAdmins\SuperAdmins.txt`. |

---

## 2. Per-mod admin / integration hooks

The integration layer should target whichever mod best exposes the data Takaro needs. Quick map of what each mod offers:

### CF (Community Framework) — `1559212036`
- The lowest-level hook layer. CF exposes a typed event system that other mods (and Takaro adapters) subscribe to.
- Useful events: player connect/disconnect, chat, action triggers, killed, item use.
- Source / docs: https://github.com/Jacob-Mango/CommunityFramework
- Most Takaro-style integrations should be a **CF module** so they get clean event subscriptions instead of polling logs.

### Dabs Framework — `2545327648`
- Adds RPC, UI, and storage helpers used by Expansion. Less integration surface area than CF, but useful if the connector mod wants to ride on existing Dabs RPC.
- Source: https://github.com/SerttBlackmore/DabsFramework

### DayZ-Expansion-Bundle — `2572331007`
- Big surface. Contains its own event hooks and its own admin-style commands (`/expansion …` chat commands), AI controller, mission system, map markers, parties.
- Most useful for Takaro: its **chat module** (player → server messages) and **groups module** (party membership).
- Wiki / scripts source: https://github.com/salutesh/DayZ-Expansion-Scripts

### TraderPlus — `2458896948`
- Has logging at `\\SERVER\GameServers\dayz\profiles\TraderPlus\TraderPlusLogs\` — every transaction is logged.
- Can be hooked via CF events for buy/sell.
- Profile config: `profiles\TraderPlus\TraderPlusConfig\` (currencies, traders, prices).
- Source: https://github.com/TheDmitri/TraderPlusV1

### BaseBuildingPlus — `1710977250`
- Has its own event hooks for "wall placed", "raid started", "code lock attempted".
- Profile data writes to `profiles\BaseBuildingPlus\` (currently empty until a player builds).
- Source: https://github.com/FelixForesight2020/BaseBuildingPlus

### VPPAdminTools — `1828439124` (PRIMARY ADMIN HOOK)
- Where the existing operational surface is. Look here first for both events out of the server and commands into the server.
- Persistent files of interest under `profiles\VPPAdminTools\`:
  - `Permissions\credentials.txt` — admin password (hashed after first boot)
  - `Permissions\SuperAdmins\SuperAdmins.txt` — Steam64 IDs with full perms
  - `Permissions\UserGroups\UserGroups.json` — role/group definitions
  - `Logging\Log_*.txt` — admin action audit log (kicks, bans, spawns, teleports — every admin action)
  - `BanList.json` — current bans
  - `LogOptions.json` — toggles for what gets logged
  - `ConfigurablePlugins\WebHooksManager\` — **webhook outbound config**, this is the cleanest event-out path
  - `ConfigurablePlugins\SteamAPI.json` — Steam Web API key slot for VPP's player-info lookups
  - `ConfigurablePlugins\TimeManager\TimeSettingsPresets.json` — admin time presets
  - `ConfigurablePlugins\WeatherManager\WeatherSettingsPresets.json` — admin weather presets
  - `ConfigurablePlugins\TeleportManager\TeleportLocation.json` — saved teleport spots
  - `ConfigurablePlugins\BuildingSetManager\` — saved building presets (admin-spawned bases)
  - `ConfigurablePlugins\ItemManager\SavedItemPresets.vpp` — saved loadout presets
- Source repo: https://github.com/Arkensor/DayZCommunityOfflineMode (related) / https://github.com/banditrobotics/VPPAdminTools

The **WebHooksManager** is the single most important folder for Takaro outbound events — VPP can fire HTTP callbacks on configurable triggers, which is exactly what Takaro's adapter wants on the receive side.

---

## 3. Takaro context

### Takaro source code (this machine)
- `/home/zmedh/Takaro-Projects/takaro-github` — full Takaro monorepo. Look at:
  - `packages/` — service packages
  - `packages/lib-modules/` — built-in modules
  - `containers/` — Docker setup
  - `docs/` — architecture docs
  - `CLAUDE.md` — project-specific instructions

### Reference integrations on this machine (study these first — they show the working patterns)

- **`/home/zmedh/Takaro-Projects/Hytale/Hytale-Takaro-Integration/`** — Java-side Hytale plugin that talks to Takaro. Includes:
  - `HytaleTakaroMod/` — the actual Hytale server-side plugin (Java, Maven)
  - `0_Modding_Github/` — decompiled Hytale source for reference
  - Pattern: server-side mod sends/receives via Takaro's HTTP gateway.
- **`/home/zmedh/Takaro-Projects/Palworld-Takaro-Integration/`** — Palworld pattern. Less direct integration; relies on:
- **`/home/zmedh/Takaro-Projects/Palworld-Bridge/`** — Node.js bridge that translates between Palworld's RCON and Takaro's HTTP/WebSocket events. **THIS IS THE PATTERN TO FOLLOW FOR DAYZ**, because DayZ also exposes BattlEye RCON and doesn't have a clean native HTTP gateway.

The Palworld-Bridge already has the boilerplate (auth, reconnect, event polling, command dispatch). The DayZ adapter is essentially "swap RCON protocol from Palworld's REST to BattlEye's binary RCON, swap event sources, ship it."

### How Takaro adapters work (per Takaro repo conventions)
- Each game has a `gameserver` adapter implementing a contract (connect, sendCommand, listenForEvents, etc.).
- Inbound events Takaro expects: `player-connected`, `player-disconnected`, `chat-message`, `player-killed`, `entity-killed`, `log-line`, plus ad-hoc.
- Outbound commands Takaro sends: `kick`, `ban`, `teleport`, `give-item`, `execute-command`, `list-players`, `get-player-info`.
- Auth: Takaro uses an API token; the bridge stores it and signs requests.

---

## 4. Architecture options for DayZ ↔ Takaro

Three viable paths — **option B is recommended**:

### A. Pure CF mod ("Takaro_DayZ_Mod")
A new DayZ mod (using CF and Dabs Framework) running on the server that:
- Hooks every CF event we care about
- Calls Takaro's HTTP API directly with a stored API token
- Receives commands from Takaro via long-polling or websockets

**Pros:** lowest latency, cleanest data (real game-state events, not log scraping). Pros for shipping as a community mod.

**Cons:** lots of Enforce Script work; HTTP from Enforce Script is doable but limited (DayZ has a `RestApi` / `RestContext` API but it's basic). Hard to maintain across DayZ updates.

### B. External bridge (Node.js or similar) — **RECOMMENDED**
A bridge process running on the server box (or anywhere reachable) that:
1. Connects to BattlEye RCON (port 2306, password `madRcon`) for **outbound commands** (kick / ban / say / loadFile / players list / etc.)
2. **Tails the server's RPT log file** (`profiles\DayZServer_x64_*.RPT`) for inbound events: connect/disconnect, chat, kills (most events show up there).
3. Subscribes to **VPPAdminTools webhooks** for high-fidelity admin events (configured in `profiles\VPPAdminTools\ConfigurablePlugins\WebHooksManager\`).
4. Relays to/from Takaro's HTTP gateway using its API token.

This is exactly what Palworld-Bridge does (with REST-RCON + log tail). DayZ's BE RCON is a different protocol but well-documented and there are existing JS libraries (`battle_eye_rcon`, `node-battleye`).

**Pros:** language flexibility, fast iteration, doesn't need DayZ Tools, easy to test. Same architecture the user already runs for Palworld.

**Cons:** RPT log tailing is fragile (line formats change between DayZ versions); webhooks limited to what VPP's WebHooksManager fires.

### C. Hybrid — small CF mod + external bridge
A minimal CF mod that fires structured events to an HTTP endpoint on the same box, plus the bridge from option B. Mod handles in-game-only events (e.g. `player-builds-wall`, `player-buys-item`) that don't show up in RPT or VPP. Bridge handles RCON commands.

**Pros:** best fidelity. **Cons:** twice the maintenance.

**Start with B. Promote to C only if data fidelity is insufficient.**

---

## 5. Specific work items for the next session

**The architecture choice in section 4 is provisional. Don't lock it in until step 1 below is done — the mod sources may reveal cleaner integration points than RCON + log tailing, in which case option A or C beats option B.**

### Phase 1 — Inventory what the mods *actually* expose (do this FIRST)

Before writing any bridge code, read each mod's source/PBO and catalogue every event, hook, RPC, command, and webhook trigger they publish. The bridge should consume the highest-fidelity surface available, not scrape logs when a typed event hook exists.

1. **Unpack the PBOs.** DayZ Tools (free on Steam) ships with PBO Manager / FileBank. Or use a third-party PBO extractor on Windows. Required input files:
   - `\\SERVER\GameServers\dayz\@CF\addons\*.pbo`
   - `\\SERVER\GameServers\dayz\@DabsFramework\addons\*.pbo`
   - `\\SERVER\GameServers\dayz\@DayZ-Expansion-Bundle\addons\*.pbo` (lots — focus on `0_*_preload.pbo`, `*_scripts.pbo`)
   - `\\SERVER\GameServers\dayz\@BaseBuildingPlus\addons\BaseBuildingPlus.pbo`
   - `\\SERVER\GameServers\dayz\@TraderPlus\addons\*.pbo`
   - `\\SERVER\GameServers\dayz\@VPPAdminTools\Addons\*.pbo`
   Extract them to a local working dir.

2. **For each mod, produce a catalogue of:**
   - **Event hooks** (CF event types, Dabs RPC channels, native script callbacks like `OnPlayerConnected`)
   - **Server-side methods** the mod exposes that take simple args (these are RPC-callable from a bridge mod)
   - **Persistent state files** the mod writes (so the bridge can tail them as a fallback signal source)
   - **Outbound webhooks** (VPP WebHooksManager — what triggers, what payload schema, what HTTP method)
   - **Chat / RCON commands** the mod registers (what `/expansion …` commands exist, what TraderPlus admin commands exist, etc.)
   - **Logging output format** in the RPT (so the bridge can fall back to log parsing for events the mod doesn't directly emit)

3. **Cross-reference with Takaro's expected event/command schema** — pulled from `/home/zmedh/Takaro-Projects/takaro-github/packages/lib-modules/` and any `gameserver` adapter packages. Build a mapping table:
   ```
   Takaro event           ←  Best source                     ←  Fallback
   player-connected       ←  CF OnClientConnect event        ←  RPT [Login] line
   player-disconnected    ←  CF OnClientDisconnect event     ←  RPT [Disconnect] line
   chat-message           ←  Expansion chat hook             ←  RPT chat line / VPP webhook
   player-killed          ←  CF / VPP killfeed webhook       ←  RPT EntityKilled
   item-purchased         ←  TraderPlus transaction event    ←  TraderPlusLogs/*.log
   wall-placed            ←  BBP build event                 ←  RPT EntityCreated
   territory-flag-placed  ←  Expansion territory hook        ←  Expansion log
   admin-action           ←  VPP webhook (kick/ban/spawn)    ←  VPPAdminTools/Logging/Log_*.txt
   ```

4. **Read & summarize** `/home/zmedh/Takaro-Projects/Palworld-Bridge/` — extract the contract/abstractions: how does it auth to Takaro, how does it dispatch commands, how does it queue/retry events? Same shape gets reused for DayZ regardless of which integration mode you pick.

### Phase 2 — Pick architecture based on Phase 1 findings

After the catalogue exists, evaluate:
- If most needed events have direct CF / mod-API hooks → **prefer option A or C** (a CF-based bridge mod) so events flow native instead of being scraped from logs.
- If most needed events only show up in RPT logs and VPP webhooks → **option B (external bridge)** is enough.
- Mixed → option C.

Write that decision down with the catalogue as the supporting evidence before writing any bridge code.

### Phase 3 — Implementation

5. **Pick a BE RCON library** — `battle_eye_rcon` (npm) or `node-battleye` are the two viable choices. Test against the local server (`192.168.1.27:2306` password `madRcon`).

6. **Write the bridge skeleton** — modeled on Palworld-Bridge, with stubs for: `connectRcon`, `tailRpt`, `subscribeVppWebhooks` (plus `subscribeCFEvents` if option A/C), `dispatchToTakaro`, `receiveFromTakaro`. Drop into `/home/zmedh/Takaro-Projects/DayZ-Takaro-Integration/`.

7. **If option A or C — write the in-game CF mod** that emits structured events to the bridge over HTTP / RPC. Use CF's `ScriptCaller` / Dabs RPC for RPC-style.

8. **Write a list of test scenarios** — e.g. "player joins → Takaro sees player-connected within 5s", "Takaro sends `say Hello` → all players see it", "trader purchase fires Takaro event with item/price/buyer", etc.

---

## 6. Reference info / where to look things up

- **DayZ scripting docs** (community wiki): https://community.bistudio.com/wiki/DayZ:Modding
- **DayZ Enforce Script docs**: https://community.bistudio.com/wiki/Enforce_Script_Syntax
- **CF wiki**: https://github.com/Jacob-Mango/CommunityFramework/wiki
- **BattlEye RCON protocol**: https://www.battleye.com/downloads/BERConProtocol.txt
- **VPPAdminTools webhook docs** (sparse — may need to read the PBO source): unzip `\\SERVER\GameServers\dayz\@VPPAdminTools\Addons\VPPAdminTools.pbo` with a PBO tool to read the WebHooks scripts.
- **Decompiled DayZ tools**: user has DayZ Tools installed via Steam.
- **Existing decompiled game code** for context: `/home/zmedh/Takaro-Projects/Hytale/Hytale-Takaro-Integration/0_Modding_Github/Hytale-Toolkit/decompiled/` (Hytale, not DayZ — but illustrates the decompilation workflow if you need to do the same for DayZ pbos).

---

## 7. Out-of-scope (do NOT do these in this pass)

- Don't write any new in-game UI for Takaro. The bridge is server-side only.
- Don't replace VPPAdminTools with a Takaro-native admin GUI. VPP stays.
- Don't touch the server's existing `serverDZ.cfg`, mod chain, or BattlEye config except read-only.
- Don't change how players join the server. The bats and the Workshop collection are already finalized.

---

## 8. Acceptance criteria for "done"

The integration is done when, with the bridge running:
1. Takaro shows the DayZ server as **online** in its dashboard.
2. Takaro lists **currently connected players** with steam64 IDs and player names.
3. Takaro can **send a chat message** that appears in-game ("global say").
4. Takaro can **kick / ban a player** and the action is reflected in `VPPAdminTools/BanList.json` and the RPT log.
5. Takaro receives **player-connected / player-disconnected / chat-message** events with no more than ~5s latency.
6. Optional but ideal: TraderPlus transactions and BBP wall-placements show up as Takaro events.

---

## 9. Quick start for the next session

```
cd /home/zmedh/Takaro-Projects/Palworld-Bridge
# Read README, package.json, src/, docs/ — understand the pattern.
# Same goes for /home/zmedh/Takaro-Projects/Hytale/Hytale-Takaro-Integration

mkdir -p /home/zmedh/Takaro-Projects/DayZ-Takaro-Integration
# Scaffold a Node.js project mirroring Palworld-Bridge's structure.
# Add BattlEye RCON deps, RPT tail logic, VPP webhook listener, Takaro client.
# Wire test connection to 192.168.1.27 (LAN) before testing public IP.
```

---

End of prompt. Treat sections 1–3 as fixed context; 4–9 are the working brief.
