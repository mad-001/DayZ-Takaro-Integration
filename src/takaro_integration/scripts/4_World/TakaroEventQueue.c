// Outbound event queue. Each entry is a pre-built JSON string for one event,
// containing only the fields valid for that event type. We build per-type to
// avoid Enforce JsonSerializer's behavior of emitting every class field
// (including empty strings), which makes Takaro's DTO whitelist validators
// reject events like player-connected when an empty `channel` field is shipped.

class TakaroEventQueue
{
    private ref array<string> m_Pending;
    private int m_MaxQueueLength;
    private int m_DroppedCount;

    void TakaroEventQueue(int maxQueueLength = 1000)
    {
        m_Pending = new array<string>;
        m_MaxQueueLength = maxQueueLength;
        m_DroppedCount = 0;
    }

    void Enqueue(string ev)
    {
        if (ev == "") return;
        if (m_Pending.Count() >= m_MaxQueueLength)
        {
            m_DroppedCount++;
            if (m_DroppedCount % 100 == 1)
                TakaroLog.Warn("Event queue full — dropped " + m_DroppedCount.ToString() + " events total");
            return;
        }
        m_Pending.Insert(ev);
    }

    int Count()
    {
        return m_Pending.Count();
    }

    array<string> Drain(int max)
    {
        array<string> drained = new array<string>;
        int n = Math.Min(max, m_Pending.Count());
        for (int i = 0; i < n; i++)
            drained.Insert(m_Pending[i]);
        if (n > 0)
        {
            for (int j = 0; j < n; j++)
                m_Pending.Remove(0);
        }
        return drained;
    }

    void Requeue(array<string> events)
    {
        if (!events) return;
        for (int i = events.Count() - 1; i >= 0; i--)
            m_Pending.InsertAt(events[i], 0);
    }
}

class TakaroEventFactory
{
    static string NowIso()
    {
        int y, mo, d, h, mi;
        GetGame().GetWorld().GetDate(y, mo, d, h, mi);
        int s = 0;
        return string.Format("%1-%2-%3T%4:%5:%6Z",
            y.ToString(),
            PadInt(mo), PadInt(d),
            PadInt(h), PadInt(mi), PadInt(s));
    }

    static string PadInt(int v)
    {
        if (v < 10) return "0" + v.ToString();
        return v.ToString();
    }

    // Strip double-quotes from user-controlled strings to keep our naive JSON
    // valid. Same trick the command dispatcher uses; backslash escapes in
    // string literals confuse Enforce's CParser.
    static string Safe(string s)
    {
        string out_s = s;
        string dq = "\"";
        out_s.Replace(dq, "'");
        return out_s;
    }

    // Produce the JSON for an IPlayerReferenceDTO. We fall back to PlayerBase
    // if no identity is available (covers post-disconnect lookups).
    //
    // ID semantics in DayZ:
    //   GetPlainId()  -> Steam64 ("76561198..."), the player's Steam account ID
    //   GetId()       -> BIS hash (base64-ish), the in-game DayZ UID
    // Takaro's platformId pattern is ^[A-Za-z0-9_-]+:[A-Za-z0-9_-]+$, so we
    // strip trailing '=' base64 padding from the BIS hash before composing it.
    static string PlayerJson(PlayerIdentity id, PlayerBase pb)
    {
        if (!id && pb) id = pb.GetIdentity();
        if (!id) return "null";
        string q = "\"";
        string sid = id.GetPlainId();
        string bisid = id.GetId();
        int eqIdx = bisid.IndexOf("=");
        if (eqIdx >= 0) bisid = bisid.Substring(0, eqIdx);
        string name = Safe(id.GetName());
        int ping = id.GetPingAct();
        string s = "{";
        s += q + "gameId" + q + ":" + q + sid + q + ",";
        s += q + "name" + q + ":" + q + name + q + ",";
        s += q + "steamId" + q + ":" + q + sid + q + ",";
        s += q + "platformId" + q + ":" + q + "dayz:" + bisid + q + ",";
        s += q + "ping" + q + ":" + ping.ToString();
        s += "}";
        return s;
    }

    static string Connected(PlayerIdentity id)
    {
        string q = "\"";
        string s = "{";
        s += q + "type" + q + ":" + q + "player-connected" + q + ",";
        s += q + "timestamp" + q + ":" + q + NowIso() + q + ",";
        s += q + "player" + q + ":" + PlayerJson(id, null);
        s += "}";
        return s;
    }

    // For disconnect, identity/player may be null by the time we're called
    // (vanilla MissionServer cleans them up before our hook in some paths).
    // Fall back to building a minimal player JSON from the BIS uid alone so
    // Takaro's DTO validation still passes.
    static string Disconnected(PlayerIdentity id, PlayerBase pb, string uid)
    {
        string q = "\"";
        string s = "{";
        s += q + "type" + q + ":" + q + "player-disconnected" + q + ",";
        s += q + "timestamp" + q + ":" + q + NowIso() + q + ",";
        if (id || pb)
        {
            s += q + "player" + q + ":" + PlayerJson(id, pb);
        }
        else
        {
            string bis = uid;
            int eqIdx = bis.IndexOf("=");
            if (eqIdx >= 0) bis = bis.Substring(0, eqIdx);
            s += q + "player" + q + ":{";
            s += q + "gameId" + q + ":" + q + bis + q + ",";
            s += q + "name" + q + ":" + q + q + ",";
            s += q + "platformId" + q + ":" + q + "dayz:" + bis + q;
            s += "}";
        }
        s += "}";
        return s;
    }

    static string Chat(PlayerIdentity id, string channel, string msg)
    {
        string q = "\"";
        string s = "{";
        s += q + "type" + q + ":" + q + "chat-message" + q + ",";
        s += q + "timestamp" + q + ":" + q + NowIso() + q + ",";
        s += q + "channel" + q + ":" + q + Safe(channel) + q + ",";
        s += q + "msg" + q + ":" + q + Safe(msg) + q + ",";
        s += q + "player" + q + ":" + PlayerJson(id, null);
        s += "}";
        return s;
    }

    static string Death(PlayerBase victim, EntityAI killer, string weapon)
    {
        // EventPlayerDeath has no `weapon` field (that's on EventEntityKilled).
        // Fold the weapon name into the optional `msg` so it isn't lost.
        string q = "\"";
        string s = "{";
        s += q + "type" + q + ":" + q + "player-death" + q + ",";
        s += q + "timestamp" + q + ":" + q + NowIso() + q + ",";
        s += q + "player" + q + ":" + PlayerJson(null, victim);
        if (killer && killer.IsInherited(PlayerBase))
        {
            PlayerBase kp = PlayerBase.Cast(killer);
            s += "," + q + "attacker" + q + ":" + PlayerJson(null, kp);
        }
        if (victim)
        {
            vector p = victim.GetPosition();
            s += "," + q + "position" + q + ":{";
            s += q + "x" + q + ":" + p[0].ToString() + ",";
            s += q + "y" + q + ":" + p[1].ToString() + ",";
            s += q + "z" + q + ":" + p[2].ToString();
            s += "}";
        }
        if (weapon != "")
            s += "," + q + "msg" + q + ":" + q + "killed with " + Safe(weapon) + q;
        s += "}";
        return s;
    }

    static string LogLine(string raw)
    {
        // EventLogLine inherits BaseGameEvent.msg — there is no `raw` field.
        string q = "\"";
        string s = "{";
        s += q + "type" + q + ":" + q + "log" + q + ",";
        s += q + "timestamp" + q + ":" + q + NowIso() + q + ",";
        s += q + "msg" + q + ":" + q + Safe(raw) + q;
        s += "}";
        return s;
    }
}
