# Event Mapping

How Takaro's canonical `GameEvents` (from `packages/lib-modules/src/dto/gameEvents.ts:7`) map onto DayZ engine hooks.

| Takaro event | Source in this mod | Hook | Notes |
| --- | --- | --- | --- |
| `player-connected` | `MissionServerTakaro.c` | `MissionServer.InvokeOnConnect(player, identity)` | Fires after the engine attaches the identity to the player object. |
| `player-disconnected` | `MissionServerTakaro.c` | `MissionServer.InvokeOnDisconnect(player)` | Fires before the player is removed; identity is still readable. |
| `chat-message` | `PluginManagerTakaro.c` | `TakaroChatRouter.Route(sender, channel, text)` | **Not bound by default.** A CF or VPP compatibility addon must call `Route()` from its chat hook. See "Chat capture" below. |
| `player-death` | `PlayerBaseTakaro.c` | `PlayerBase.EEKilled(killer)` | Killer entity passed through; weapon classname taken from `killer.GetType()`. Falls back gracefully if killer is null (zombie, env damage). |
| `entity-killed` | not implemented | — | TODO. DayZ doesn't have a single canonical "entity killed" hook; each entity class fires `EEKilled` separately. Add in a future iteration if Takaro modules need it. |
| `log` | `TakaroBridge.OnRawLogLine` | manually called | Generic log-line passthrough. Not wired to anything by default. Hook it from a custom log emitter if you want raw RPT lines forwarded. |

## IGamePlayer field mapping

DayZ field → Takaro `IGamePlayer` field (from `packages/lib-gameserver/src/interfaces/GameServer.ts`):

| Takaro field | DayZ source |
| --- | --- |
| `gameId` | `PlayerIdentity.GetPlainId()` (Steam64) |
| `name` | `PlayerIdentity.GetName()` |
| `steamId` | `PlayerIdentity.GetPlainId()` (DayZ uses the same value) |
| `ip` | not currently extracted — `PlayerIdentity` doesn't expose IP server-side in script |
| `ping` | `PlayerIdentity.GetPingAct()` |
| `epicOnlineServicesId` | n/a for DayZ |
| `xboxLiveId` | n/a for DayZ on PC |
| `platformId` | n/a — could fill with `"steam"` constant |

## Position mapping

DayZ uses a Y-up coordinate system where Y is altitude. Takaro's `IPosition` is `{x, y, z}` without a documented up-axis convention. We pass DayZ's vector through as-is:
- `position.x` = DayZ X (east-west)
- `position.y` = DayZ Y (altitude)
- `position.z` = DayZ Z (north-south)

If Takaro modules expect Z-up, we can swap `y` and `z` in `TakaroEventFactory.Death`. Decide once we have a real Takaro module exercising the data.

## Chat capture — the awkward one

DayZ's vanilla chat path is engine-side; there is no clean `OnChatSent` script hook. Three viable sources, in preference order:

1. **CommunityFramework.** CF exposes a global event for chat messages (`CF_OnChatMessage` or similar — exact symbol TBD after PBO inspection). The right way to bind it is in a separate `@TakaroIntegration_CF` addon that lists `@CF` as a `requiredAddon` in its `config.cpp`, so this base mod doesn't break on servers that don't run CF.
2. **VPPAdminTools.** VPP intercepts chat for moderation. Its scripts can be patched (or `modded class`-extended) to also call `TakaroChatRouter.Route()`.
3. **DayZ Expansion.** Expansion's chat module fires its own RPC on chat send. A compat addon could override the relevant Expansion class.

Until one of those compat addons ships, the bridge will not emit `chat-message` events. Everything else (connect/disconnect/death/operations) works without them.

## Inbound operation mapping

| Takaro `IGameServer` method | Bridge handler | Source code |
| --- | --- | --- |
| `testReachability()` | `HandleReachability` | `TakaroCommandDispatcher.c` |
| `getPlayers()` | `HandleListPlayers` | `TakaroCommandDispatcher.c` |
| `getPlayer(ref)` | `HandleGetPlayer` | iterates online players, matches `gameId` |
| `sendMessage(msg, opts)` | `HandleSendMessage` | broadcast via per-identity RPC; whisper if `recipientGameId` set |
| `kickPlayer(ref, reason)` | `HandleKick` | `GetGame().DisconnectPlayer(identity)` |
| `banPlayer(opts)` | `HandleBan` | **partial** — kicks only; needs VPP compat for real ban list |
| `unbanPlayer(ref)` | `HandleUnban` | **noop** — needs VPP compat |
| `teleportPlayer(ref, x, y, z)` | `HandleTeleport` | `PlayerBase.SetPosition(Vector(x,y,z))` |
| `giveItem(ref, item, n, q)` | `HandleGiveItem` | `PlayerBase.GetInventory().CreateInInventory(item)` × n; falls back to ground spawn |
| `executeConsoleCommand(raw)` | `HandleExecCommand` | only `say <msg>` is supported; rest return error |
| `shutdown()` | `HandleShutdown` | acknowledged-noop |
| `getPlayerLocation(ref)` | not implemented | TODO — read `PlayerBase.GetPosition()` |
| `getPlayerInventory(ref)` | not implemented | TODO — walk inventory cargo |
| `listItems()` | not implemented | TODO — would need to walk the type DB |
| `listEntities()` | not implemented | TODO |
| `listLocations()` | not implemented | TODO |
| `listBans()` | not implemented | TODO — read VPP `BanList.json` once compat exists |
| `getMapInfo()` / `getMapTile()` | not implemented | low priority |
