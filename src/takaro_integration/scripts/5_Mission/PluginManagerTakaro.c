// Best-effort chat capture.
//
// Vanilla DayZ does not expose a clean server-side "chat sent" hook in script —
// chat is routed through the engine's ChatPlayer / chat RPC pipeline.
// CommunityFramework adds a global event we can subscribe to; if CF isn't
// loaded we fall back to a no-op.
//
// We declare a TakaroChatRouter that other code can call directly. The CF
// listener (if active) will hand chat lines here. Mods that already have a
// handle on chat (Expansion, VPP) can also call TakaroChatRouter.Route().
//
// The class is server-side only; it's compiled into the missionScriptModule
// which runs on the server when this mod is loaded with -serverMod=.

class TakaroChatRouter
{
    static void Route(PlayerIdentity sender, string channel, string text)
    {
        if (!sender) return;
        if (text == "") return;

        TakaroBridge bridge = TakaroBridge.Cast(TakaroBridgeAccessor.Get());
        if (!bridge) return;

        bridge.OnChatMessage(sender, channel, text);
    }
}

// Optional CF integration. If CommunityFramework is loaded, its CF_OnChatMessage
// global event signature can be subscribed to from here. We probe at OnInit
// time and bind only if the CF symbol exists, to avoid a hard dependency on CF
// which would break loading on servers that don't have it.

class TakaroCFBridge
{
    static bool s_BoundChatHook;

    static void TryBindChatHook()
    {
        if (s_BoundChatHook) return;

        // Probing for CF_OnChatMessage at script-load time without a hard
        // include is awkward in Enforce Script. The cleanest approach is to
        // ship a separate optional CF compatibility addon that depends on
        // @CF and explicitly imports CF's chat event class. In this minimal
        // baseline we leave the binding as a TODO and rely on whichever
        // chat-emitting mod (CF/VPP/Expansion) calls TakaroChatRouter.Route
        // directly.
        s_BoundChatHook = true;
        TakaroLog.Debug("CF chat hook binding deferred to compatibility addon");
    }
}
