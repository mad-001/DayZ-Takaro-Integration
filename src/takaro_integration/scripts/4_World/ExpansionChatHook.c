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
        super.AddChatMessage_Server(sender, target, ctx, data);

        if (!sender) return;
        if (!data) return;

        string text = data.param3;
        if (text == "") return;

        TakaroBridge bridge = TakaroBridge.Cast(TakaroBridgeAccessor.Get());
        if (!bridge) return;

        // Takaro's ChatChannel enum only accepts global|team|friends|whisper —
        // collapse every Expansion channel to "global" so events validate.
        bridge.OnChatMessage(sender, "global", text);
    }
}
#endif
