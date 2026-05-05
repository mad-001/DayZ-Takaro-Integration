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
            case "listItems":
                HandleListItems(op);
                break;
            case "listEntities":
                HandleListEntities(op);
                break;
            case "listLocations":
                HandleListLocations(op);
                break;
            case "listBans":
                HandleListBans(op);
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
            PlayerBase pb = FindPlayerByGameId(args.recipientGameId);
            if (!pb)
            {
                ReplyError(op, "Recipient not online: " + args.recipientGameId);
                return;
            }
            BroadcastSystemMessage(args.message, pb);
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

        string trimmed = args.command;
        trimmed.TrimInPlace();

        string rawResult = "";
        bool success = true;

        // Tokenize on spaces — verb is the first token, rest is the argument.
        int firstSp = trimmed.IndexOf(" ");
        string verb = trimmed;
        string rest = "";
        if (firstSp >= 0)
        {
            verb = trimmed.Substring(0, firstSp);
            rest = trimmed.Substring(firstSp + 1, trimmed.Length() - firstSp - 1);
            rest.TrimInPlace();
        }
        // Verbs are case-insensitive — Help == help == HELP.
        verb.ToLower();

        if (verb == "say")
        {
            BroadcastSystemMessage(rest, null);
            rawResult = "Broadcast: " + rest;
        }
        else if (verb == "help")
        {
            rawResult = BuildHelpString();
        }
        else if (verb == "shutdown" || verb == "#shutdown")
        {
            rawResult = "Shutdown initiated";
            ReplyOk(op, BuildCommandOutput(rawResult, true));
            // Disconnect everyone first so persistence flushes, then exit.
            array<Man> all = new array<Man>;
            GetGame().GetPlayers(all);
            for (int i = 0; i < all.Count(); i++) {
                PlayerBase pb = PlayerBase.Cast(all[i]);
                if (!pb) continue;
                PlayerIdentity pid = pb.GetIdentity();
                if (pid) GetGame().DisconnectPlayer(pid);
            }
            GetGame().RequestExit(1);
            return;
        }
        else if (verb == "kick")
        {
            // kick <gameId> [reason]
            string kgid;
            string kreason;
            SplitFirstToken(rest, kgid, kreason);
            if (kgid == "") { rawResult = "Usage: kick <gameId> [reason]"; success = false; }
            else
            {
                PlayerIdentity kid = FindIdentityByGameId(kgid);
                if (!kid) { rawResult = "Player not online: " + kgid; success = false; }
                else { GetGame().DisconnectPlayer(kid); rawResult = "Kicked " + kgid; }
            }
        }
        else if (verb == "ban" || verb == "addban")
        {
            // ban <gameId> [reason]
            string bgid;
            string breason;
            SplitFirstToken(rest, bgid, breason);
            if (bgid == "") { rawResult = "Usage: ban <gameId> [reason]"; success = false; }
            else
            {
                PlayerIdentity bid = FindIdentityByGameId(bgid);
                if (bid) GetGame().DisconnectPlayer(bid);
                AppendBanLine(bgid, 0, breason);
                rawResult = "Banned " + bgid;
            }
        }
        else if (verb == "unban" || verb == "removeban")
        {
            // unban <gameId>
            if (rest == "") { rawResult = "Usage: unban <gameId>"; success = false; }
            else { RemoveBanLine(rest); rawResult = "Unbanned " + rest; }
        }
        else if (verb == "tp" || verb == "teleport")
        {
            // tp <gameId> <x> <y> <z>
            array<string> tparts = new array<string>;
            SplitTokens(rest, tparts);
            if (tparts.Count() < 4) { rawResult = "Usage: tp <gameId> <x> <y> <z>"; success = false; }
            else
            {
                PlayerBase tpb = FindPlayerByGameId(tparts[0]);
                if (!tpb) { rawResult = "Player not online: " + tparts[0]; success = false; }
                else
                {
                    float tx = tparts[1].ToFloat();
                    float ty = tparts[2].ToFloat();
                    float tz = tparts[3].ToFloat();
                    tpb.SetPosition(Vector(tx, ty, tz));
                    rawResult = "Teleported " + tparts[0] + " to " + tx.ToString() + "," + ty.ToString() + "," + tz.ToString();
                }
            }
        }
        else if (verb == "give" || verb == "giveitem")
        {
            // give <gameId> <classname> [amount]
            array<string> gparts = new array<string>;
            SplitTokens(rest, gparts);
            if (gparts.Count() < 2) { rawResult = "Usage: give <gameId> <classname> [amount]"; success = false; }
            else
            {
                PlayerBase gpb = FindPlayerByGameId(gparts[0]);
                if (!gpb) { rawResult = "Player not online: " + gparts[0]; success = false; }
                else
                {
                    int gamount = 1;
                    if (gparts.Count() >= 3) { gamount = gparts[2].ToInt(); if (gamount <= 0) gamount = 1; }
                    int gcreated = 0;
                    for (int gi = 0; gi < gamount; gi++)
                    {
                        EntityAI gent = gpb.GetInventory().CreateInInventory(gparts[1]);
                        if (!gent) GetGame().CreateObject(gparts[1], gpb.GetPosition());
                        else gcreated++;
                    }
                    rawResult = "Gave " + gamount.ToString() + "x " + gparts[1] + " to " + gparts[0] + " (" + gcreated.ToString() + " in inventory, rest dropped)";
                }
            }
        }
        else if (verb == "players" || verb == "listplayers")
        {
            array<Man> ppl = new array<Man>;
            GetGame().GetPlayers(ppl);
            rawResult = "Online (" + ppl.Count().ToString() + "):";
            for (int pi = 0; pi < ppl.Count(); pi++)
            {
                PlayerBase ppb = PlayerBase.Cast(ppl[pi]);
                if (!ppb) continue;
                PlayerIdentity pid2 = ppb.GetIdentity();
                if (pid2) rawResult += " " + pid2.GetName() + "(" + pid2.GetPlainId() + ")";
            }
        }
        else if (verb == "announce" || verb == "banner")
        {
            // Loud center-screen banner via Expansion's BAGUETTE notification.
            // Format: `announce <title> | <body>`. If no `|` present, the
            // whole text becomes the body and title defaults to "Server".
            if (rest == "") { rawResult = "Usage: announce <title> | <message>  (or: announce <message>)"; success = false; }
            else
            {
                string atitle = "Server";
                string abody = rest;
                int barIdx = rest.IndexOf("|");
                if (barIdx > 0)
                {
                    atitle = rest.Substring(0, barIdx);
                    atitle.TrimInPlace();
                    abody = rest.Substring(barIdx + 1, rest.Length() - barIdx - 1);
                    abody.TrimInPlace();
                }
                BroadcastBanner(atitle, abody);
                rawResult = "Banner sent: [" + atitle + "] " + abody;
            }
        }
        else
        {
            rawResult = "Unknown command: " + verb + ". Type 'help' for the command list.";
            success = false;
        }
        ReplyOk(op, BuildCommandOutput(rawResult, success));
    }

    // Build the help string. One verb per line with description. Kept as
    // many += statements rather than one big concat to dodge Enforce's
    // "Formula too complex" parser limit.
    string BuildHelpString()
    {
        string nl = "\n";
        string h = "Available commands:" + nl;
        h += "  say <text>                          - broadcast a system message to all players" + nl;
        h += "  help                                - show this command list" + nl;
        h += "  shutdown                            - gracefully stop the server (kicks all, RequestExit)" + nl;
        h += "  kick <gameId> [reason]              - disconnect a player (gameId = Steam64)" + nl;
        h += "  ban <gameId> [reason]               - write to ban.txt + battleye/bans.txt and kick" + nl;
        h += "  unban <gameId>                      - remove the gameId from both ban files" + nl;
        h += "  tp <gameId> <x> <y> <z>             - teleport a player to world coordinates" + nl;
        h += "  give <gameId> <classname> [amount]  - spawn item(s) into the player's inventory" + nl;
        h += "  players                             - list every online player (name + gameId)" + nl;
        h += "  announce <message>                  - center-screen banner to all players (Expansion BAGUETTE)" + nl;
        h += "  announce <title> | <message>       -   ...with a custom title (default 'Server')" + nl;
        h += nl;
        h += "Aliases: addBan/removeBan/teleport/giveItem/listPlayers/banner." + nl;
        h += "Other RCON verbs route through BattlEye when the BE RCON connection is up.";
        return h;
    }

    // Split 'rest' into [first-token, remainder-after-first-space].
    void SplitFirstToken(string s, out string first, out string remainder)
    {
        first = "";
        remainder = "";
        if (s == "") return;
        int sp = s.IndexOf(" ");
        if (sp < 0) { first = s; return; }
        first = s.Substring(0, sp);
        remainder = s.Substring(sp + 1, s.Length() - sp - 1);
        remainder.TrimInPlace();
    }

    // Whitespace-split into an array.
    void SplitTokens(string s, out array<string> parts)
    {
        if (!parts) return;
        parts.Clear();
        if (s == "") return;
        string cur = "";
        int n = s.Length();
        for (int i = 0; i < n; i++)
        {
            string ch = s.Substring(i, 1);
            if (ch == " " || ch == "\t")
            {
                if (cur != "") { parts.Insert(cur); cur = ""; }
            }
            else
            {
                cur += ch;
            }
        }
        if (cur != "") parts.Insert(cur);
    }

    string BuildCommandOutput(string rawResult, bool success)
    {
        string q = "\"";
        string s = "{";
        s += q + "rawResult" + q + ":" + q + JsonSafeString(rawResult) + q + ",";
        s += q + "success" + q + ":";
        if (success) s += "true"; else s += "false";
        s += "}";
        return s;
    }

    // JSON-escape rawResult content so it survives Takaro's JSON parser
    // and renders multi-line strings as actual newlines on the dashboard.
    // Raw control chars in a JSON string value are invalid (Takaro rejects
    // the whole response, which surfaces as a 10s request timeout). We
    // escape newlines/CRs/tabs as the two-char JSON escape sequences and
    // drop double-quotes.
    //
    // CParser gotcha: `"\\"` literals break Enforce's parser, but `"\\n"`
    // parses fine because the lexer sees `\\` as one escaped backslash
    // followed by a regular `n` — net result is the 2-char string `\n`.
    string JsonSafeString(string s)
    {
        string out_s = s;
        string dq = "\"";
        out_s.Replace(dq, "'");
        string nl = "\n";
        out_s.Replace(nl, "\\n");
        string cr = "\r";
        out_s.Replace(cr, "\\r");
        string tab = "\t";
        out_s.Replace(tab, "\\t");
        return out_s;
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

    // Walks CfgVehicles and returns spawnable items as IItemDTO[]. Filters by
    // scope (>=1 = inventory item or world-spawnable). Cap at MAX_LIST_ITEMS
    // so we don't ship a 30k-entry payload over WS.
    void HandleListItems(TakaroOperation op)
    {
        const int MAX_LIST_ITEMS = 5000;
        const string ROOT = "CfgVehicles";
        int count = GetGame().ConfigGetChildrenCount(ROOT);
        string q = "\"";
        string json = "[";
        bool first = true;
        int emitted = 0;
        for (int i = 0; i < count && emitted < MAX_LIST_ITEMS; i++)
        {
            string cls;
            GetGame().ConfigGetChildName(ROOT, i, cls);
            if (cls == "") continue;
            string base = ROOT + " " + cls + " ";
            int scope = GetGame().ConfigGetInt(base + "scope");
            if (scope < 1) continue;
            string display = "";
            GetGame().ConfigGetText(base + "displayName", display);
            if (display == "") display = cls;
            display = JsonSafeString(display);
            string entry = "{";
            entry += q + "name" + q + ":" + q + display + q + ",";
            entry += q + "code" + q + ":" + q + cls + q + ",";
            entry += q + "amount" + q + ":1,";
            entry += q + "quality" + q + ":" + q + q;
            entry += "}";
            if (!first) json += ",";
            first = false;
            json += entry;
            emitted++;
        }
        json += "]";
        TakaroLog.Info("listItems: " + emitted.ToString() + " items returned");
        ReplyOk(op, json);
    }

    // Same idea but for AI/animal/zombie entity types — anything in CfgVehicles
    // whose base class chain includes 'DZ_LightAI_Base', 'DayZAnimal', etc.
    void HandleListEntities(TakaroOperation op)
    {
        const int MAX = 2000;
        const string ROOT = "CfgVehicles";
        int count = GetGame().ConfigGetChildrenCount(ROOT);
        string q = "\"";
        string json = "[";
        bool first = true;
        int emitted = 0;
        for (int i = 0; i < count && emitted < MAX; i++)
        {
            string cls;
            GetGame().ConfigGetChildName(ROOT, i, cls);
            if (cls == "") continue;
            // Filter to entity types: animals, infected (zombies), AI.
            bool isEntity = false;
            if (GetGame().IsKindOf(cls, "DayZAnimal")) isEntity = true;
            else if (GetGame().IsKindOf(cls, "DayZInfected")) isEntity = true;
            else if (GetGame().IsKindOf(cls, "DayZCreature")) isEntity = true;
            if (!isEntity) continue;
            string display = "";
            GetGame().ConfigGetText(ROOT + " " + cls + " displayName", display);
            if (display == "") display = cls;
            display = JsonSafeString(display);
            string entry = "{";
            entry += q + "name" + q + ":" + q + display + q + ",";
            entry += q + "code" + q + ":" + q + cls + q + ",";
            entry += q + "type" + q + ":" + q + "entity" + q;
            entry += "}";
            if (!first) json += ",";
            first = false;
            json += entry;
            emitted++;
        }
        json += "]";
        TakaroLog.Info("listEntities: " + emitted.ToString() + " entities returned");
        ReplyOk(op, json);
    }

    // Map locations from the world's location config. DayZ exposes these via
    // GetGame().GetWorld().GetMapLoctList — typed result is array<MapLocation>.
    // We surface name + position as IMapLocationDTO[].
    void HandleListLocations(TakaroOperation op)
    {
        // DayZ doesn't have a stable script API for enumerating named locations;
        // they live in the world config. Returning [] is the honest default;
        // server-specific location lists can be added by a custom mission later.
        ReplyOk(op, "[]");
        TakaroLog.Info("listLocations: returned empty (no enumeration API in vanilla)");
    }

    // Single ban entry builder — extracted from HandleListBans because Enforce
    // Script's parser flags single-line concatenations of more than ~10
    // segments as "Formula too complex".
    string BuildBanEntry(string gameId, string reason)
    {
        string q = "\"";
        string s = "{";
        s += q + "player" + q + ":{";
        s += q + "gameId" + q + ":" + q + gameId + q;
        s += "},";
        s += q + "reason" + q + ":" + q + reason + q + ",";
        s += q + "expiresAt" + q + ":null";
        s += "}";
        return s;
    }

    // Reads vanilla DayZ ban.txt (Steam64-per-line) AND BattlEye bans.txt
    // (GUID DURATION REASON). Merges, dedups, returns IBanDTO[].
    void HandleListBans(TakaroOperation op)
    {
        string q = "\"";
        string json = "[";
        bool first = true;
        int emitted = 0;

        // Vanilla ban.txt — one Steam64 per line (sometimes followed by name)
        FileHandle f = OpenFile("ban.txt", FileMode.READ);
        if (f != 0)
        {
            string line;
            while (FGets(f, line) > 0)
            {
                line.TrimInPlace();
                if (line == "") continue;
                int sp = line.IndexOf(" ");
                string steamId;
                if (sp >= 0) steamId = line.Substring(0, sp); else steamId = line;
                if (steamId == "") continue;
                if (!first) json += ",";
                first = false;
                json += BuildBanEntry(steamId, "");
                emitted++;
            }
            CloseFile(f);
        }

        // BE bans.txt — "GUID DURATION REASON" per line
        FileHandle bf = OpenFile("battleye/bans.txt", FileMode.READ);
        if (bf != 0)
        {
            string bline;
            while (FGets(bf, bline) > 0)
            {
                bline.TrimInPlace();
                if (bline == "" || bline.IndexOf("//") == 0) continue;
                int sp1 = bline.IndexOf(" ");
                if (sp1 < 0) continue;
                string guid = bline.Substring(0, sp1);
                string rest = bline.Substring(sp1 + 1, bline.Length() - sp1 - 1);
                rest.TrimInPlace();
                int sp2 = rest.IndexOf(" ");
                string reason = "";
                if (sp2 >= 0)
                    reason = JsonSafeString(rest.Substring(sp2 + 1, rest.Length() - sp2 - 1));
                if (!first) json += ",";
                first = false;
                json += BuildBanEntry(guid, reason);
                emitted++;
            }
            CloseFile(bf);
        }

        json += "]";
        TakaroLog.Info("listBans: " + emitted.ToString() + " bans returned");
        ReplyOk(op, json);
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

    void BroadcastSystemMessage(string msg, PlayerBase onlyTo)
    {
        // With Expansion loaded we use the global chat channel (CCGlobal),
        // which renders the entire message body in GlobalChatColor (88,195,255
        // — sky blue) — closest available preset to Discord blurple. Note:
        // ExpansionChatLine.FormatText replaces every "<" / ">" with the
        // unicode look-alikes "‹" / "›", which kills inline markup, and
        // ExpansionChatLine.SetColorByName does not parse hex codes — only
        // named presets — so an exact #5865F2 isn't reachable server-side.
#ifdef EXPANSIONMOD
        ExpansionGlobalChatModule mod;
        if (CF_Modules<ExpansionGlobalChatModule>.Get(mod))
        {
            string body = msg;
            ExpansionChatMessageEventParams data = new ExpansionChatMessageEventParams(
                ExpansionChatChannels.CCGlobal, "", body, "", "");
            auto rpc = mod.Expansion_CreateRPC("RPC_AddChatMessage");
            rpc.Write(data);
            if (onlyTo && onlyTo.GetIdentity())
                rpc.Expansion_Send(true, onlyTo.GetIdentity());
            else
                rpc.Expansion_Send(true);
            TakaroLog.Info("BROADCAST (chat): " + msg);
            return;
        }
#endif
        if (onlyTo)
        {
            NotificationSystem.SendNotificationToPlayerExtended(onlyTo, 8.0, "Discord", msg, "");
        }
        else
        {
            array<Man> players = new array<Man>;
            GetGame().GetPlayers(players);
            for (int i = 0; i < players.Count(); i++)
            {
                PlayerBase pb = PlayerBase.Cast(players[i]);
                if (!pb) continue;
                NotificationSystem.SendNotificationToPlayerExtended(pb, 8.0, "Discord", msg, "");
            }
        }
        TakaroLog.Info("BROADCAST (popup): " + msg);
    }

    // Loud center-screen banner shown to every connected player. Uses
    // Expansion's BAGUETTE notification when Expansion is loaded; falls
    // back to popup notifications otherwise.
    void BroadcastBanner(string title, string msg)
    {
#ifdef EXPANSIONMOD
        // Use Expansion's built-in "Info" icon so the banner doesn't render
        // with an empty white box on the left edge.
        ExpansionNotification(title, msg, EXPANSION_NOTIFICATION_ICON_INFO, COLOR_EXPANSION_NOTIFICATION_INFO, 8, ExpansionNotificationType.BAGUETTE).Create(NULL);
        TakaroLog.Info("ANNOUNCE (banner): " + title + " — " + msg);
        return;
#endif
        array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        for (int i = 0; i < players.Count(); i++)
        {
            PlayerBase pb = PlayerBase.Cast(players[i]);
            if (!pb) continue;
            NotificationSystem.SendNotificationToPlayerExtended(pb, 10.0, title, msg, "");
        }
        TakaroLog.Info("ANNOUNCE (popup): " + title + " — " + msg);
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
