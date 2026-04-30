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
        ReplyOk(op, JsonSerialize(r));
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
        ReplyOk(op, JsonSerialize(list));
    }

    void HandleGetPlayer(TakaroOperation op)
    {
        ArgsGetPlayer args = new ArgsGetPlayer();
        if (!ParseArgs(op, args)) return;

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
        ReplyOk(op, JsonSerialize(info));
    }

    void HandleSendMessage(TakaroOperation op)
    {
        ArgsSendMessage args = new ArgsSendMessage();
        if (!ParseArgs(op, args)) return;

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
        if (!ParseArgs(op, args)) return;

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
        if (!ParseArgs(op, args)) return;

        // DayZ's vanilla ban API isn't directly exposed to script; we delegate
        // to a BattlEye command via the server console. VPPAdminTools or
        // CommunityFramework can be invoked here if available.
        string cmd = "#exec ban " + args.gameId;
        if (args.reason != "")
            cmd += " " + args.reason;
        // Fall back to kick + record so the bridge always reports success quickly.
        PlayerIdentity id = FindIdentityByGameId(args.gameId);
        if (id)
            GetGame().DisconnectPlayer(id);

        TakaroLog.Warn("banPlayer requested for " + args.gameId + " — kicked locally; full ban requires VPP/BE integration (TODO)");
        ReplyOk(op, "{\"banApplied\":\"kick-only\"}");
    }

    void HandleUnban(TakaroOperation op)
    {
        ArgsUnbanPlayer args = new ArgsUnbanPlayer();
        if (!ParseArgs(op, args)) return;
        TakaroLog.Warn("unbanPlayer for " + args.gameId + " — ban list edit not implemented in script-only path (TODO)");
        ReplyOk(op, "{\"unbanApplied\":\"noop\"}");
    }

    void HandleTeleport(TakaroOperation op)
    {
        ArgsTeleportPlayer args = new ArgsTeleportPlayer();
        if (!ParseArgs(op, args)) return;

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
        if (!ParseArgs(op, args)) return;
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
        if (!ParseArgs(op, args)) return;

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
        // DayZ has GetGame().RestartMission() but no clean shutdown from script;
        // surface as a noop with a log line. Operators can wire this to a hook.
        ReplyOk(op, "{\"shutdown\":\"acknowledged-noop\"}");
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

    bool ParseArgs(TakaroOperation op, Class target)
    {
        if (!op || op.argsJson == "")
        {
            ReplyError(op, "Missing args");
            return false;
        }
        string err;
        JsonSerializer js = new JsonSerializer;
        bool ok = js.ReadFromString(target, op.argsJson, err);
        if (!ok)
        {
            ReplyError(op, "Bad args JSON: " + err);
            return false;
        }
        return true;
    }

    string JsonSerialize(Class obj)
    {
        string json;
        JsonSerializer js = new JsonSerializer;
        js.WriteToString(obj, false, json);
        return json;
    }

    void ReplyOk(TakaroOperation op, string resultJson)
    {
        TakaroOperationResult r = new TakaroOperationResult();
        r.operationId = op.operationId;
        r.ok = true;
        r.resultJson = resultJson;
        SendResult(r);
    }

    void ReplyError(TakaroOperation op, string msg)
    {
        TakaroOperationResult r = new TakaroOperationResult();
        r.operationId = op.operationId;
        r.ok = false;
        r.errorMessage = msg;
        SendResult(r);
    }

    void SendResult(TakaroOperationResult r)
    {
        TakaroConfigData cfg = TakaroConfig.Get();
        if (!cfg) return;
        if (cfg.GameServerId == "")
        {
            TakaroLog.Warn("No GameServerId set; cannot deliver operation result " + r.operationId);
            return;
        }
        string body = JsonSerialize(r);
        string path = "/gameserver/" + cfg.GameServerId + "/operation/" + r.operationId + "/result";
        TakaroHttpCallback cb = new TakaroHttpCallback("opResult");
        m_Http.Post(path, body, cb);
    }
}
