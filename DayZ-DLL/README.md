# DayZ Takaro Native DLL

A native C++ DLL injected into `DayZServer_x64.exe` that connects directly to `wss://connect.takaro.io/`. No sidecar, no Enforce Script mod, no Workshop.

## Pattern

DayZ runs on Bohemia's Enfusion engine. Enfusion's scripting language (Enforce Script) has no WebSocket primitive — only one-shot HTTP via `RestApi`/`RestContext`. To talk to Takaro's `wss://` endpoint from inside the server process, we inject a native C++ DLL that uses Win32's WinHTTP (which has full WebSocket support: `WinHttpWebSocketCompleteUpgrade`, send/receive frames, persistent connection).

Injection mechanism: **winmm.dll proxy hijack**. `DayZServer_x64.exe` imports `WINMM.dll`. We ship a fake `winmm.dll` next to the EXE that:
1. Forwards every winmm export back to `C:\Windows\System32\winmm.dll` (so the game's actual winmm calls keep working).
2. In `DllMain.PROCESS_ATTACH`, calls `LoadLibraryA("dayz_takaro.dll")` which sits in the same folder.
3. `dayz_takaro.dll`'s `DllMain` then spins up two threads — one for the WebSocket and one tailing the RPT log — and is fully running before `DayZServer_x64.exe` reaches its main loop.

## Files

| File | Purpose |
| --- | --- |
| `dayz_takaro.cpp` | The integration. WinHTTP WebSocket → Takaro, RPT-log tail → player events, Takaro request handlers. |
| `winmm_proxy.cpp` | Auto-generated proxy. 181 `#pragma comment(linker, "/export:...")` lines forwarding every winmm function back to System32 + a `DllMain` that loads `dayz_takaro.dll`. |
| `build.bat` | Wraps `cl.exe` with `vcvars64.bat`. Outputs `dayz_takaro.dll` and `winmm.dll`. |

## Build

```bat
:: From a Visual Studio 2022 install. The .bat sources vcvars64.bat itself.
build.bat
```

Output: `dayz_takaro.dll` and `winmm.dll` next to the source files.

## Deploy

Copy both DLLs into the DayZ server folder (next to `DayZServer_x64.exe`). Add a config file there:

```text
# takaro_config.txt
registrationToken=YOUR_TOKEN_FROM_TAKARO_DASHBOARD
serverName=My DayZ Server
profilesDir=profiles
```

Restart the server. On boot, look for a new file:

```
<server>/logs/TakaroLogs/dayz-takaro-<timestamp>.log
```

A successful boot looks like:

```
[Takaro] DayZ DLL initializing
[Takaro] serverName=... profilesDir=profiles
[Takaro] threads started
[Takaro] RPT monitor starting; profiles dir = profiles
[Takaro] Tailing profiles\DayZServer_x64_<ts>.RPT from offset N
[Takaro] WS open to wss://connect.takaro.io/
[Takaro] connected confirmed
[Takaro] identified, gameServerId=...
```

The gameServerId line means the Takaro dashboard now shows the server as online.

## Verified

Live tested on DayZ 1.29 server `2026-04-30`:
- DLL injection works (TakaroLogs created on first boot)
- WebSocket established to `connect.takaro.io`
- Identification accepted by real Takaro, gameServerId returned
- No anti-cheat issues (DayZ server-side BattlEye is RCON-only — no kernel scanning of the server process)

## Implemented Takaro request handlers

- `testReachability` → `{connectable:true,reason:null}`
- `getPlayers` → array of `{gameId,name,steamId,platformId}` from RPT-tailed connections
- `getPlayerLocation` → `{x:0,y:0,z:0}` (RPT doesn't expose live positions; v1 task)
- `getPlayerInventory` → `[]` (same)
- `executeConsoleCommand` → ack with `{rawResult,success:true}` (real RCON dispatch is v1)
- Other actions → `{success:false,reason:"not implemented"}`

## RPT log parsing

DayZ writes a per-boot RPT file at `<profilesDir>\DayZServer_x64_<timestamp>.RPT`. We tail it for:

- `Player "NAME" (id=HASH ...) connected` → emit `player-connected`
- `Player "NAME" (id=HASH ...) has been disconnected` → emit `player-disconnected`

The `id` field is a hash, not a Steam64. To get real Steam IDs we'd need to query BattlEye RCON or hook a game function — left for v1.

## Why not a separate Node.js bridge or an Enforce Script mod?

- Enforce Script can't do WebSocket — only one-shot HTTP via `RestApi`. Can't talk Takaro's protocol.
- A Node.js sidecar works but is two processes to keep alive instead of one.
- The native DLL pattern is the same approach used by `Shroudtopia` and the Enshrouded Takaro DLL — proven to work with WinHTTP WebSocket inside game processes.
- The DayZ-specific docs are in [Lystic's "callExtension in DayZ"](https://lystic.dev/blog/2021/05/22/callextension-in-dayz/) and [KeganHollern/Infinity](https://github.com/KeganHollern/Infinity) (uses Secur32.dll proxy; we use winmm.dll because that's what DayZServer_x64.exe actually imports).
