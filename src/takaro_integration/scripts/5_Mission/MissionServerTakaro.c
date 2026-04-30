// Hooks into MissionServer's lifecycle and the vanilla connect/disconnect path.
// We extend the existing MissionServer rather than replace it so other mods
// (CF, VPP, Expansion) keep working.

modded class MissionServer
{
    ref TakaroBridge m_TakaroBridge;
    static const float TAKARO_TICK_INTERVAL = 1.0;
    float m_TakaroTickAccum;

    override void OnInit()
    {
        super.OnInit();

        if (!m_TakaroBridge)
        {
            m_TakaroBridge = new TakaroBridge();
            m_TakaroBridge.Initialize();
            TakaroLog.Info("Bridge attached to MissionServer.OnInit");
        }
    }

    override void OnUpdate(float timeslice)
    {
        super.OnUpdate(timeslice);

        // We don't want to spam the bridge every frame; the bridge itself only
        // does work when its internal accumulators trip, so passing timeslice
        // every tick is fine. But we throttle here to be defensive.
        m_TakaroTickAccum += timeslice;
        if (m_TakaroTickAccum >= TAKARO_TICK_INTERVAL)
        {
            if (m_TakaroBridge)
                m_TakaroBridge.OnUpdate(m_TakaroTickAccum);
            m_TakaroTickAccum = 0;
        }
    }

    override void InvokeOnConnect(PlayerBase player, PlayerIdentity identity)
    {
        super.InvokeOnConnect(player, identity);
        if (m_TakaroBridge && identity)
            m_TakaroBridge.OnPlayerConnected(player, identity);
    }

    override void InvokeOnDisconnect(PlayerBase player)
    {
        if (m_TakaroBridge && player)
            m_TakaroBridge.OnPlayerDisconnected(player);
        super.InvokeOnDisconnect(player);
    }

    // Helper accessor so other modules can route events into the bridge.
    static TakaroBridge GetTakaroBridge()
    {
        MissionServer ms = MissionServer.Cast(GetGame().GetMission());
        if (!ms) return null;
        return ms.m_TakaroBridge;
    }
}
