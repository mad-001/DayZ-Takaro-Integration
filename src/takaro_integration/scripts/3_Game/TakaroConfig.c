// Persistent configuration for the Takaro bridge.
// Stored at: $profile:TakaroIntegration/config.json
// Loaded on server start, regenerated with defaults if missing.

class TakaroConfigData
{
    string TakaroApiUrl;            // e.g. "https://api.takaro.io"
    string IdentityToken;           // Takaro identity token (long-lived)
    string RegistrationToken;       // Takaro registration token (used once)
    string GameServerId;            // Filled in by Takaro after registration
    int CommandPollIntervalMs;      // How often to poll for outbound commands
    int EventBatchIntervalMs;       // How often to flush queued events
    int MaxEventsPerBatch;          // Max events sent per flush
    int RequestTimeoutSeconds;      // HTTP request timeout
    bool LogVerbose;                // Verbose logging to RPT
    bool DryRun;                    // If true, log outbound calls instead of sending

    void TakaroConfigData()
    {
        TakaroApiUrl = "https://api.takaro.io";
        IdentityToken = "";
        RegistrationToken = "";
        GameServerId = "";
        CommandPollIntervalMs = 2000;
        EventBatchIntervalMs = 1000;
        MaxEventsPerBatch = 50;
        RequestTimeoutSeconds = 10;
        LogVerbose = false;
        DryRun = false;
    }
}

class TakaroConfig
{
    static const string CONFIG_DIR = "$profile:TakaroIntegration";
    static const string CONFIG_FILE = "$profile:TakaroIntegration/config.json";

    static ref TakaroConfigData m_Data;

    static TakaroConfigData Get()
    {
        if (!m_Data)
            Load();
        return m_Data;
    }

    static void Load()
    {
        if (!FileExist(CONFIG_DIR))
            MakeDirectory(CONFIG_DIR);

        if (FileExist(CONFIG_FILE))
        {
            m_Data = new TakaroConfigData();
            JsonFileLoader<TakaroConfigData>.JsonLoadFile(CONFIG_FILE, m_Data);
            TakaroLog.Info("Loaded config from " + CONFIG_FILE);
        }
        else
        {
            m_Data = new TakaroConfigData();
            Save();
            TakaroLog.Warn("No config found — wrote defaults to " + CONFIG_FILE + ". Edit it and restart.");
        }
    }

    static void Save()
    {
        if (!m_Data)
            return;
        JsonFileLoader<TakaroConfigData>.JsonSaveFile(CONFIG_FILE, m_Data);
    }

    static bool IsConfigured()
    {
        TakaroConfigData d = Get();
        if (!d) return false;
        if (d.TakaroApiUrl == "") return false;
        // Either an identity token or a registration token is required to bootstrap.
        if (d.IdentityToken == "" && d.RegistrationToken == "") return false;
        return true;
    }
}

class TakaroLog
{
    static void Info(string msg)
    {
        Print("[Takaro] " + msg);
    }

    static void Warn(string msg)
    {
        Print("[Takaro][WARN] " + msg);
    }

    static void Error(string msg)
    {
        Print("[Takaro][ERROR] " + msg);
    }

    static void Debug(string msg)
    {
        TakaroConfigData d = TakaroConfig.Get();
        if (d && d.LogVerbose)
            Print("[Takaro][DBG] " + msg);
    }
}
