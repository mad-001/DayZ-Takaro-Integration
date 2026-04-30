# Production Deployment

How to add the Takaro integration to the public DayZ server (`\\SERVER\GameServers\dayz`).

## What gets deployed

Two pieces, both server-side only:

1. **Native DLLs** dropped next to `DayZServer_x64.exe`:
   - `winmm.dll` — proxy that hijacks the game's `winmm.dll` import and loads `dayz_takaro.dll`
   - `dayz_takaro.dll` — the integration: WS client to Takaro + local HTTP server for the script mod
   - `takaro_config.txt` — registration token, server name, port

2. **Companion mod** `@TakaroIntegration` loaded with `-serverMod=`:
   - Built from `src/takaro_integration/` and packed to `TakaroIntegration.pbo`
   - Hooks player connect/disconnect/death and POSTs events to the DLL over `localhost`
   - Polls the DLL for queued Takaro requests and dispatches them via vanilla game APIs
   - Server-only — clients never download it

3. **Optional Expansion compat addon** `@TakaroIntegration_Expansion` (load if `@DayZ-Expansion-Bundle` is on the server):
   - Hooks `ExpansionGlobalChatModule.AddChatMessage_Server` and routes chat into the Takaro bridge
   - Without this addon, `chat-message` events are not emitted (the rest still works)
   - Built from `src/takaro_integration_expansion/`, packed to `TakaroIntegration_Expansion.pbo`

## Pre-flight

1. **Get a fresh registration token from Takaro.** Don't reuse the local-test token; create a new gameserver entry in the dashboard for the public server. Copy its registrationToken.
2. **Pick a local HTTP port** that isn't already in use on the production server. `8089` works if nothing else is bound to it.
3. **Confirm BattlEye config still works** — we don't touch it, but verify `\\SERVER\GameServers\dayz\battleye\beserver_x64.cfg` still has `RConPort 2306` (or whatever you use).

## Deploy

From a Windows PowerShell prompt on the dev machine (or directly on the server):

```powershell
$repo = '\\wsl.localhost\Ubuntu\home\zmedh\Takaro-Projects\DayZ'
$server = '\\SERVER\GameServers\dayz'
$build = 'C:\temp\dayz_dll_build'

# 1. Build the DLLs (one-time per code change)
Copy-Item -Path "$repo\DayZ-DLL\*.cpp" -Destination $build -Force
Copy-Item -Path "$repo\DayZ-DLL\build.bat" -Destination $build -Force
Push-Location $build
& cmd /c build.bat
Pop-Location

# 2. Build the script mod PBO (one-time per code change)
wsl -e python3 /home/zmedh/Takaro-Projects/DayZ/scripts/pack_pbo.py `
    /home/zmedh/Takaro-Projects/DayZ/src/takaro_integration `
    /home/zmedh/Takaro-Projects/DayZ/@TakaroIntegration/Addons/TakaroIntegration.pbo `
    --prefix TakaroIntegration

# 3. Deploy DLLs to server root
Copy-Item "$build\dayz_takaro.dll" "$server\dayz_takaro.dll" -Force
Copy-Item "$build\winmm.dll" "$server\winmm.dll" -Force

# 4. Write the production config (use YOUR registration token)
@'
registrationToken=YOUR_PRODUCTION_REGISTRATION_TOKEN
serverName=Double Tap Bloodlines
localPort=8089
'@ | Set-Content "$server\takaro_config.txt" -Encoding ASCII

# 5. Deploy the script mod
if (Test-Path "$server\@TakaroIntegration") {
    Remove-Item "$server\@TakaroIntegration" -Recurse -Force
}
Copy-Item "$repo\@TakaroIntegration" "$server\@TakaroIntegration" -Recurse -Force

# 6. (Optional) Deploy the Expansion compat addon (only if Expansion is loaded)
wsl -e python3 /home/zmedh/Takaro-Projects/DayZ/scripts/pack_pbo.py `
    /home/zmedh/Takaro-Projects/DayZ/src/takaro_integration_expansion `
    /home/zmedh/Takaro-Projects/DayZ/@TakaroIntegration_Expansion/Addons/TakaroIntegration_Expansion.pbo `
    --prefix TakaroIntegration_Expansion
if (Test-Path "$server\@TakaroIntegration_Expansion") {
    Remove-Item "$server\@TakaroIntegration_Expansion" -Recurse -Force
}
Copy-Item "$repo\@TakaroIntegration_Expansion" "$server\@TakaroIntegration_Expansion" -Recurse -Force
```

## Wire into the server start command

Find your server's start `.bat` (typically `start_server.bat` or similar in `\\SERVER\GameServers\dayz\`). Add `@TakaroIntegration` to the **`-serverMod=`** list (NOT `-mod=`). It must be loaded as serverMod so clients don't get told to download it.

Existing line probably looks like:
```bat
DayZServer_x64.exe -config=serverDZ.cfg -port=2302 -profiles=profiles ^
    -mod="@CF;@DabsFramework;@DayZ-Expansion-Licensed;@DayZ-Expansion-Bundle;@BaseBuildingPlus;@BBP_ExpansionCompat;@TraderPlus;@VPPAdminTools" ^
    -dologs -adminlog -netlog -freezecheck
```

Add (note the new `-serverMod=` line):
```bat
DayZServer_x64.exe -config=serverDZ.cfg -port=2302 -profiles=profiles ^
    -mod="@CF;@DabsFramework;@DayZ-Expansion-Licensed;@DayZ-Expansion-Bundle;@BaseBuildingPlus;@BBP_ExpansionCompat;@TraderPlus;@VPPAdminTools" ^
    -serverMod="@TakaroIntegration;@TakaroIntegration_Expansion" ^
    -dologs -adminlog -netlog -freezecheck
```

## First boot

1. Restart the DayZ server.
2. The DLL writes to `\\SERVER\GameServers\dayz\logs\TakaroLogs\dayz-takaro-<timestamp>.log`. Tail that file:
   ```powershell
   Get-Content "\\SERVER\GameServers\dayz\logs\TakaroLogs\dayz-takaro-*.log" -Tail 50 -Wait
   ```
   Successful boot looks like:
   ```
   [Takaro] DayZ DLL initializing v0.2 (HTTP-bridged)
   [Takaro] HTTP listening on 127.0.0.1:8089
   [Takaro] WS open to wss://connect.takaro.io/
   [Takaro] connected confirmed
   [Takaro] identified, gameServerId=<UUID>
   ```

3. The script mod writes to the standard DayZ script log (`\\SERVER\GameServers\dayz\profiles\script_<ts>.log`). Look for:
   ```
   [Takaro] Bridge v0.1.0 starting
   [Takaro] Loaded config from $profile:TakaroIntegration/config.json
   [Takaro] Already registered (gameserver=local)
   [Takaro] Bridge attached to MissionServer.OnInit
   [Takaro][DBG] GET http://localhost:8089/gameserver/local/poll
   ```

4. Open the Takaro dashboard. The server should show as **online** within ~5 seconds.

## Smoke test

From the Takaro dashboard or API:
- **Server online?** ✅ if status is online with the gameServerId from the log
- **List players** — empty array if no one's connected, populated once players join (uses real Steam64 IDs from `PlayerIdentity.GetPlainId()`)
- **Send broadcast** — issue `sendMessage` from Takaro; in-game players see a system message and the script mod logs `[Takaro] BROADCAST: <text>`
- **Kick player** — issues `kickPlayer`; script mod calls `GetGame().DisconnectPlayer(identity)`

## What's NOT yet implemented

Documented for you so the dashboard's "missing data" warnings make sense:

| Feature | Status |
| --- | --- |
| `chat-message` events | **available via `@TakaroIntegration_Expansion`** if Expansion is loaded; not available on a pure-vanilla server (no clean server-side chat hook in vanilla DayZ). |
| Real ban list write | partial — falls back to kick. Needs VPP compat addon writing to `BanList.json`. |
| `getPlayerLocation` real positions | returns `{0,0,0}` placeholder. PlayerBase position is available in script — drop a real reader in v1.5. |
| `getPlayerInventory` | returns `[]`. Inventory walk is doable but volumetric — v1.5. |
| `executeConsoleCommand` real RCON dispatch | acks but doesn't execute. Add BattlEye RCON client to DLL for this. |

## Rolling back

Stop the server. Delete:
- `\\SERVER\GameServers\dayz\winmm.dll`
- `\\SERVER\GameServers\dayz\dayz_takaro.dll`
- `\\SERVER\GameServers\dayz\takaro_config.txt`
- `\\SERVER\GameServers\dayz\@TakaroIntegration\`
- The `-serverMod="@TakaroIntegration"` arg from the start command

That's it — the DayZ server is fully restored. Nothing else on disk was modified.

## Updating

Same as deploy. The build script overwrites the DLLs in place; the PBO copy overwrites the old PBO. Restart the server to pick up changes.

## Logs to watch

| Log | What to watch for |
| --- | --- |
| `logs/TakaroLogs/dayz-takaro-*.log` | DLL: WS state, queued ops, response status, errors from Takaro |
| `profiles/script_*.log` | Script mod: poll/dispatch cycle, BROADCAST lines, error compile/runtime |
| `profiles/DayZServer_x64_*.RPT` | DayZ server's own log — use to correlate player connects/disconnects with what the bridge saw |
