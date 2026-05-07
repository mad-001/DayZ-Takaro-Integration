# DayZ ↔ Takaro Integration — Handoff

## Architecture (one paragraph)
DayZ server runs a native DLL (`dayz_takaro.dll`) injected via `winmm.dll` proxy hijack — `DayZServer_x64.exe` imports WINMM, so a fake `winmm.dll` in the server folder loads first and `LoadLibrary`s our DLL. The DLL owns the WSS connection to `wss://connect.takaro.io/` (WinHTTP WebSocket), exposes a tiny HTTP server on `127.0.0.1:8089`, and tails the server RPT to forward log lines as `gameEvent type=log`. The companion Enforce Script mod (`@TakaroIntegration`) hooks `MissionServer` (connect/disconnect/chat), runs a command dispatcher for Takaro operations (kick/ban/teleport/give/etc.), and uses `RestApi`/`RestContext` to talk to the DLL on `127.0.0.1:8089`.

## What works
- DLL injection via winmm proxy on local + production.
- WS identifies with Takaro, stays connected, reconnects on drop.
- Local HTTP bridge: script POSTs events to `/gameserver/<id>/events`, polls operations from `/gameserver/<id>/poll`, posts results back.
- Operation handlers wired in `TakaroCommandDispatcher.c`: `say`, `help`, `shutdown`, `kick`, `ban`/`addban`, `unban`/`removeban`, `tp`/`teleport`, `give`/`giveitem`, `players`/`listplayers`, `listItems`, `listBans`, `listEntities`, `getPlayerLocation`, `getPlayerInventory`, `executeConsoleCommand`.
- `CommandOutput` shape is correct everywhere (`{rawResult: string, success: bool}`).
- Case-insensitive verb dispatch (`Help` and `help` both work).
- RPT log forwarding with deny-list (filters our own `[Takaro][DBG]` lines, BE recv-error spam, common engine warnings).
- Per-action shape coercion in `ForwardResponse` (unwraps `{players:[...]}` → bare array; coerces 0/1 → false/true for connectable/online).
- BE RCON works on **production**.

## What's open / unverified
1. **Per-type event JSON** — just rewrote `TakaroEventQueue.c` and `TakaroBridge.FlushEvents` so events build their own JSON with only relevant fields (kills the empty `channel:""` that was tripping Takaro's `IChatChannel` whitelist on `player-connected`/`player-disconnected`). **Not yet verified on a running local server.** PBO sha1 `0ca51edc1ba8daab6c98baaa25558ebd84e4e36e` deployed to local only.
2. **Chat-message flow** — code path in `MissionServerTakaro.c::OnEvent` and `OnChatMessage` exists; previous validation failures may have masked it. Verify after the event-JSON fix lands.
3. **Production deploy** — pending explicit user approval after local verification.

## Known environmental
- Local DayZ test server's BE silently fails to load while the user's DayZ client BE is installed on the same Windows box. BE RCON only works on production. (See `dayz_be_local_conflict.md` memory.)
- DayZ does **not** write `[BattlEye]` lines to RPT — RPT-absence is not diagnostic for BE.

## Repo layout
- `DayZ-DLL/dayz_takaro.cpp` — native bridge (WS + HTTP server + RPT tail + BE RCON)
- `DayZ-DLL/winmm_proxy.cpp` — auto-generated proxy DLL (181 export forwards + DllMain LoadLibrary)
- `src/takaro_integration/scripts/3_Game/TakaroConfig.c` — config loader
- `src/takaro_integration/scripts/4_World/TakaroBridge.c` — script-side controller, owns queue + http client + dispatcher
- `src/takaro_integration/scripts/4_World/TakaroCommandDispatcher.c` — every operation verb
- `src/takaro_integration/scripts/4_World/TakaroEventQueue.c` — **just rewritten**: queue holds pre-built JSON strings, `TakaroEventFactory` builds per-type
- `src/takaro_integration/scripts/4_World/TakaroHttpClient.c` — RestApi wrapper for talking to the local DLL HTTP server
- `src/takaro_integration/scripts/4_World/PlayerBaseTakaro.c` — modded PlayerBase for death hook
- `src/takaro_integration/scripts/5_Mission/MissionServerTakaro.c` — connect/disconnect + chat OnEvent hooks
- `src/takaro_integration/scripts/5_Mission/PluginManagerTakaro.c` — registration

## Paths
- **Local test server**: `C:\Program Files (x86)\Steam\steamapps\common\DayZServer\` — user starts via **Steam Play button** on the DayZ Server entry. **Never auto-launch.**
- **Production**: `\\SERVER\GameServers\dayz\` — **never touch without explicit approval.**
- **Mod folder on each**: `<server>\@TakaroIntegration\Addons\TakaroIntegration.pbo`
- **DLL on each**: `<server>\dayz_takaro.dll` + `<server>\winmm.dll` (proxy)
- **Config on each**: `<server>\profiles\TakaroIntegration\config.json`
- **Logs**:
  - Server RPT: `<server>\profiles\DayZServer_x64_<ts>.RPT`
  - DLL log: `<server>\logs\TakaroLogs\dayz-takaro-*.log`
  - Script log: `<server>\profiles\script_*.log`

## Build & deploy
```bash
# 1. Pack PBO (in WSL):
cd /home/zmedh/Takaro-Projects/DayZ
python3 scripts/pack_pbo.py src/takaro_integration @TakaroIntegration/Addons/TakaroIntegration.pbo --prefix TakaroIntegration

# 2. Deploy to LOCAL test server only:
powershell.exe -Command "Copy-Item -Path '\\\\wsl.localhost\\Ubuntu\\home\\zmedh\\Takaro-Projects\\DayZ\\@TakaroIntegration\\Addons\\TakaroIntegration.pbo' -Destination 'C:\\Program Files (x86)\\Steam\\steamapps\\common\\DayZServer\\@TakaroIntegration\\Addons\\TakaroIntegration.pbo' -Force"

# 3. (Production — only when authorized):
# powershell.exe -Command "Copy-Item -Path '\\\\wsl.localhost\\Ubuntu\\home\\zmedh\\Takaro-Projects\\DayZ\\@TakaroIntegration\\Addons\\TakaroIntegration.pbo' -Destination '\\\\SERVER\\GameServers\\dayz\\@TakaroIntegration\\Addons\\TakaroIntegration.pbo' -Force"
```

DLL build: `cd DayZ-DLL && build.bat` (MSVC) → produces `dayz_takaro.dll` + `winmm.dll`. Same deploy pattern.

## Takaro registration
- Token (reused local + prod): `CbbLktIhRG6QIPiNNUYvtlZ4OdwOoUhtp9552lbrfMw=`
- Production server name in dashboard: `DayZ`
- Local server name in dashboard: `DayZ-Local-Test`

## Hard rules (from user)
- **Never start, stop, restart production server processes.** User does it.
- **Never auto-start the local test server.** User starts via Steam.
- **Don't add logging to troubleshoot** — read the code and fix the bug.
- **Don't tell the user to restart** — assume they did.
- **Don't touch production without explicit per-task approval.**
- **Never use `functionCreate` in Takaro** — inline code in cronjob/hook/command `function`.

## Enforce Script gotchas (already burned)
- `out` is a reserved keyword. Use `out_s` / `s` / `drained`.
- CParser breaks on `"\\"` literals — avoid backslash escapes; `"\""` for double-quote works fine. Build literal characters via `string q = "\""`.
- "Formula too complex" on long concat — split into multiple `+=` statements or extract helper.
- `JsonSerializer.WriteToString` writes **every** class field, including empty strings — that's why the event payload now builds JSON manually per type.
- `JsonSerializer` serializes bool as `0`/`1`, not `true`/`false` — the DLL coerces.
- PBO must have `product='dayz ugc'` + `PboType='Arma Addon'` properties, else DayZ silently skips it.
- `PlayerBase` modded class must live in `4_World` (where `PlayerBase` is declared), not `5_Mission`.
- DayZDiag-only: `callExtension`. Production DayZ has no DLL bridge primitive — that's why we run a local HTTP server in the DLL and call it via `RestApi`/`RestContext`.

## First steps for the next conversation
1. User starts the local test server via Steam.
2. Have user join, leave, send a chat message.
3. Read `<local server>\logs\TakaroLogs\dayz-takaro-*.log` — confirm `forwarded gameEvent player-connected/disconnected/chat-message` lines.
4. Check Takaro dashboard / events stream for the same events with no validation errors.
5. If clean, ask user before deploying to production. If `whitelistValidation` errors persist, the per-type JSON build is wrong — read `TakaroEventQueue.c::Connected/Disconnected/Chat` and check what fields ship.
