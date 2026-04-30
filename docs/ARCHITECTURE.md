# Architecture

## High-level

```
┌─────────────────────────────────────────────────────────────┐
│  DayZ Server process (DayZServer_x64.exe)                   │
│                                                             │
│  Vanilla mods (-mod=)         Server-only (-serverMod=)     │
│  ┌──────────────┐             ┌──────────────────────────┐  │
│  │ @CF          │             │ @TakaroIntegration       │  │
│  │ @VPPAdmin..  │ ─ events ─► │  ┌────────────────────┐  │  │
│  │ @Expansion   │             │  │ MissionServer hook │  │  │
│  │ ...          │             │  ├────────────────────┤  │  │
│  └──────────────┘             │  │ TakaroBridge       │  │  │
│                               │  │   ├ EventQueue     │  │  │
│                               │  │   ├ HttpClient     │  │  │
│                               │  │   └ Dispatcher     │  │  │
│                               │  └─────────┬──────────┘  │  │
│                               └────────────┼─────────────┘  │
└────────────────────────────────────────────┼────────────────┘
                                             │
                                             ▼
                              ┌─────────────────────────────┐
                              │   Takaro API                │
                              │   POST /gameserver/{id}/    │
                              │     events                  │
                              │     register                │
                              │     poll  (GET)             │
                              │     operation/{opId}/result │
                              └─────────────────────────────┘
```

## Why an in-game mod, not an external bridge

The original brief recommended a Node.js bridge wrapping BattlEye RCON + RPT log tail. We pivoted to a server-side DayZ mod for three reasons:

1. **Latency.** Events surface in the engine's hooks (`MissionServer.InvokeOnConnect`, `PlayerBase.EEKilled`, CF chat events) milliseconds after they happen. The log-tail path waits for the engine to flush the RPT, which can be 100s of ms.
2. **Fidelity.** From script we have direct access to `PlayerIdentity`, the killer entity, the weapon classname, world position. The log-tail path has to regex-match log lines whose format changes between DayZ versions.
3. **Operations.** No second process to keep alive, no port to firewall, no separate auth surface. The mod is just another addon in the server folder.

Trade-offs we accept:
- DayZ's `RestApi`/`RestContext` is HTTP-only — no WebSocket. So we use Takaro's polling-based generic adapter shape (POST events + GET poll), not the WebSocket model the Palworld bridge uses.
- The header API on `RestContext` is single-string. We pack `Authorization` into the `SetHeader` call; if your DayZ build supports a richer header API, swap `TakaroHttpClient.NewContext` accordingly.
- Some operations (writing to the BattlEye ban list, hard server shutdown) aren't directly exposed to script. We document those as TODOs that route through VPPAdminTools when present.

## Component breakdown

### `MissionServerTakaro.c` (`5_Mission/`)
A `modded class MissionServer`. Owns the `TakaroBridge` instance, calls its lifecycle hooks (`OnInit`, `OnUpdate`, `InvokeOnConnect`, `InvokeOnDisconnect`). Server-only, never reaches the client.

### `PlayerBaseTakaro.c` (`5_Mission/`)
A `modded class PlayerBase` overriding `EEKilled` to extract the killer entity and weapon, then routes the event to the bridge.

### `PluginManagerTakaro.c` (`5_Mission/`)
Holds `TakaroChatRouter.Route()` — a single function any chat-emitting mod can call to route a chat line into the bridge. We do **not** subscribe directly to a CF event from the base mod, because that would create a hard dependency on CF. A separate compatibility addon (TODO) can depend on CF and bind the actual event.

### `TakaroBridge.c` (`4_World/`)
The orchestrator. Holds the HTTP client, event queue, and dispatcher. Drives two timers off `OnUpdate`:
- **Flush timer** (`EventBatchIntervalMs`) — drains up to `MaxEventsPerBatch` events from the queue and POSTs them as a batch.
- **Poll timer** (`CommandPollIntervalMs`) — GETs pending operations from Takaro and hands each to the dispatcher.

Both timers are gated on the previous request being done, so a slow Takaro doesn't pile up concurrent in-flight HTTP calls. If a flush fails, the in-flight batch is requeued at the head.

### `TakaroEventQueue.c` (`4_World/`)
- `TakaroEventQueue` — bounded FIFO. Drops events past `maxQueueLength` (default 2000) with a logged drop count.
- `TakaroEventFactory` — static helpers to build canonical Takaro events (`player-connected`, `chat-message`, etc.) from DayZ engine objects. Maps `PlayerIdentity.GetPlainId()` → `gameId` + `steamId`.

### `TakaroHttpClient.c` (`4_World/`)
- `TakaroHttpClient` — wraps `RestApi` / `RestContext`, owns the auth header, exposes `Get(path, callback)` and `Post(path, body, callback)`.
- `TakaroHttpCallback` (extends `RestCallback`) — base class with `m_Done`, `m_Ok`, `m_StatusCode`, `m_ResponseBody`. Subclasses override `OnComplete()` to handle the response.

### `TakaroCommandDispatcher.c` (`4_World/`)
Dispatches inbound `TakaroOperation` records to handlers. Each handler:
1. Parses its arg shape via `JsonSerializer.ReadFromString`.
2. Performs the action against the engine.
3. Replies to Takaro with `ReplyOk` / `ReplyError`, which POSTs to `/gameserver/{id}/operation/{opId}/result`.

Currently implemented:
- `testReachability`, `listPlayers`, `getPlayer`
- `sendMessage` (broadcast or whisper via RPC)
- `kickPlayer` (via `GetGame().DisconnectPlayer`)
- `banPlayer` (kicks; full ban deferred to VPP compat addon)
- `unbanPlayer` (no-op; deferred)
- `teleportPlayer`, `giveItem`
- `executeConsoleCommand` (only `say <msg>` is script-executable; others return error)
- `shutdown` (acknowledged but not actioned — no script-side clean shutdown in DayZ)

### `TakaroConfig.c` (`3_Game/`)
Persistent config + logger. Config lives at `$profile:TakaroIntegration/config.json`, deserialized into `TakaroConfigData`. On first boot we write defaults and warn the operator. The bridge stays inert until tokens are filled in.

`TakaroLog` is the only logger used in the project. `Info`/`Warn`/`Error` always emit to RPT; `Debug` only emits when `LogVerbose=true`.

## Data flow examples

### Player connects
1. Client connects → engine fires `MissionServer.InvokeOnConnect(player, identity)`.
2. Modded override calls `m_TakaroBridge.OnPlayerConnected(player, identity)`.
3. Bridge creates a `TakaroEvent` via `TakaroEventFactory.Connected(identity)` and enqueues it.
4. On the next flush tick, the event is included in the batch sent to `POST /gameserver/{id}/events`.

### Takaro kicks a player
1. Bridge polls: `GET /gameserver/{id}/poll` → returns `[{operationId, action: "kickPlayer", args: '{"gameId":"...","reason":"..."}'}]`.
2. Dispatcher parses args, finds the matching `PlayerIdentity` via `FindIdentityByGameId`.
3. Calls `GetGame().DisconnectPlayer(id)`.
4. POSTs `{operationId, ok:true}` back to `/gameserver/{id}/operation/{operationId}/result`.

### Death
1. Player dies → `PlayerBase.EEKilled(killer)` fires.
2. Modded override extracts `killer` (an `Object`) and the weapon classname (`killer.GetType()`).
3. Bridge enqueues a `TakaroEvent` with type `player-death`, attacker (if a player), and the victim's position.

## Known gaps / TODO

- **CF chat hook.** Build a separate `@TakaroIntegration_CF` compat addon that depends on `@CF` and binds the CF chat event. Without it chat-message events only fire when another mod calls `TakaroChatRouter.Route()` directly.
- **VPP ban integration.** `banPlayer` should write to `profiles/VPPAdminTools/BanList.json` and notify VPP. Build `@TakaroIntegration_VPP` compat addon.
- **Multi-header HTTP.** `RestContext.SetHeader` may behave differently on newer DayZ builds. If Takaro starts rejecting requests on a 1.2x server, revisit `TakaroHttpClient.NewContext`.
- **`shutdown` operation.** No clean script-side shutdown exists. We acknowledge with a noop. Could be wired through VPP's shutdown command if VPP is loaded.
- **`executeConsoleCommand`.** Only `say` is supported. Most BattlEye RCON commands aren't reachable from script. The right answer is for Takaro to send those over BE RCON directly to port 2306 — which the bridge doesn't try to be.
