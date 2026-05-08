// modded class must live in the same script pass as the original class
// definition. ExpansionGlobalChatModule is declared in 4_World, so this hook
// also lives in 4_World. We can't reference TakaroChatRouter (5_Mission) from
// here, so we go through TakaroBridgeAccessor — the same cross-pass holder
// pattern PlayerBaseTakaro.c uses to reach the bridge from 4_World.
#ifdef EXPANSIONMOD
modded class ExpansionGlobalChatModule
{
    override void AddChatMessage_Server(
        PlayerIdentity sender, Object target,
        ParamsReadContext ctx, ExpansionChatMessageEventParams data)
    {
        // Intercept Takaro chat commands BEFORE super broadcasts the message.
        // If the trimmed message starts with '/' we treat it as a command:
        // forward it to Takaro for processing and skip super entirely so it
        // never hits the chat RPC (other players don't see the command text)
        // and never fires g_Game.GetMission().OnEvent (so the vanilla OnEvent
        // hook in MissionServerTakaro doesn't double-forward to Takaro).
        if (sender && data && data.param3 != "")
        {
            string trimmed = data.param3;
            trimmed.TrimInPlace();
            if (trimmed != "" && trimmed.IndexOf("/") == 0)
            {
                // Whisper an immediate ack so the player sees their command
                // was received even if Takaro's response is delayed or the
                // command isn't registered. Routed via CCTransport because
                // its TransportChatColor (#FFCE09) is the closest preset
                // to "gold" in this server's ChatSettings — SystemChatColor
                // (#EB45EB) renders bright magenta on this profile, so it's
                // the wrong knob to turn for an ack.
                int sp = trimmed.IndexOf(" ");
                string verb = trimmed;
                if (sp > 0) verb = trimmed.Substring(0, sp);
                ExpansionChatMessageEventParams ackData =
                    new ExpansionChatMessageEventParams(
                        ExpansionChatChannels.CCTransport, "", "command received " + verb, "", "");
                auto ackRpc = Expansion_CreateRPC("RPC_AddChatMessage");
                ackRpc.Write(ackData);
                ackRpc.Expansion_Send(true, sender);

                TakaroBridge bridge = TakaroBridge.Cast(TakaroBridgeAccessor.Get());
                if (bridge)
                    bridge.OnChatMessage(sender, "global", trimmed);
                return;
            }
        }

        super.AddChatMessage_Server(sender, target, ctx, data);
    }
}
#endif
