// Routes Expansion chat into the Takaro bridge.
//
// ExpansionGlobalChatModule receives every chat message that flows through
// Expansion's chat system. Server-side, AddChatMessage_Server is the entry
// point with the sender's PlayerIdentity already attached. Override it,
// pass-through to the original behaviour, then mirror the message into
// TakaroChatRouter.Route() so the Takaro bridge enqueues a chat-message
// event for the dashboard.
//
// Channel mapping:
//   ExpansionChatChannels.GLOBAL   -> "global"
//   ExpansionChatChannels.DIRECT   -> "direct"
//   ExpansionChatChannels.TRANSPORT-> "vehicle"
//   ExpansionChatChannels.PARTY    -> "party"
//   ExpansionChatChannels.ADMIN    -> "admin"
//   anything else                  -> "global"

modded class ExpansionGlobalChatModule
{
    override void AddChatMessage_Server(
        PlayerIdentity sender, Object target,
        ParamsReadContext ctx, ExpansionChatMessageEventParams data)
    {
        super.AddChatMessage_Server(sender, target, ctx, data);

        if (!sender) return;
        if (!data) return;

        string channel = ExpansionChannelToString(data.param1);
        // data.param2 is the player's display name, data.param3 the text.
        string text = data.param3;
        if (text == "") return;

        TakaroChatRouter.Route(sender, channel, text);
    }

    private string ExpansionChannelToString(int channelId)
    {
        // ExpansionChatChannels enum values vary slightly between Expansion
        // versions — match by enum name through string comparison if the
        // direct numeric values shift.
        if (channelId == ExpansionChatChannels.GLOBAL) return "global";
        if (channelId == ExpansionChatChannels.DIRECT) return "direct";
        if (channelId == ExpansionChatChannels.TRANSPORT) return "vehicle";
        if (channelId == ExpansionChatChannels.PARTY) return "party";
        if (channelId == ExpansionChatChannels.ADMIN) return "admin";
        return "global";
    }
}
