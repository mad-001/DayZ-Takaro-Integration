// DayZ Takaro Integration — server-side native DLL.
//
// Loaded into DayZServer_x64.exe via the winmm.dll proxy. Two responsibilities:
//
//   1. WebSocket client to wss://connect.takaro.io/ — speaks Takaro's
//      identify/gameEvent/request/response protocol (WinHTTP).
//
//   2. Tiny HTTP server on 127.0.0.1:<port> for the companion @TakaroIntegration
//      Enforce Script mod to talk to. The script mod has access to game state
//      (PlayerIdentity.GetPlainId() for real Steam64, modded chat hooks, EEKilled
//      for deaths) — it POSTs events here and polls here for queued Takaro
//      requests. The DLL is the network bridge; the script mod is the data source.

// winsock2.h must precede windows.h, otherwise windows.h drags in the older
// winsock.h whose types collide with winsock2.h.
#include <winsock2.h>
#include <ws2tcpip.h>
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
#include <atomic>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

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

struct PendingOperation {
    std::string operationId;  // = WS requestId
    std::string action;
    std::string argsJson;
};

// Forward decl
class TakaroDayZ;

// ====================================================================
// Local HTTP server for the script mod to talk to.
// ====================================================================

class LocalHttpServer {
public:
    LocalHttpServer(TakaroDayZ* parent, int port);
    void Start();
    void Stop();

    void EnqueueOperation(const PendingOperation& op);

private:
    void AcceptLoop();
    void HandleClient(SOCKET client);
    std::string ReadHttpRequest(SOCKET sock, std::string& method, std::string& path, std::string& body);
    void WriteHttpResponse(SOCKET sock, int code, const std::string& body, const std::string& contentType = "application/json");

    bool RouteRegister(const std::string& body, std::string& outBody);
    bool RouteEvents(const std::string& body, std::string& outBody);
    bool RoutePoll(std::string& outBody);
    bool RouteResult(const std::string& path, const std::string& body, std::string& outBody);

    TakaroDayZ* m_parent;
    int m_port;
    SOCKET m_listen = INVALID_SOCKET;
    std::thread m_acceptThread;
    std::atomic<bool> m_running{false};

    std::mutex m_queueMutex;
    std::queue<PendingOperation> m_operations;
    std::map<std::string, std::string> m_opAction;  // opId -> action (for shape coercion on result)
};

// ====================================================================
// Main integration class
// ====================================================================

class TakaroDayZ {
public:
    void Start();
    void Stop();

    // Called from the local HTTP server when the script mod POSTs events.
    void ForwardEventToWs(const std::string& type, const std::string& dataJson);

    // Called when the script mod POSTs an operation result.
    void ForwardResponse(const std::string& opId, bool ok,
                        const std::string& resultJson, const std::string& errorMessage,
                        const std::string& action);

    // Cached identity for /register replies.
    std::string GetIdentityToken() const { return identityToken; }
    std::string GetGameServerId() const { return gameServerId; }

    void Log(const std::string& msg);

private:
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hWebSocket = nullptr;
    std::thread wsThread;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};

    std::string registrationToken;
    std::string serverName = "DayZ Server";
    std::string identityToken;
    std::string gameServerId;
    int localPort = 8089;

    std::ofstream logFile;
    std::mutex logMutex;

    LocalHttpServer* localServer = nullptr;

    void LoadConfig();
    void CloseHandles();
    bool ConnectToTakaro();
    void SendRaw(const std::string& msg);
    void SendIdentify();
    std::string ExtractField(const std::string& m, const std::string& field);
    void HandleMessage(const std::string& m);
    void WebSocketThread();
    std::string TimestampedLogPath();

    // Built-in handlers (no script mod needed):
    void HandleTestReachability(const std::string& requestId);
    void HandleListEmpty(const std::string& requestId);  // for listItems/listEntities/etc.

    // For non-builtin actions, hand off to the script mod.
    void QueueForScriptMod(const std::string& requestId, const std::string& action,
                           const std::string& argsJson);
};

// ====================================================================
// LocalHttpServer impl
// ====================================================================

LocalHttpServer::LocalHttpServer(TakaroDayZ* parent, int port)
    : m_parent(parent), m_port(port) {}

void LocalHttpServer::Start() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listen == INVALID_SOCKET) {
        m_parent->Log("[Takaro] HTTP socket() failed: " + std::to_string(WSAGetLastError()));
        return;
    }
    int reuse = 1;
    setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons((u_short)m_port);
    if (bind(m_listen, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        m_parent->Log("[Takaro] HTTP bind() failed on 127.0.0.1:" + std::to_string(m_port)
                      + " err=" + std::to_string(WSAGetLastError()));
        closesocket(m_listen);
        m_listen = INVALID_SOCKET;
        return;
    }
    if (listen(m_listen, 16) == SOCKET_ERROR) {
        m_parent->Log("[Takaro] HTTP listen() failed: " + std::to_string(WSAGetLastError()));
        closesocket(m_listen);
        m_listen = INVALID_SOCKET;
        return;
    }
    m_running = true;
    m_acceptThread = std::thread(&LocalHttpServer::AcceptLoop, this);
    m_parent->Log("[Takaro] HTTP listening on 127.0.0.1:" + std::to_string(m_port));
}

void LocalHttpServer::Stop() {
    m_running = false;
    if (m_listen != INVALID_SOCKET) {
        closesocket(m_listen);
        m_listen = INVALID_SOCKET;
    }
    if (m_acceptThread.joinable()) m_acceptThread.join();
    WSACleanup();
}

void LocalHttpServer::EnqueueOperation(const PendingOperation& op) {
    std::lock_guard<std::mutex> g(m_queueMutex);
    m_operations.push(op);
    m_opAction[op.operationId] = op.action;
}

void LocalHttpServer::AcceptLoop() {
    while (m_running) {
        sockaddr_in caddr{};
        int caddrLen = sizeof(caddr);
        SOCKET client = accept(m_listen, (sockaddr*)&caddr, &caddrLen);
        if (client == INVALID_SOCKET) {
            if (m_running) Sleep(50);
            continue;
        }
        // Each connection on its own thread — cheap, infrequent calls
        std::thread([this, client]() { HandleClient(client); }).detach();
    }
}

std::string LocalHttpServer::ReadHttpRequest(SOCKET sock, std::string& method,
                                              std::string& path, std::string& body) {
    std::string headerBuf;
    char buf[4096];
    while (headerBuf.find("\r\n\r\n") == std::string::npos) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) return "recv-failed";
        headerBuf.append(buf, n);
        if (headerBuf.size() > 65536) return "header-too-big";
    }
    size_t headerEnd = headerBuf.find("\r\n\r\n");
    std::string header = headerBuf.substr(0, headerEnd);
    body = headerBuf.substr(headerEnd + 4);

    // Parse request line
    size_t lineEnd = header.find("\r\n");
    std::string reqLine = header.substr(0, lineEnd);
    size_t sp1 = reqLine.find(' ');
    size_t sp2 = reqLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return "bad-request-line";
    method = reqLine.substr(0, sp1);
    path = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);

    // Find Content-Length and read remainder of body if needed
    size_t cl = 0;
    {
        std::string h = header;
        for (auto& c : h) c = (char)tolower((unsigned char)c);
        size_t p = h.find("content-length:");
        if (p != std::string::npos) {
            p += 15;
            while (p < h.size() && h[p] == ' ') p++;
            cl = strtoul(h.c_str() + p, nullptr, 10);
        }
    }
    while (body.size() < cl) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, n);
    }
    if (body.size() > cl) body.resize(cl);
    return "";
}

void LocalHttpServer::WriteHttpResponse(SOCKET sock, int code, const std::string& body,
                                         const std::string& contentType) {
    std::string status;
    switch (code) {
        case 200: status = "200 OK"; break;
        case 400: status = "400 Bad Request"; break;
        case 404: status = "404 Not Found"; break;
        case 500: status = "500 Internal Server Error"; break;
        default: status = std::to_string(code) + " ?"; break;
    }
    std::ostringstream os;
    os << "HTTP/1.1 " << status << "\r\n";
    os << "Content-Type: " << contentType << "\r\n";
    os << "Content-Length: " << body.size() << "\r\n";
    os << "Connection: close\r\n\r\n";
    os << body;
    std::string out = os.str();
    send(sock, out.data(), (int)out.size(), 0);
}

void LocalHttpServer::HandleClient(SOCKET client) {
    std::string method, path, body;
    std::string err = ReadHttpRequest(client, method, path, body);
    if (!err.empty()) { closesocket(client); return; }

    // Strip query string
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);

    std::string outBody = "{}";
    int code = 200;

    if (method == "GET" && path == "/health") {
        std::ostringstream os;
        os << "{\"ok\":true,\"identified\":"
           << (m_parent->GetGameServerId().empty() ? "false" : "true")
           << ",\"gameServerId\":\"" << SimpleJSON::escape(m_parent->GetGameServerId())
           << "\",\"pendingOperations\":" << [&]() {
                std::lock_guard<std::mutex> g(m_queueMutex);
                return std::to_string(m_operations.size());
              }() << "}";
        outBody = os.str();
    } else if (method == "POST" && path == "/gameserver/register") {
        if (m_parent->GetGameServerId().empty()) {
            code = 503; outBody = "{\"error\":\"not-yet-identified\"}";
        } else {
            outBody = SimpleJSON::object(
                SimpleJSON::pair("identityToken", m_parent->GetIdentityToken()) + "," +
                SimpleJSON::pair("gameServerId", m_parent->GetGameServerId())
            );
        }
    } else if (method == "POST" && path.find("/gameserver/") == 0 && path.size() > 12) {
        std::string rest = path.substr(12);  // strip "/gameserver/"
        size_t slash = rest.find('/');
        std::string id = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        std::string sub = (slash == std::string::npos) ? "" : rest.substr(slash + 1);

        if (sub == "events") {
            // body should be {"events":[{type,...}, ...]}
            // We naively extract each event by finding "type":"X" and forwarding.
            // The script mod sends one event at a time in normal usage, so the
            // dumbest possible parsing works.
            size_t pos = 0;
            int forwarded = 0;
            while (true) {
                size_t tpos = body.find("\"type\":\"", pos);
                if (tpos == std::string::npos) break;
                size_t tend = body.find("\"", tpos + 8);
                std::string type = body.substr(tpos + 8, tend - (tpos + 8));
                // Find the enclosing object: scan for matching braces around tpos
                size_t objStart = body.rfind('{', tpos);
                if (objStart == std::string::npos) break;
                int depth = 0; size_t i = objStart;
                while (i < body.size()) {
                    if (body[i] == '{') depth++;
                    else if (body[i] == '}') { depth--; if (depth == 0) break; }
                    i++;
                }
                if (i >= body.size()) break;
                std::string obj = body.substr(objStart, i - objStart + 1);
                // Strip the "type" field from the data we forward — Takaro takes
                // type separately. Just pass the whole object as `data`.
                m_parent->ForwardEventToWs(type, obj);
                forwarded++;
                pos = i + 1;
            }
            outBody = "{\"ok\":true,\"forwarded\":" + std::to_string(forwarded) + "}";
        } else if (sub.rfind("operation/", 0) == 0) {
            std::string opPath = sub.substr(10);
            size_t s2 = opPath.find('/');
            if (s2 != std::string::npos && opPath.substr(s2 + 1) == "result") {
                std::string opId = opPath.substr(0, s2);
                // body is {"operationId":..., "ok":true|false, "result":..., "errorMessage":...}
                // Extract fields naively.
                bool ok = body.find("\"ok\":true") != std::string::npos;
                std::string action;
                {
                    std::lock_guard<std::mutex> g(m_queueMutex);
                    auto it = m_opAction.find(opId);
                    if (it != m_opAction.end()) { action = it->second; m_opAction.erase(it); }
                }
                std::string resultJson;
                {
                    // Find "result": value (object or array). If not present, ""
                    size_t rp = body.find("\"result\":");
                    if (rp != std::string::npos) {
                        rp += 9;
                        while (rp < body.size() && body[rp] == ' ') rp++;
                        if (rp < body.size() && (body[rp] == '{' || body[rp] == '[')) {
                            char open = body[rp], close = (open == '{') ? '}' : ']';
                            int d = 0; size_t e = rp;
                            while (e < body.size()) {
                                if (body[e] == open) d++;
                                else if (body[e] == close) { d--; if (d == 0) break; }
                                e++;
                            }
                            resultJson = body.substr(rp, e - rp + 1);
                        }
                    }
                }
                std::string errMsg;
                {
                    size_t ep = body.find("\"error\":\"");
                    if (ep == std::string::npos) ep = body.find("\"errorMessage\":\"");
                    if (ep != std::string::npos) {
                        ep = body.find("\"", ep + 9) + 1;
                        size_t ee = body.find("\"", ep);
                        if (ee != std::string::npos) errMsg = body.substr(ep, ee - ep);
                    }
                }
                m_parent->ForwardResponse(opId, ok, resultJson, errMsg, action);
                outBody = "{\"ok\":true}";
            } else { code = 404; outBody = "{\"error\":\"bad-op-path\"}"; }
        } else { code = 404; outBody = "{\"error\":\"unknown-route\"}"; }
    } else if (method == "GET" && path.find("/gameserver/") == 0 && path.size() > 12) {
        std::string rest = path.substr(12);
        size_t slash = rest.find('/');
        std::string sub = (slash == std::string::npos) ? "" : rest.substr(slash + 1);
        if (sub == "poll") {
            std::string ops = "[";
            bool first = true;
            std::lock_guard<std::mutex> g(m_queueMutex);
            while (!m_operations.empty()) {
                const PendingOperation& op = m_operations.front();
                if (!first) ops += ",";
                first = false;
                ops += SimpleJSON::object(
                    SimpleJSON::pair("operationId", op.operationId) + "," +
                    SimpleJSON::pair("action", op.action) + "," +
                    SimpleJSON::pair("argsJson", op.argsJson)
                );
                m_operations.pop();
            }
            ops += "]";
            outBody = "{\"operations\":" + ops + "}";
        } else { code = 404; outBody = "{\"error\":\"unknown-route\"}"; }
    } else {
        code = 404; outBody = "{\"error\":\"not-found\",\"path\":\"" + SimpleJSON::escape(path) + "\"}";
    }

    WriteHttpResponse(client, code, outBody);
    closesocket(client);
}

// ====================================================================
// TakaroDayZ impl
// ====================================================================

void TakaroDayZ::Log(const std::string& msg) {
    std::lock_guard<std::mutex> g(logMutex);
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

void TakaroDayZ::LoadConfig() {
    std::ifstream cfg("takaro_config.txt");
    if (!cfg.is_open()) return;
    std::string line;
    while (std::getline(cfg, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        if (k == "registrationToken") registrationToken = v;
        else if (k == "serverName") serverName = v;
        else if (k == "identityToken") identityToken = v;
        else if (k == "localPort") localPort = atoi(v.c_str());
    }
    if (identityToken.empty()) identityToken = serverName;
}

void TakaroDayZ::CloseHandles() {
    if (hWebSocket) {
        WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(hWebSocket);
        hWebSocket = nullptr;
    }
    if (hConnect) { WinHttpCloseHandle(hConnect); hConnect = nullptr; }
    if (hSession) { WinHttpCloseHandle(hSession); hSession = nullptr; }
}

bool TakaroDayZ::ConnectToTakaro() {
    hSession = WinHttpOpen(L"DayZ-Takaro/0.2",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    hConnect = WinHttpConnect(hSession, L"connect.takaro.io", 443, 0);
    if (!hConnect) { CloseHandles(); return false; }
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", L"/", nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
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

void TakaroDayZ::SendRaw(const std::string& msg) {
    if (!hWebSocket) return;
    DWORD r = WinHttpWebSocketSend(hWebSocket,
                                   WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                   (PVOID)msg.c_str(), (DWORD)msg.length());
    if (r != ERROR_SUCCESS) Log("[Takaro] WS send error: " + std::to_string(r));
}

void TakaroDayZ::SendIdentify() {
    std::string m = SimpleJSON::object(
        SimpleJSON::pair("type", "identify") + "," +
        SimpleJSON::objectPair("payload", SimpleJSON::object(
            SimpleJSON::pair("identityToken", identityToken) + "," +
            SimpleJSON::pair("registrationToken", registrationToken) + "," +
            SimpleJSON::pair("name", serverName)
        ))
    );
    SendRaw(m);
}

std::string TakaroDayZ::ExtractField(const std::string& m, const std::string& f) {
    std::string s = "\"" + f + "\":\"";
    size_t p = m.find(s);
    if (p == std::string::npos) return "";
    p += s.length();
    size_t e = m.find("\"", p);
    return e == std::string::npos ? "" : m.substr(p, e - p);
}

void TakaroDayZ::ForwardEventToWs(const std::string& type, const std::string& dataJson) {
    if (!connected || !hWebSocket) return;
    // Wrap into Takaro gameEvent shape.
    std::string msg = SimpleJSON::object(
        SimpleJSON::pair("type", "gameEvent") + "," +
        "\"payload\":{" +
            SimpleJSON::pair("type", type) + "," +
            "\"data\":" + dataJson +
        "}"
    );
    SendRaw(msg);
    Log("[Takaro] forwarded gameEvent " + type);
}

void TakaroDayZ::ForwardResponse(const std::string& opId, bool ok,
                                 const std::string& resultJson, const std::string& errorMessage,
                                 const std::string& action) {
    if (!connected || !hWebSocket) return;
    if (ok) {
        // Per-action result-shape coercion (Enforce Script JsonSerializer quirks):
        //  - bool fields serialize as 0/1 → coerce known fields back to true/false
        //  - {players:[...]} wrapper → unwrap to bare array for getPlayers/listPlayers
        std::string payload = resultJson.empty() ? std::string("{}") : resultJson;

        if (action == "getPlayers" || action == "listPlayers") {
            // Try unwrap {"players":[...]}
            size_t pp = payload.find("\"players\":");
            if (pp != std::string::npos) {
                size_t lb = payload.find('[', pp);
                if (lb != std::string::npos) {
                    int d = 0; size_t e = lb;
                    while (e < payload.size()) {
                        if (payload[e] == '[') d++;
                        else if (payload[e] == ']') { d--; if (d == 0) break; }
                        e++;
                    }
                    if (e < payload.size()) payload = payload.substr(lb, e - lb + 1);
                }
            }
        }
        // Bool coercion: replace "connectable":1 → "connectable":true, etc.
        auto coerceField = [&](const std::string& key) {
            std::string k1 = "\"" + key + "\":1";
            std::string k0 = "\"" + key + "\":0";
            std::string v1 = "\"" + key + "\":true";
            std::string v0 = "\"" + key + "\":false";
            for (size_t p = 0; (p = payload.find(k1, p)) != std::string::npos; p += v1.size())
                payload.replace(p, k1.size(), v1);
            for (size_t p = 0; (p = payload.find(k0, p)) != std::string::npos; p += v0.size())
                payload.replace(p, k0.size(), v0);
        };
        if (action == "testReachability") coerceField("connectable");
        if (action == "getPlayers" || action == "listPlayers" || action == "getPlayer")
            coerceField("online");

        std::string msg = SimpleJSON::object(
            SimpleJSON::pair("type", "response") + "," +
            SimpleJSON::pair("requestId", opId) + "," +
            "\"payload\":" + payload
        );
        SendRaw(msg);
    } else {
        std::string msg = SimpleJSON::object(
            SimpleJSON::pair("type", "error") + "," +
            SimpleJSON::pair("requestId", opId) + "," +
            SimpleJSON::objectPair("payload", SimpleJSON::object(
                SimpleJSON::pair("message", errorMessage.empty() ? "unknown" : errorMessage)
            ))
        );
        SendRaw(msg);
    }
    Log("[Takaro] response op=" + opId + " action=" + action + " ok=" + (ok ? "true" : "false"));
}

void TakaroDayZ::HandleTestReachability(const std::string& requestId) {
    std::string msg = SimpleJSON::object(
        SimpleJSON::pair("type", "response") + "," +
        SimpleJSON::pair("requestId", requestId) + "," +
        "\"payload\":{\"connectable\":true,\"reason\":null}"
    );
    SendRaw(msg);
}

void TakaroDayZ::HandleListEmpty(const std::string& requestId) {
    std::string msg = SimpleJSON::object(
        SimpleJSON::pair("type", "response") + "," +
        SimpleJSON::pair("requestId", requestId) + "," +
        "\"payload\":[]"
    );
    SendRaw(msg);
}

void TakaroDayZ::QueueForScriptMod(const std::string& requestId, const std::string& action,
                                    const std::string& argsJson) {
    PendingOperation op;
    op.operationId = requestId;
    op.action = action;
    op.argsJson = argsJson.empty() ? "{}" : argsJson;
    if (localServer) localServer->EnqueueOperation(op);
    Log("[Takaro] queued op for script mod: action=" + action + " id=" + requestId);
}

void TakaroDayZ::HandleMessage(const std::string& m) {
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
    if (m.find("\"type\":\"request\"") == std::string::npos) return;

    std::string requestId = ExtractField(m, "requestId");
    std::string action = ExtractField(m, "action");

    // Built-ins handled directly (no script mod needed):
    if (action == "testReachability") { HandleTestReachability(requestId); return; }
    if (action == "listItems" || action == "listEntities" || action == "listLocations" || action == "listBans") {
        HandleListEmpty(requestId); return;
    }

    // Everything else: hand off to script mod via the local HTTP queue.
    // Extract argsJson — find "args": value (could be string or object).
    std::string argsJson;
    {
        size_t p = m.find("\"args\":");
        if (p != std::string::npos) {
            p += 7;
            while (p < m.size() && m[p] == ' ') p++;
            if (p < m.size() && m[p] == '"') {
                // String form: args is a quoted JSON string
                size_t e = p + 1;
                while (e < m.size() && m[e] != '"') {
                    if (m[e] == '\\' && e + 1 < m.size()) e++;
                    e++;
                }
                argsJson = m.substr(p + 1, e - p - 1);
                // Unescape backslash-quote and backslash-backslash sequences.
                std::string un;
                for (size_t i = 0; i < argsJson.size(); i++) {
                    if (argsJson[i] == '\\' && i + 1 < argsJson.size()) {
                        un += argsJson[i + 1]; i++;
                    } else un += argsJson[i];
                }
                argsJson = un;
            } else if (p < m.size() && m[p] == '{') {
                // Object form
                int d = 0; size_t e = p;
                while (e < m.size()) {
                    if (m[e] == '{') d++;
                    else if (m[e] == '}') { d--; if (d == 0) break; }
                    e++;
                }
                argsJson = m.substr(p, e - p + 1);
            }
        }
    }
    QueueForScriptMod(requestId, action, argsJson);
}

void TakaroDayZ::WebSocketThread() {
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
        BYTE buf[8192];
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

std::string TakaroDayZ::TimestampedLogPath() {
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

void TakaroDayZ::Start() {
    logFile.open(TimestampedLogPath(), std::ios::out);
    Log("[Takaro] DayZ DLL initializing v0.2 (HTTP-bridged)");
    LoadConfig();
    if (registrationToken.empty()) {
        Log("[Takaro] ERROR: takaro_config.txt missing or has no registrationToken");
        return;
    }
    Log("[Takaro] serverName=" + serverName + " localPort=" + std::to_string(localPort));

    running = true;
    localServer = new LocalHttpServer(this, localPort);
    localServer->Start();
    wsThread = std::thread(&TakaroDayZ::WebSocketThread, this);
    Log("[Takaro] threads started");
}

void TakaroDayZ::Stop() {
    running = false;
    connected = false;
    CloseHandles();
    if (localServer) {
        localServer->Stop();
        delete localServer;
        localServer = nullptr;
    }
    if (wsThread.joinable()) wsThread.join();
    Log("[Takaro] stopped");
    if (logFile.is_open()) logFile.close();
}

// ---- DLL entry ------------------------------------------------------

static TakaroDayZ* g_Takaro = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/) {
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
