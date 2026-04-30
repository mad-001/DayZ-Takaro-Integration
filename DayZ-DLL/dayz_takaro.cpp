// DayZ Takaro Integration — server-side native DLL.
//
// Loaded into DayZServer_x64.exe via the Secur32.dll proxy alongside this DLL.
// Once loaded, opens a WebSocket to wss://connect.takaro.io/, identifies with
// the registration token from takaro_config.txt, and tails the DayZ server's
// RPT log to forward player events as Takaro `gameEvent` messages.
//
// Adapted from the Enshrouded reference implementation (takaro_no_hooks.cpp).
// The WS protocol code is identical because Takaro's protocol is the same
// across all games — only the log-parsing and event-extraction differ.

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <map>
#include <vector>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <regex>

#pragma comment(lib, "winhttp.lib")

// ---- Tiny JSON builder -----------------------------------------------

class SimpleJSON {
public:
    static std::string escape(const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '"' || c == '\\') r += '\\';
            r += c;
        }
        return r;
    }
    static std::string object(const std::string& content) { return "{" + content + "}"; }
    static std::string pair(const std::string& k, const std::string& v) {
        return "\"" + escape(k) + "\":\"" + escape(v) + "\"";
    }
    static std::string objectPair(const std::string& k, const std::string& v) {
        return "\"" + escape(k) + "\":" + v;
    }
};

struct PlayerInfo {
    std::string steamId;
    std::string name;
    std::string gameId;
};

// ---- Main integration class ------------------------------------------

class TakaroDayZ {
private:
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hWebSocket = nullptr;
    std::thread wsThread;
    std::thread logMonitorThread;
    std::mutex playerMutex;
    std::map<std::string, PlayerInfo> players; // steamId -> PlayerInfo
    bool running = false;
    bool connected = false;
    std::string registrationToken;
    std::string serverName = "DayZ Server";
    std::string identityToken;
    std::string gameServerId;
    std::string profilesDir = "profiles";
    std::ofstream logFile;
    std::streampos lastRptPosition = 0;
    std::string currentRptPath;

    void Log(const std::string& msg) {
        if (!logFile.is_open()) return;
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        std::tm tm; localtime_s(&tm, &t);
        logFile << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.'
                << std::setfill('0') << std::setw(3) << ms.count()
                << " " << msg << std::endl;
        logFile.flush();
    }

    void LoadConfig() {
        std::ifstream cfg("takaro_config.txt");
        if (!cfg.is_open()) {
            // Fall back to absolute next-to-DLL path: DllMain set CWD to DayZ root.
            return;
        }
        std::string line;
        while (std::getline(cfg, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            // strip CRLF
            while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
            if (key == "registrationToken") registrationToken = val;
            else if (key == "serverName") serverName = val;
            else if (key == "identityToken") identityToken = val;
            else if (key == "profilesDir") profilesDir = val;
        }
        if (identityToken.empty()) identityToken = serverName;
    }

    // ---- WebSocket plumbing (lifted from Enshrouded reference) -------

    void CloseHandles() {
        if (hWebSocket) {
            WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(hWebSocket);
            hWebSocket = nullptr;
        }
        if (hConnect) { WinHttpCloseHandle(hConnect); hConnect = nullptr; }
        if (hSession) { WinHttpCloseHandle(hSession); hSession = nullptr; }
    }

    bool ConnectToTakaro() {
        hSession = WinHttpOpen(L"DayZ-Takaro/0.1",
                               WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return false;

        hConnect = WinHttpConnect(hSession, L"connect.takaro.io", 443, 0);
        if (!hConnect) { CloseHandles(); return false; }

        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", L"/", nullptr,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
        if (!hReq) { CloseHandles(); return false; }

        if (!WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
            WinHttpCloseHandle(hReq); CloseHandles(); return false;
        }
        if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            WinHttpCloseHandle(hReq); CloseHandles(); return false;
        }
        if (!WinHttpReceiveResponse(hReq, nullptr)) {
            WinHttpCloseHandle(hReq); CloseHandles(); return false;
        }

        hWebSocket = WinHttpWebSocketCompleteUpgrade(hReq, 0);
        WinHttpCloseHandle(hReq);
        return hWebSocket != nullptr;
    }

    void SendRaw(const std::string& msg) {
        if (!hWebSocket) return;
        DWORD r = WinHttpWebSocketSend(hWebSocket,
                                       WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                       (PVOID)msg.c_str(), (DWORD)msg.length());
        if (r != ERROR_SUCCESS) Log("[Takaro] Send error: " + std::to_string(r));
    }

    void SendIdentify() {
        std::string msg = SimpleJSON::object(
            SimpleJSON::pair("type", "identify") + "," +
            SimpleJSON::objectPair("payload", SimpleJSON::object(
                SimpleJSON::pair("identityToken", identityToken) + "," +
                SimpleJSON::pair("registrationToken", registrationToken) + "," +
                SimpleJSON::pair("name", serverName)
            ))
        );
        SendRaw(msg);
    }

    std::string ExtractField(const std::string& m, const std::string& field) {
        std::string s = "\"" + field + "\":\"";
        size_t p = m.find(s);
        if (p == std::string::npos) return "";
        p += s.length();
        size_t e = m.find("\"", p);
        return e == std::string::npos ? "" : m.substr(p, e - p);
    }

    // ---- Sending events ----------------------------------------------

    std::string PlayerObject(const PlayerInfo& p) {
        return SimpleJSON::object(
            SimpleJSON::pair("gameId", p.gameId) + "," +
            SimpleJSON::pair("name", p.name) + "," +
            SimpleJSON::pair("platformId", "steam:" + p.steamId) + "," +
            SimpleJSON::pair("steamId", p.steamId)
        );
    }

    void SendPlayerEvent(const std::string& evType, const PlayerInfo& p) {
        if (!connected || !hWebSocket) return;
        std::string playerObj = PlayerObject(p);
        std::string msg = SimpleJSON::object(
            SimpleJSON::pair("type", "gameEvent") + "," +
            "\"payload\":{" +
                "\"type\":\"" + evType + "\"," +
                "\"data\":{\"player\":" + playerObj + "}" +
            "}"
        );
        SendRaw(msg);
        Log("[Takaro] " + evType + " for " + p.name + " (" + p.steamId + ")");
    }

    // ---- DayZ RPT log parsing ----------------------------------------
    //
    // DayZ writes a per-boot RPT file at $profile\DayZServer_x64_<timestamp>.RPT.
    // Player connect lines look like:
    //   12:34:56 Player "Mad" (id=ABCDEF1234567890... beid=...) connected
    //   12:34:56 Player "Mad" (id=...) has been disconnected
    // and on death:
    //   12:34:56 Player "Mad" (DEAD) (id=...) ...
    //
    // The "id" field is a hash, not a Steam ID. To get Steam IDs we'd need
    // BE RCON or a hooked function — for v0 we use the id field as gameId.

    std::string FindLatestRpt() {
        std::string pattern = profilesDir + "\\DayZServer_x64_*.RPT";
        WIN32_FIND_DATAA data;
        HANDLE h = FindFirstFileA(pattern.c_str(), &data);
        if (h == INVALID_HANDLE_VALUE) return "";
        FILETIME bestTime = {};
        std::string best;
        do {
            if (best.empty() || CompareFileTime(&data.ftLastWriteTime, &bestTime) > 0) {
                bestTime = data.ftLastWriteTime;
                best = profilesDir + "\\" + data.cFileName;
            }
        } while (FindNextFileA(h, &data));
        FindClose(h);
        return best;
    }

    // Pull Player "NAME" out of an RPT line.
    std::string ExtractRptPlayerName(const std::string& line) {
        size_t p = line.find("Player \"");
        if (p == std::string::npos) return "";
        p += 8;
        size_t e = line.find('"', p);
        if (e == std::string::npos) return "";
        return line.substr(p, e - p);
    }

    // Pull id=XXXX (hex up to the next space/) ) out of an RPT line.
    std::string ExtractRptId(const std::string& line) {
        size_t p = line.find("id=");
        if (p == std::string::npos) return "";
        p += 3;
        size_t e = p;
        while (e < line.size() && line[e] != ' ' && line[e] != ')' && line[e] != ',') e++;
        return line.substr(p, e - p);
    }

    void HandleRptLine(const std::string& line) {
        // Player connected.
        if (line.find(") connected") != std::string::npos &&
            line.find("Player \"") != std::string::npos) {
            std::string name = ExtractRptPlayerName(line);
            std::string id = ExtractRptId(line);
            if (name.empty() || id.empty()) return;
            PlayerInfo p; p.steamId = id; p.gameId = id; p.name = name;
            { std::lock_guard<std::mutex> g(playerMutex); players[id] = p; }
            SendPlayerEvent("player-connected", p);
            return;
        }
        // Player disconnected.
        if (line.find("disconnected") != std::string::npos &&
            line.find("Player \"") != std::string::npos) {
            std::string name = ExtractRptPlayerName(line);
            std::string id = ExtractRptId(line);
            if (id.empty()) return;
            PlayerInfo p;
            {
                std::lock_guard<std::mutex> g(playerMutex);
                auto it = players.find(id);
                if (it == players.end()) return;
                p = it->second;
                players.erase(it);
            }
            SendPlayerEvent("player-disconnected", p);
            return;
        }
    }

    void MonitorRpt() {
        Log("[Takaro] RPT monitor starting; profiles dir = " + profilesDir);

        // Find latest RPT and seek to end so we ignore boot noise.
        std::string rpt = FindLatestRpt();
        if (rpt.empty()) {
            Log("[Takaro] WARN: no RPT file found in " + profilesDir);
        } else {
            currentRptPath = rpt;
            std::ifstream f(rpt, std::ios::binary);
            if (f) {
                f.seekg(0, std::ios::end);
                lastRptPosition = f.tellg();
                Log("[Takaro] Tailing " + rpt + " from offset " + std::to_string(lastRptPosition));
            }
        }

        while (running) {
            // Re-discover RPT periodically — DayZ rotates per boot.
            std::string latest = FindLatestRpt();
            if (!latest.empty() && latest != currentRptPath) {
                currentRptPath = latest;
                lastRptPosition = 0;
                Log("[Takaro] RPT rotated to " + latest);
            }

            if (!currentRptPath.empty()) {
                std::ifstream f(currentRptPath, std::ios::binary);
                if (f) {
                    f.seekg(0, std::ios::end);
                    auto size = f.tellg();
                    if (size < lastRptPosition) lastRptPosition = 0; // truncated
                    if (size > lastRptPosition) {
                        f.seekg(lastRptPosition);
                        std::string line;
                        while (std::getline(f, line)) {
                            // Strip CR
                            if (!line.empty() && line.back() == '\r') line.pop_back();
                            HandleRptLine(line);
                        }
                        lastRptPosition = size;
                    }
                }
            }
            Sleep(1000);
        }
    }

    // ---- Inbound Takaro request handlers ------------------------------

    void SendResponse(const std::string& requestId, const std::string& payloadJson) {
        std::string msg = SimpleJSON::object(
            SimpleJSON::pair("type", "response") + "," +
            SimpleJSON::pair("requestId", requestId) + "," +
            "\"payload\":" + payloadJson
        );
        SendRaw(msg);
    }

    void SendTestReachability(const std::string& requestId) {
        SendResponse(requestId, "{\"connectable\":true,\"reason\":null}");
    }

    void SendGetPlayers(const std::string& requestId) {
        std::lock_guard<std::mutex> g(playerMutex);
        std::string arr = "[";
        bool first = true;
        for (const auto& [k, p] : players) {
            if (!first) arr += ",";
            first = false;
            arr += PlayerObject(p);
        }
        arr += "]";
        SendResponse(requestId, arr);
    }

    void SendPlayerLocation(const std::string& requestId) {
        SendResponse(requestId, "{\"x\":0.0,\"y\":0.0,\"z\":0.0}");
    }

    void SendPlayerInventory(const std::string& requestId) {
        SendResponse(requestId, "[]");
    }

    void SendExecuteCommand(const std::string& requestId, const std::string& message) {
        // Echo command back as ack — actual RCON dispatch is a v1 feature.
        std::string command;
        std::string key = "\\\"command\\\":\\\"";
        size_t p = message.find(key);
        if (p != std::string::npos) {
            p += key.length();
            size_t e = message.find("\\\"", p);
            if (e != std::string::npos) command = message.substr(p, e - p);
        }
        std::string payload = SimpleJSON::object(
            SimpleJSON::pair("rawResult", command) + ",\"success\":true"
        );
        SendResponse(requestId, payload);
    }

    void SendGenericFail(const std::string& requestId, const std::string& reason = "") {
        std::string payload = "{\"success\":false";
        if (!reason.empty()) payload += "," + SimpleJSON::pair("reason", reason);
        payload += "}";
        SendResponse(requestId, payload);
    }

    void HandleMessage(const std::string& m) {
        if (m.find("\"type\":\"error\"") != std::string::npos) {
            Log("[Takaro] error from server: " + m);
            return;
        }
        if (m.find("\"type\":\"connected\"") != std::string::npos) {
            Log("[Takaro] connected confirmed");
            return;
        }
        if (m.find("\"type\":\"identifyResponse\"") != std::string::npos) {
            if (m.find("\"error\"") != std::string::npos) {
                Log("[Takaro] identify FAILED: " + m);
            } else {
                gameServerId = ExtractField(m, "gameServerId");
                Log("[Takaro] identified, gameServerId=" + gameServerId);
            }
            return;
        }
        if (m.find("\"type\":\"request\"") != std::string::npos) {
            std::string requestId = ExtractField(m, "requestId");
            if (m.find("testReachability") != std::string::npos) SendTestReachability(requestId);
            else if (m.find("getPlayers") != std::string::npos) SendGetPlayers(requestId);
            else if (m.find("getPlayerLocation") != std::string::npos) SendPlayerLocation(requestId);
            else if (m.find("getPlayerInventory") != std::string::npos) SendPlayerInventory(requestId);
            else if (m.find("executeConsoleCommand") != std::string::npos) SendExecuteCommand(requestId, m);
            // List-type actions return arrays per Takaro DTOs.
            else if (m.find("\"action\":\"listItems\"") != std::string::npos
                  || m.find("\"action\":\"listEntities\"") != std::string::npos
                  || m.find("\"action\":\"listLocations\"") != std::string::npos
                  || m.find("\"action\":\"listBans\"") != std::string::npos)
                SendResponse(requestId, "[]");
            else SendGenericFail(requestId, "not implemented");
        }
    }

    void WebSocketThread() {
        while (running) {
            if (!connected) {
                if (ConnectToTakaro()) {
                    Log("[Takaro] WS open to wss://connect.takaro.io/");
                    SendIdentify();
                    connected = true;
                } else {
                    Log("[Takaro] connect failed; retrying in 5s");
                    Sleep(5000);
                    continue;
                }
            }
            BYTE buf[4096];
            DWORD bytes = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bt;
            DWORD r = WinHttpWebSocketReceive(hWebSocket, buf, sizeof(buf), &bytes, &bt);
            if (r == ERROR_SUCCESS && bytes > 0) {
                if (bt == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                    bt == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
                    HandleMessage(std::string((char*)buf, bytes));
                }
            } else if (r != ERROR_SUCCESS) {
                Log("[Takaro] receive error: " + std::to_string(r));
                connected = false;
                CloseHandles();
                Sleep(5000);
            }
            Sleep(10);
        }
    }

    std::string TimestampedLogPath() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm; localtime_s(&tm, &t);
        std::ostringstream os;
        CreateDirectoryA("logs", nullptr);
        CreateDirectoryA("logs\\TakaroLogs", nullptr);
        os << "logs\\TakaroLogs\\dayz-takaro-"
           << std::put_time(&tm, "%Y-%m-%d-%H-%M-%S")
           << ".log";
        return os.str();
    }

public:
    void Start() {
        logFile.open(TimestampedLogPath(), std::ios::out);
        Log("[Takaro] DayZ DLL initializing");
        LoadConfig();
        if (registrationToken.empty()) {
            Log("[Takaro] ERROR: takaro_config.txt missing or has no registrationToken");
            Log("[Takaro] Create takaro_config.txt next to DayZServer_x64.exe with:");
            Log("[Takaro]   registrationToken=YOUR_TOKEN");
            Log("[Takaro]   serverName=My Server");
            Log("[Takaro]   profilesDir=profiles");
            return;
        }
        Log("[Takaro] serverName=" + serverName + " profilesDir=" + profilesDir);

        running = true;
        wsThread = std::thread(&TakaroDayZ::WebSocketThread, this);
        logMonitorThread = std::thread(&TakaroDayZ::MonitorRpt, this);
        Log("[Takaro] threads started");
    }

    void Stop() {
        running = false;
        connected = false;
        CloseHandles();
        if (wsThread.joinable()) wsThread.join();
        if (logMonitorThread.joinable()) logMonitorThread.join();
        Log("[Takaro] stopped");
        if (logFile.is_open()) logFile.close();
    }
};

static TakaroDayZ* g_Takaro = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        g_Takaro = new TakaroDayZ();
        g_Takaro->Start();
        break;
    case DLL_PROCESS_DETACH:
        if (g_Takaro) {
            g_Takaro->Stop();
            delete g_Takaro;
            g_Takaro = nullptr;
        }
        break;
    }
    return TRUE;
}
