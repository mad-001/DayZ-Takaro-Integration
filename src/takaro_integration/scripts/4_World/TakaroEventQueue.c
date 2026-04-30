// Outbound event queue. Events are enqueued from gameplay code,
// flushed to Takaro on a timer in batches, and dropped on overflow.

class TakaroEventPlayer
{
    string gameId;          // Stable internal ID (Steam64)
    string name;
    string steamId;
    string ip;
    int ping;
}

class TakaroEventPosition
{
    float x;
    float y;
    float z;
}

// Single canonical event payload. We send only the fields that apply
// for a given event type — JsonSerializer in Enforce Script ignores nulls
// only if they are explicitly unset, so all fields are cleared on construct.
class TakaroEvent
{
    string type;            // GameEvents enum: player-connected, etc.
    string timestamp;       // ISO-8601 UTC
    string msg;             // chat-message body
    string channel;         // chat channel
    ref TakaroEventPlayer player;
    ref TakaroEventPlayer recipient;
    ref TakaroEventPlayer attacker;
    ref TakaroEventPosition position;
    string weapon;
    string entity;
    string raw;             // raw log line, only set for LOG_LINE
}

class TakaroEventBatch
{
    ref array<ref TakaroEvent> events;

    void TakaroEventBatch()
    {
        events = new array<ref TakaroEvent>;
    }
}

class TakaroEventQueue
{
    private ref array<ref TakaroEvent> m_Pending;
    private int m_MaxQueueLength;
    private int m_DroppedCount;

    void TakaroEventQueue(int maxQueueLength = 1000)
    {
        m_Pending = new array<ref TakaroEvent>;
        m_MaxQueueLength = maxQueueLength;
        m_DroppedCount = 0;
    }

    void Enqueue(TakaroEvent ev)
    {
        if (!ev) return;
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

    // Take up to `max` events out of the queue and return them as a new array.
    // Caller is responsible for sending them; on failure they should re-enqueue.
    array<ref TakaroEvent> Drain(int max)
    {
        array<ref TakaroEvent> drained = new array<ref TakaroEvent>;
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

    // Re-insert a batch at the head if a flush failed.
    void Requeue(array<ref TakaroEvent> events)
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
        // DayZ has GetYearMonthDay/GetHourMinuteSecond on GetGame() — use UTC-ish.
        int y, mo, d, h, mi, s;
        GetGame().GetWorld().GetDate(y, mo, d, h, mi);
        s = 0;
        // Format: YYYY-MM-DDTHH:MM:SSZ
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

    static TakaroEventPlayer PlayerFrom(PlayerIdentity id, PlayerBase pb = null)
    {
        TakaroEventPlayer p = new TakaroEventPlayer();
        if (id)
        {
            p.gameId = id.GetPlainId();
            p.name = id.GetName();
            p.steamId = id.GetPlainId();
            p.ping = id.GetPingAct();
        }
        else if (pb && pb.GetIdentity())
        {
            return PlayerFrom(pb.GetIdentity(), pb);
        }
        return p;
    }

    static TakaroEvent Connected(PlayerIdentity id)
    {
        TakaroEvent ev = new TakaroEvent();
        ev.type = "player-connected";
        ev.timestamp = NowIso();
        ev.player = PlayerFrom(id);
        return ev;
    }

    static TakaroEvent Disconnected(PlayerIdentity id, PlayerBase pb)
    {
        TakaroEvent ev = new TakaroEvent();
        ev.type = "player-disconnected";
        ev.timestamp = NowIso();
        ev.player = PlayerFrom(id, pb);
        return ev;
    }

    static TakaroEvent Chat(PlayerIdentity id, string channel, string msg)
    {
        TakaroEvent ev = new TakaroEvent();
        ev.type = "chat-message";
        ev.timestamp = NowIso();
        ev.player = PlayerFrom(id);
        ev.channel = channel;
        ev.msg = msg;
        return ev;
    }

    static TakaroEvent Death(PlayerBase victim, EntityAI killer, string weapon)
    {
        TakaroEvent ev = new TakaroEvent();
        ev.type = "player-death";
        ev.timestamp = NowIso();
        ev.player = PlayerFrom(null, victim);

        if (killer && killer.IsInherited(PlayerBase))
        {
            PlayerBase killerPlayer = PlayerBase.Cast(killer);
            ev.attacker = PlayerFrom(null, killerPlayer);
        }

        if (victim)
        {
            vector pos = victim.GetPosition();
            TakaroEventPosition tp = new TakaroEventPosition();
            tp.x = pos[0];
            tp.y = pos[1];
            tp.z = pos[2];
            ev.position = tp;
        }
        ev.weapon = weapon;
        return ev;
    }

    static TakaroEvent LogLine(string raw)
    {
        TakaroEvent ev = new TakaroEvent();
        ev.type = "log";
        ev.timestamp = NowIso();
        ev.raw = raw;
        return ev;
    }
}
