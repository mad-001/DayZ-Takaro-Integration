// Inbound command pipeline.
// Takaro hands the bridge a list of pending operations via:
//   POST {api}/gameserver/{id}/poll  -> returns array of TakaroOperation
// We dispatch each operation to a handler, run it, and POST the result to:
//   POST {api}/gameserver/{id}/operation/{operationId}/result

class TakaroOperation
{
    string operationId;
    string action;          // testReachability, listPlayers, sendMessage, kickPlayer, ...
    string argsJson;        // raw JSON args; each handler parses what it needs
}

class TakaroOperationsResponse
{
    ref array<ref TakaroOperation> operations;

    void TakaroOperationsResponse()
    {
        operations = new array<ref TakaroOperation>;
    }
}

class TakaroOperationResult
{
    string operationId;
    bool ok;
    string resultJson;       // success payload as JSON
    string errorMessage;
}

// ---- Action arg shapes -------------------------------------------------

class ArgsSendMessage
{
    string message;
    string channel;          // "global" by default; future: "direct" -> recipientId
    string recipientGameId;
}

class ArgsKickPlayer
{
    string gameId;
    string reason;
}

class ArgsBanPlayer
{
    string gameId;
    string reason;
    int durationSeconds;     // 0 = permanent
}

class ArgsUnbanPlayer
{
    string gameId;
}

class ArgsTeleportPlayer
{
    string gameId;
    float x;
    float y;
    float z;
}

class ArgsGiveItem
{
    string gameId;
    string item;             // classname
    int amount;
    string quality;
}

class ArgsExecuteCommand
{
    string command;          // raw RCON-style command
}

class ArgsGetPlayer
{
    string gameId;
}

// ---- Result payload shapes --------------------------------------------

class ResultPlayerInfo
{
    string gameId;
    string name;
    string steamId;
    string ip;
    int ping;
    bool online;
}

class ResultPlayersList
{
    ref array<ref ResultPlayerInfo> players;

    void ResultPlayersList()
    {
        players = new array<ref ResultPlayerInfo>;
    }
}

class ResultReachability
{
    bool connectable;
    string reason;
}

// ---- Dispatcher --------------------------------------------------------

class TakaroCommandDispatcher
{
    TakaroHttpClient m_Http;

    void TakaroCommandDispatcher(TakaroHttpClient http)
    {
        m_Http = http;
    }

    void Dispatch(TakaroOperation op)
    {
        if (!op || op.action == "")
            return;

        TakaroLog.Debug("Dispatch: " + op.action + " (op " + op.operationId + ")");

        switch (op.action)
        {
            case "testReachability":
                HandleReachability(op);
                break;
            case "listPlayers":
            case "getPlayers":
                HandleListPlayers(op);
                break;
            case "getPlayer":
                HandleGetPlayer(op);
                break;
            case "getPlayerLocation":
                HandleGetPlayerLocation(op);
                break;
            case "getPlayerInventory":
                HandleGetPlayerInventory(op);
                break;
            case "sendMessage":
                HandleSendMessage(op);
                break;
            case "kickPlayer":
                HandleKick(op);
                break;
            case "banPlayer":
                HandleBan(op);
                break;
            case "unbanPlayer":
                HandleUnban(op);
                break;
            case "teleportPlayer":
                HandleTeleport(op);
                break;
            case "giveItem":
                HandleGiveItem(op);
                break;
            case "executeConsoleCommand":
            case "executeCommand":
                HandleExecCommand(op);
                break;
            case "shutdown":
                HandleShutdown(op);
                break;
            default:
                ReplyError(op, "Unknown action: " + op.action);
                break;
        }
    }

    // ---- handlers ------------------------------------------------------

    void HandleReachability(TakaroOperation op)
    {
        ResultReachability r = new ResultReachability();
        r.connectable = true;
        r.reason = "";
        ReplyOk(op, SerializeReachability(r));
    }

    void HandleListPlayers(TakaroOperation op)
    {
        ResultPlayersList list = new ResultPlayersList();
        array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        for (int i = 0; i < players.Count(); i++)
        {
            PlayerBase pb = PlayerBase.Cast(players[i]);
            if (!pb) continue;
            PlayerIdentity id = pb.GetIdentity();
            if (!id) continue;

            ResultPlayerInfo info = new ResultPlayerInfo();
            info.gameId = id.GetPlainId();
            info.name = id.GetName();
            info.steamId = id.GetPlainId();
            info.ping = id.GetPingAct();
            info.online = true;
            list.players.Insert(info);
        }
        ReplyOk(op, SerializePlayersList(list));
    }

    void HandleGetPlayer(TakaroOperation op)
    {
        ArgsGetPlayer args = new ArgsGetPlayer();
        if (!ParseGetPlayer(op, args)) return;

        PlayerIdentity id = FindIdentityByGameId(args.gameId);
        if (!id)
        {
            ReplyError(op, "Player not online: " + args.gameId);
            return;
        }
        ResultPlayerInfo info = new ResultPlayerInfo();
        info.gameId = id.GetPlainId();
        info.name = id.GetName();
        info.steamId = id.GetPlainId();
        info.ping = id.GetPingAct();
        info.online = true;
        ReplyOk(op, SerializePlayerInfo(info));
    }

    void HandleGetPlayerInventory(TakaroOperation op)
    {
        ArgsGetPlayer args = new ArgsGetPlayer();
        if (!ParseGetPlayer(op, args)) return;

        PlayerBase pb = FindPlayerByGameId(args.gameId);
        if (!pb)
        {
            // Empty inventory for offline player.
            ReplyOk(op, "[]");
            return;
        }
        // Walk the player's inventory and produce a Takaro IItemDTO array:
        //   [{name: "<classname>", code: "<classname>", amount: <int>, quality: <string>}]
        array<EntityAI> items = new array<EntityAI>;
        pb.GetInventory().EnumerateInventory(InventoryTraversalType.PREORDER, items);
        string json = "[";
        bool first = true;
        for (int i = 0; i < items.Count(); i++)
        {
            EntityAI it = items[i];
            if (!it) continue;
            string code = it.GetType();
            if (code == "") continue;
            int amount = 1;
            // Stack count for stackable items (ammo etc.)
            ItemBase ib = ItemBase.Cast(it);
            if (ib) amount = (int)ib.GetQuantity();
            if (amount <= 0) amount = 1;
            if (!first) json += ",";
            first = false;
            string q = "\"";
            string entry = "{";
            entry += q + "name" + q + ":" + q + code + q + ",";
            entry += q + "code" + q + ":" + q + code + q + ",";
            entry += q + "amount" + q + ":" + amount.ToString() + ",";
            entry += q + "quality" + q + ":" + q + q;
            entry += "}";
            json += entry;
        }
        json += "]";
        ReplyOk(op, json);
    }

    void HandleGetPlayerLocation(TakaroOperation op)
    {
        ArgsGetPlayer args = new ArgsGetPlayer();
        if (!ParseGetPlayer(op, args)) return;

        PlayerBase pb = FindPlayerByGameId(args.gameId);
        if (!pb)
        {
            // Per Takaro contract, returning null is valid for "player not online".
            ReplyOk(op, "null");
            return;
        }
        vector pos = pb.GetPosition();
        // Build IPosition manually so we don't risk float-as-int rendering.
        string json = "{\"x\":" + pos[0].ToString() + ",\"y\":" + pos[1].ToString() + ",\"z\":" + pos[2].ToString() + "}";
        ReplyOk(op, json);
    }

    void HandleSendMessage(TakaroOperation op)
    {
        ArgsSendMessage args = new ArgsSendMessage();
        if (!ParseSendMessage(op, args)) return;

        if (args.recipientGameId != "")
        {
            PlayerIdentity id = FindIdentityByGameId(args.recipientGameId);
            if (!id)
            {
                ReplyError(op, "Recipient not online: " + args.recipientGameId);
                return;
            }
            // Whisper: send only to that identity.
            BroadcastSystemMessage(args.message, id);
        }
        else
        {
            BroadcastSystemMessage(args.message, null);
        }
        ReplyOk(op, "{}");
    }

    void HandleKick(TakaroOperation op)
    {
        ArgsKickPlayer args = new ArgsKickPlayer();
        if (!ParseKick(op, args)) return;

        PlayerIdentity id = FindIdentityByGameId(args.gameId);
        if (!id)
        {
            ReplyError(op, "Player not online: " + args.gameId);
            return;
        }
        GetGame().DisconnectPlayer(id);
        ReplyOk(op, "{}");
    }

    void HandleBan(TakaroOperation op)
    {
        ArgsBanPlayer args = new ArgsBanPlayer();
        if (!ParseBan(op, args)) return;

        // BattlEye stores GUID-based bans in <serverRoot>\battleye\bans.txt.
        // Format per line: "GUID DURATION REASON"
        // DURATION = -1 for permanent, otherwise minutes.
        // We can't compute the BE GUID from the Steam64 in script (needs MD5),
        // so we ban by Steam ID using DayZ's online-player kick first; the DLL
        // also computes the BE GUID and writes it to bans.txt. For now, kick
        // + write a Steam-ID line so admins can match later.
        PlayerIdentity id = FindIdentityByGameId(args.gameId);
        if (id)
            GetGame().DisconnectPlayer(id);

        AppendBanLine(args.gameId, args.durationSeconds, args.reason);
        TakaroLog.Info("banPlayer applied: " + args.gameId + " duration=" + args.durationSeconds.ToString() + "s reason=" + args.reason);
        ReplyOk(op, "{}");
    }

    void HandleUnban(TakaroOperation op)
    {
        ArgsUnbanPlayer args = new ArgsUnbanPlayer();
        if (!ParseUnban(op, args)) return;
        RemoveBanLine(args.gameId);
        TakaroLog.Info("unbanPlayer applied: " + args.gameId);
        ReplyOk(op, "{}");
    }

    void AppendBanLine(string gameId, int durationSeconds, string reason)
    {
        // BattlEye bans.txt is plain text: "GUID-or-name DURATION REASON" per line.
        // Vanilla DayZ also reads "ban.txt" at server root for steamID-based bans.
        // We write to BOTH so whichever the server uses picks it up.
        int durMinutes = -1;
        if (durationSeconds > 0) durMinutes = (durationSeconds + 59) / 60;
        string line = gameId + " " + durMinutes.ToString() + " " + reason + "\n";

        // Vanilla DayZ ban.txt — Steam64 IDs, one per line, no extra columns.
        FileHandle f = OpenFile("ban.txt", FileMode.APPEND);
        if (f != 0)
        {
            FPrint(f, gameId + "\n");
            CloseFile(f);
        }
        // BE bans.txt — full ban entry
        FileHandle bf = OpenFile("battleye/bans.txt", FileMode.APPEND);
        if (bf != 0)
        {
            FPrint(bf, line);
            CloseFile(bf);
        }
    }

    void RemoveBanLine(string gameId)
    {
        // Read both files, write back without lines containing the gameId.
        RemoveBanFromFile("ban.txt", gameId);
        RemoveBanFromFile("battleye/bans.txt", gameId);
    }

    void RemoveBanFromFile(string path, string gameId)
    {
        FileHandle f = OpenFile(path, FileMode.READ);
        if (f == 0) return;
        array<string> kept = new array<string>;
        string line;
        while (FGets(f, line) > 0)
        {
            if (line.IndexOf(gameId) == -1)
                kept.Insert(line);
        }
        CloseFile(f);

        FileHandle wf = OpenFile(path, FileMode.WRITE);
        if (wf == 0) return;
        for (int i = 0; i < kept.Count(); i++)
            FPrint(wf, kept[i] + "\n");
        CloseFile(wf);
    }

    void HandleTeleport(TakaroOperation op)
    {
        ArgsTeleportPlayer args = new ArgsTeleportPlayer();
        if (!ParseTeleport(op, args)) return;

        PlayerBase pb = FindPlayerByGameId(args.gameId);
        if (!pb)
        {
            ReplyError(op, "Player not online: " + args.gameId);
            return;
        }
        vector destination = Vector(args.x, args.y, args.z);
        pb.SetPosition(destination);
        ReplyOk(op, "{}");
    }

    void HandleGiveItem(TakaroOperation op)
    {
        ArgsGiveItem args = new ArgsGiveItem();
        if (!ParseGiveItem(op, args)) return;
        if (args.amount <= 0) args.amount = 1;

        PlayerBase pb = FindPlayerByGameId(args.gameId);
        if (!pb)
        {
            ReplyError(op, "Player not online: " + args.gameId);
            return;
        }
        for (int i = 0; i < args.amount; i++)
        {
            EntityAI item = pb.GetInventory().CreateInInventory(args.item);
            if (!item)
            {
                // Drop on ground at player's feet as a fallback.
                GetGame().CreateObject(args.item, pb.GetPosition());
            }
        }
        ReplyOk(op, "{}");
    }

    void HandleExecCommand(TakaroOperation op)
    {
        ArgsExecuteCommand args = new ArgsExecuteCommand();
        if (!ParseExec(op, args)) return;

        // Treat as a "say all" if it's a plain message; otherwise log it.
        // Script-side execution of arbitrary BE/RCON commands isn't supported
        // — we record what was requested and return ok=false for unknown verbs.
        string trimmed = args.command;
        trimmed.TrimInPlace();

        if (trimmed.IndexOf("say ") == 0)
        {
            string msg = trimmed.Substring(4, trimmed.Length() - 4);
            BroadcastSystemMessage(msg, null);
            ReplyOk(op, "{}");
            return;
        }

        TakaroLog.Warn("executeConsoleCommand: not script-executable — " + trimmed);
        ReplyError(op, "Command not script-executable: " + trimmed);
    }

    void HandleShutdown(TakaroOperation op)
    {
        TakaroLog.Warn("Shutdown requested by Takaro");
        ReplyOk(op, "{}");
        // Disconnect all players first so persistence has a chance to flush.
        array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        for (int i = 0; i < players.Count(); i++)
        {
            PlayerBase pb = PlayerBase.Cast(players[i]);
            if (!pb) continue;
            PlayerIdentity id = pb.GetIdentity();
            if (id) GetGame().DisconnectPlayer(id);
        }
        // Tell the engine to exit. RequestExit is the canonical clean-shutdown
        // entrypoint; the integer reason code is informational (1 = admin-shutdown).
        GetGame().RequestExit(1);
    }

    // ---- helpers -------------------------------------------------------

    PlayerIdentity FindIdentityByGameId(string gameId)
    {
        array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        for (int i = 0; i < players.Count(); i++)
        {
            PlayerBase pb = PlayerBase.Cast(players[i]);
            if (!pb) continue;
            PlayerIdentity id = pb.GetIdentity();
            if (id && id.GetPlainId() == gameId)
                return id;
        }
        return null;
    }

    PlayerBase FindPlayerByGameId(string gameId)
    {
        array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        for (int i = 0; i < players.Count(); i++)
        {
            PlayerBase pb = PlayerBase.Cast(players[i]);
            if (!pb) continue;
            PlayerIdentity id = pb.GetIdentity();
            if (id && id.GetPlainId() == gameId)
                return pb;
        }
        return null;
    }

    void BroadcastSystemMessage(string msg, PlayerIdentity onlyTo)
    {
        // Use vanilla SendChatToServer / chat broadcast via the standard
        // ChatMessageEventTypeID broadcast. Most servers route system messages
        // through "MissionServer.NotifyAll(msg)" or a Chat helper class.
        Param1<string> param = new Param1<string>(msg);
        if (onlyTo)
        {
            GetGame().RPCSingleParam(null, ERPCs.RPC_USER_SYNC_PERMISSIONS, param, true, onlyTo);
            // Best-effort: also send via ChatPlayerComponent if available.
        }
        else
        {
            // Broadcast to all by iterating players.
            array<Man> players = new array<Man>;
            GetGame().GetPlayers(players);
            for (int i = 0; i < players.Count(); i++)
            {
                PlayerBase pb = PlayerBase.Cast(players[i]);
                if (!pb) continue;
                PlayerIdentity id = pb.GetIdentity();
                if (id)
                    GetGame().RPCSingleParam(null, ERPCs.RPC_USER_SYNC_PERMISSIONS, param, true, id);
            }
        }
        // Also write to RPT so admins can see it server-side.
        TakaroLog.Info("BROADCAST: " + msg);
    }

    // Per-arg-type parsers. We can't use a generic Class-typed parser because
    // JsonSerializer in Enforce Script needs the static type at the call site
    // for proper field introspection.
    bool ParseGetPlayer(TakaroOperation op, out ArgsGetPlayer args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        string err; JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(args, op.argsJson, err)) { ReplyError(op, "Bad args: " + err); return false; }
        return true;
    }
    bool ParseSendMessage(TakaroOperation op, out ArgsSendMessage args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        string err; JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(args, op.argsJson, err)) { ReplyError(op, "Bad args: " + err); return false; }
        return true;
    }
    bool ParseKick(TakaroOperation op, out ArgsKickPlayer args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        string err; JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(args, op.argsJson, err)) { ReplyError(op, "Bad args: " + err); return false; }
        return true;
    }
    bool ParseBan(TakaroOperation op, out ArgsBanPlayer args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        string err; JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(args, op.argsJson, err)) { ReplyError(op, "Bad args: " + err); return false; }
        return true;
    }
    bool ParseUnban(TakaroOperation op, out ArgsUnbanPlayer args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        string err; JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(args, op.argsJson, err)) { ReplyError(op, "Bad args: " + err); return false; }
        return true;
    }
    bool ParseTeleport(TakaroOperation op, out ArgsTeleportPlayer args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        string err; JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(args, op.argsJson, err)) { ReplyError(op, "Bad args: " + err); return false; }
        return true;
    }
    bool ParseGiveItem(TakaroOperation op, out ArgsGiveItem args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        string err; JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(args, op.argsJson, err)) { ReplyError(op, "Bad args: " + err); return false; }
        return true;
    }
    bool ParseExec(TakaroOperation op, out ArgsExecuteCommand args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        string err; JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(args, op.argsJson, err)) { ReplyError(op, "Bad args: " + err); return false; }
        return true;
    }

    // Generic serialization helpers — the JsonSerializer works correctly when
    // given the concrete type, so we provide one helper per result class
    // rather than passing through a `Class`-typed wrapper which serializes
    // to an empty object.
    string SerializeReachability(ResultReachability r)
    {
        string s; JsonSerializer js = new JsonSerializer; js.WriteToString(r, false, s); return s;
    }
    string SerializePlayersList(ResultPlayersList r)
    {
        string s; JsonSerializer js = new JsonSerializer; js.WriteToString(r, false, s); return s;
    }
    string SerializePlayerInfo(ResultPlayerInfo r)
    {
        string s; JsonSerializer js = new JsonSerializer; js.WriteToString(r, false, s); return s;
    }

    // Build the operation-result body manually. We deliberately keep
    // `result` as an inline JSON object (not a quoted string) so we don't
    // have to escape its content — Enforce Script's parser handles escape
    // sequences awkwardly. operationId and errorMessage are simple values
    // that callers ensure don't contain quotes or control chars.
    string BuildResultJson(string operationId, bool ok, string resultJson, string errorMessage)
    {
        string okLit;
        if (ok) okLit = "true"; else okLit = "false";
        string body = "{" + Quote("operationId") + ":" + Quote(operationId) + "," + Quote("ok") + ":" + okLit;
        if (resultJson != "")
            body += "," + Quote("result") + ":" + resultJson;
        if (errorMessage != "")
            body += "," + Quote("error") + ":" + Quote(errorMessage);
        body += "}";
        return body;
    }

    // Wrap a string in JSON-style double quotes. Inputs are expected to be
    // simple identifiers / IDs / human-readable error messages without quotes
    // or control characters; we don't try to do full JSON escaping here.
    string Quote(string s)
    {
        return "\"" + s + "\"";
    }

    void ReplyOk(TakaroOperation op, string resultJson)
    {
        SendResultRaw(op.operationId, true, resultJson, "");
    }

    void ReplyError(TakaroOperation op, string msg)
    {
        SendResultRaw(op.operationId, false, "", msg);
    }

    void SendResultRaw(string operationId, bool ok, string resultJson, string errorMessage)
    {
        TakaroConfigData cfg = TakaroConfig.Get();
        if (!cfg) return;
        if (cfg.GameServerId == "")
        {
            TakaroLog.Warn("No GameServerId set; cannot deliver operation result " + operationId);
            return;
        }
        string body = BuildResultJson(operationId, ok, resultJson, errorMessage);
        string path = "/gameserver/" + cfg.GameServerId + "/operation/" + operationId + "/result";
        TakaroHttpCallback cb = new TakaroHttpCallback("opResult");
        m_Http.Post(path, body, cb);
    }
}
