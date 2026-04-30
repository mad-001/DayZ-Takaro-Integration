// Optional companion addon for @TakaroIntegration. Loaded only when both this
// mod AND DayZ-Expansion-Bundle are running on the server. Adds chat-capture
// by hooking ExpansionGlobalChatModule.AddChatMessage_Server.
//
// Hard-depends on @TakaroIntegration (provides TakaroChatRouter) and on the
// Expansion chat module — DON'T load this on a server that doesn't have both.

class CfgPatches
{
    class TakaroIntegration_Expansion
    {
        units[] = {};
        weapons[] = {};
        requiredVersion = 0.1;
        requiredAddons[] = {"DZ_Data", "DZ_Scripts", "TakaroIntegration", "DayZExpansion_Chat_Scripts"};
    };
};

class CfgMods
{
    class TakaroIntegration_Expansion
    {
        type = "mod";
        dir = "TakaroIntegration_Expansion";
        name = "TakaroIntegration_Expansion";
        author = "mad-001";
        version = "0.1.0";
        dependencies[] = {"Mission"};

        class defs
        {
            class missionScriptModule
            {
                value = "";
                files[] = {"TakaroIntegration_Expansion/scripts/5_Mission"};
            };
        };
    };
};
