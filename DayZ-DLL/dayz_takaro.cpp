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
#include <condition_variable>
#include <functional>
#include <cstring>

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
// CRC32 + MD5 helpers (used by BattlEye RCON framing and Steam64→BE-GUID)
// ====================================================================

static uint32_t Crc32_Table(uint32_t i) {
    uint32_t c = i;
    for (int j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    return c;
}
static uint32_t Crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) { for (int i = 0; i < 256; i++) table[i] = Crc32_Table((uint32_t)i); init = true; }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// Tiny MD5 implementation. Public-domain reference (Solar Designer / Alexander
// Peslyak). Inlined to avoid pulling in OpenSSL/CryptoAPI.
struct Md5Ctx { uint32_t a,b,c,d; uint32_t lo,hi; uint8_t buffer[64]; uint32_t block[16]; };
static const uint8_t* md5_body(Md5Ctx* ctx, const uint8_t* data, uint32_t size) {
    uint32_t a=ctx->a,b=ctx->b,c=ctx->c,d=ctx->d;
    do {
        uint32_t saved_a=a, saved_b=b, saved_c=c, saved_d=d;
        for (int i=0;i<16;i++) ctx->block[i] =
            (uint32_t)data[i*4] | ((uint32_t)data[i*4+1]<<8) |
            ((uint32_t)data[i*4+2]<<16) | ((uint32_t)data[i*4+3]<<24);
#define F(x,y,z) ((z) ^ ((x) & ((y)^(z))))
#define G(x,y,z) ((y) ^ ((z) & ((x)^(y))))
#define H(x,y,z) (((x)^(y))^(z))
#define I(x,y,z) ((y) ^ ((x) | ~(z)))
#define STEP(f,a,b,c,d,x,t,s) (a) += f((b),(c),(d)) + (x) + (t); (a) = ((a) << (s)) | (((a) & 0xFFFFFFFFu) >> (32-(s))); (a) += (b);
        STEP(F,a,b,c,d,ctx->block[0],0xd76aa478,7) STEP(F,d,a,b,c,ctx->block[1],0xe8c7b756,12)
        STEP(F,c,d,a,b,ctx->block[2],0x242070db,17) STEP(F,b,c,d,a,ctx->block[3],0xc1bdceee,22)
        STEP(F,a,b,c,d,ctx->block[4],0xf57c0faf,7) STEP(F,d,a,b,c,ctx->block[5],0x4787c62a,12)
        STEP(F,c,d,a,b,ctx->block[6],0xa8304613,17) STEP(F,b,c,d,a,ctx->block[7],0xfd469501,22)
        STEP(F,a,b,c,d,ctx->block[8],0x698098d8,7) STEP(F,d,a,b,c,ctx->block[9],0x8b44f7af,12)
        STEP(F,c,d,a,b,ctx->block[10],0xffff5bb1,17) STEP(F,b,c,d,a,ctx->block[11],0x895cd7be,22)
        STEP(F,a,b,c,d,ctx->block[12],0x6b901122,7) STEP(F,d,a,b,c,ctx->block[13],0xfd987193,12)
        STEP(F,c,d,a,b,ctx->block[14],0xa679438e,17) STEP(F,b,c,d,a,ctx->block[15],0x49b40821,22)
        STEP(G,a,b,c,d,ctx->block[1],0xf61e2562,5) STEP(G,d,a,b,c,ctx->block[6],0xc040b340,9)
        STEP(G,c,d,a,b,ctx->block[11],0x265e5a51,14) STEP(G,b,c,d,a,ctx->block[0],0xe9b6c7aa,20)
        STEP(G,a,b,c,d,ctx->block[5],0xd62f105d,5) STEP(G,d,a,b,c,ctx->block[10],0x02441453,9)
        STEP(G,c,d,a,b,ctx->block[15],0xd8a1e681,14) STEP(G,b,c,d,a,ctx->block[4],0xe7d3fbc8,20)
        STEP(G,a,b,c,d,ctx->block[9],0x21e1cde6,5) STEP(G,d,a,b,c,ctx->block[14],0xc33707d6,9)
        STEP(G,c,d,a,b,ctx->block[3],0xf4d50d87,14) STEP(G,b,c,d,a,ctx->block[8],0x455a14ed,20)
        STEP(G,a,b,c,d,ctx->block[13],0xa9e3e905,5) STEP(G,d,a,b,c,ctx->block[2],0xfcefa3f8,9)
        STEP(G,c,d,a,b,ctx->block[7],0x676f02d9,14) STEP(G,b,c,d,a,ctx->block[12],0x8d2a4c8a,20)
        STEP(H,a,b,c,d,ctx->block[5],0xfffa3942,4) STEP(H,d,a,b,c,ctx->block[8],0x8771f681,11)
        STEP(H,c,d,a,b,ctx->block[11],0x6d9d6122,16) STEP(H,b,c,d,a,ctx->block[14],0xfde5380c,23)
        STEP(H,a,b,c,d,ctx->block[1],0xa4beea44,4) STEP(H,d,a,b,c,ctx->block[4],0x4bdecfa9,11)
        STEP(H,c,d,a,b,ctx->block[7],0xf6bb4b60,16) STEP(H,b,c,d,a,ctx->block[10],0xbebfbc70,23)
        STEP(H,a,b,c,d,ctx->block[13],0x289b7ec6,4) STEP(H,d,a,b,c,ctx->block[0],0xeaa127fa,11)
        STEP(H,c,d,a,b,ctx->block[3],0xd4ef3085,16) STEP(H,b,c,d,a,ctx->block[6],0x04881d05,23)
        STEP(H,a,b,c,d,ctx->block[9],0xd9d4d039,4) STEP(H,d,a,b,c,ctx->block[12],0xe6db99e5,11)
        STEP(H,c,d,a,b,ctx->block[15],0x1fa27cf8,16) STEP(H,b,c,d,a,ctx->block[2],0xc4ac5665,23)
        STEP(I,a,b,c,d,ctx->block[0],0xf4292244,6) STEP(I,d,a,b,c,ctx->block[7],0x432aff97,10)
        STEP(I,c,d,a,b,ctx->block[14],0xab9423a7,15) STEP(I,b,c,d,a,ctx->block[5],0xfc93a039,21)
        STEP(I,a,b,c,d,ctx->block[12],0x655b59c3,6) STEP(I,d,a,b,c,ctx->block[3],0x8f0ccc92,10)
        STEP(I,c,d,a,b,ctx->block[10],0xffeff47d,15) STEP(I,b,c,d,a,ctx->block[1],0x85845dd1,21)
        STEP(I,a,b,c,d,ctx->block[8],0x6fa87e4f,6) STEP(I,d,a,b,c,ctx->block[15],0xfe2ce6e0,10)
        STEP(I,c,d,a,b,ctx->block[6],0xa3014314,15) STEP(I,b,c,d,a,ctx->block[13],0x4e0811a1,21)
        STEP(I,a,b,c,d,ctx->block[4],0xf7537e82,6) STEP(I,d,a,b,c,ctx->block[11],0xbd3af235,10)
        STEP(I,c,d,a,b,ctx->block[2],0x2ad7d2bb,15) STEP(I,b,c,d,a,ctx->block[9],0xeb86d391,21)
        a += saved_a; b += saved_b; c += saved_c; d += saved_d;
        data += 64; size -= 64;
    } while (size);
    ctx->a=a;ctx->b=b;ctx->c=c;ctx->d=d; return data;
#undef F
#undef G
#undef H
#undef I
#undef STEP
}
static void md5_init(Md5Ctx* ctx) {
    ctx->a=0x67452301;ctx->b=0xefcdab89;ctx->c=0x98badcfe;ctx->d=0x10325476;ctx->lo=0;ctx->hi=0;
}
static void md5_update(Md5Ctx* ctx, const uint8_t* data, uint32_t size) {
    uint32_t saved_lo = ctx->lo;
    if ((ctx->lo = (saved_lo + size) & 0x1FFFFFFF) < saved_lo) ctx->hi++;
    ctx->hi += size >> 29;
    uint32_t used = saved_lo & 0x3F;
    if (used) {
        uint32_t available = 64 - used;
        if (size < available) { memcpy(&ctx->buffer[used], data, size); return; }
        memcpy(&ctx->buffer[used], data, available);
        data += available; size -= available;
        md5_body(ctx, ctx->buffer, 64);
    }
    if (size >= 64) { uint32_t whole = size & ~(uint32_t)0x3F; data = md5_body(ctx, data, whole); size &= 0x3F; }
    memcpy(ctx->buffer, data, size);
}
static void md5_final(Md5Ctx* ctx, uint8_t* result) {
    uint32_t used = ctx->lo & 0x3F;
    ctx->buffer[used++] = 0x80;
    uint32_t available = 64 - used;
    if (available < 8) { memset(&ctx->buffer[used], 0, available); md5_body(ctx, ctx->buffer, 64); used = 0; available = 64; }
    memset(&ctx->buffer[used], 0, available - 8);
    ctx->lo <<= 3;
    ctx->buffer[56] = (uint8_t)ctx->lo; ctx->buffer[57] = (uint8_t)(ctx->lo>>8);
    ctx->buffer[58] = (uint8_t)(ctx->lo>>16); ctx->buffer[59] = (uint8_t)(ctx->lo>>24);
    ctx->buffer[60] = (uint8_t)ctx->hi; ctx->buffer[61] = (uint8_t)(ctx->hi>>8);
    ctx->buffer[62] = (uint8_t)(ctx->hi>>16); ctx->buffer[63] = (uint8_t)(ctx->hi>>24);
    md5_body(ctx, ctx->buffer, 64);
    for (int i = 0; i < 4; i++) {
        uint32_t v = (i==0)?ctx->a:(i==1)?ctx->b:(i==2)?ctx->c:ctx->d;
        result[i*4]=(uint8_t)v; result[i*4+1]=(uint8_t)(v>>8);
        result[i*4+2]=(uint8_t)(v>>16); result[i*4+3]=(uint8_t)(v>>24);
    }
}

// Compute the BattlEye GUID for a Steam64 ID.
// BE GUID = MD5("BE" + steam64 as 8 little-endian bytes), lowercase hex.
static std::string SteamIdToBeGuid(const std::string& steam64) {
    if (steam64.empty()) return "";
    uint64_t s = 0;
    for (char c : steam64) {
        if (c < '0' || c > '9') return "";
        s = s * 10 + (uint64_t)(c - '0');
    }
    uint8_t buf[10] = { 'B', 'E',
        (uint8_t)s, (uint8_t)(s>>8), (uint8_t)(s>>16), (uint8_t)(s>>24),
        (uint8_t)(s>>32), (uint8_t)(s>>40), (uint8_t)(s>>48), (uint8_t)(s>>56) };
    Md5Ctx ctx; md5_init(&ctx); md5_update(&ctx, buf, 10);
    uint8_t digest[16]; md5_final(&ctx, digest);
    std::string hex; hex.reserve(32);
    static const char* h = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { hex += h[digest[i] >> 4]; hex += h[digest[i] & 0xF]; }
    return hex;
}

// ====================================================================
// BattlEye RCON client
// ====================================================================
//
// Spec: https://www.battleye.com/downloads/BERConProtocol.txt
// Framing (every packet, both directions):
//     'B' 'E'  <CRC32:LE 4>  0xFF  <type:1>  <payload...>
// CRC covers from 0xFF onward (i.e. type + payload).
//
// Types:
//   0x00 = login. Client: <password>. Server: 0x01 success / 0x00 fail.
//   0x01 = command. Client: <seq:1> <command-ascii>. Empty command = keepalive.
//          Server: <seq:1> <ascii>  OR  <seq:1> 0x00 <total:1> <index:1> <chunk>
//   0x02 = server-pushed message. Server: <seq:1> <ascii>. Client MUST ack
//          with <seq:1> (no payload) or it'll get disconnected.
//
// Idle timeout ~45s — we send empty command every 30s.

struct BeRconConfig {
    std::string host = "127.0.0.1";
    int port = 2306;
    std::string password;
};

class BeRcon {
public:
    BeRcon(TakaroDayZ* parent, const BeRconConfig& cfg) : m_parent(parent), m_cfg(cfg) {}
    void Start();
    void Stop();
    bool LoggedIn() const { return m_loggedIn.load(); }

    // Fire-and-forget command. Returns the seq number used.
    uint8_t SendAsync(const std::string& cmd);
    // Synchronous: blocks up to timeoutMs for the response. Returns "" on timeout.
    std::string SendSync(const std::string& cmd, int timeoutMs = 5000);

    // Setter for server-pushed message callback (chat, join/leave from BE).
    std::function<void(const std::string&)> onServerMessage;

private:
    void RunLoop();
    void KeepaliveLoop();
    bool DoLogin();
    void SendPacket(uint8_t type, const std::vector<uint8_t>& payload);
    void HandlePacket(const uint8_t* data, int len);

    TakaroDayZ* m_parent;
    BeRconConfig m_cfg;
    SOCKET m_sock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_loggedIn{false};
    std::atomic<uint8_t> m_nextSeq{0};
    std::thread m_recvThread;
    std::thread m_keepaliveThread;

    // Fragment reassembly: seq -> (total, map<index,chunk>)
    struct Partial { uint8_t total = 0; std::map<uint8_t, std::string> chunks; };
    std::map<uint8_t, Partial> m_partials;

    // Pending sync waiter
    std::mutex m_waitMutex;
    std::condition_variable m_waitCv;
    int m_waitSeq = -1;            // -1 = no waiter
    std::string m_waitResponse;
    std::atomic<bool> m_waitDone{false};
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
    BeRcon* beRcon = nullptr;
    std::string beServerCfgPath = "battleye\\beserver_x64.cfg";

    void LoadConfig();
    bool LoadBeRconConfig(BeRconConfig& out);
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

    // BE RCON–backed handlers
    void HandleExecConsoleCommandRcon(const std::string& requestId, const std::string& argsJson);
    void HandleSendMessageRcon(const std::string& requestId, const std::string& argsJson);

    // For non-builtin actions, hand off to the script mod.
    void QueueForScriptMod(const std::string& requestId, const std::string& action,
                           const std::string& argsJson);

    // Helper: pull a JSON string-field out of a small object.
    static std::string ExtractStringField(const std::string& json, const std::string& field);
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
// BeRcon impl
// ====================================================================

void BeRcon::Start() {
    m_running = true;
    m_recvThread = std::thread(&BeRcon::RunLoop, this);
    m_keepaliveThread = std::thread(&BeRcon::KeepaliveLoop, this);
}

void BeRcon::Stop() {
    m_running = false;
    m_loggedIn = false;
    if (m_sock != INVALID_SOCKET) { closesocket(m_sock); m_sock = INVALID_SOCKET; }
    if (m_recvThread.joinable()) m_recvThread.join();
    if (m_keepaliveThread.joinable()) m_keepaliveThread.join();
}

void BeRcon::SendPacket(uint8_t type, const std::vector<uint8_t>& payload) {
    if (m_sock == INVALID_SOCKET) return;
    // CRC covers 0xFF + type + payload
    std::vector<uint8_t> crcRegion;
    crcRegion.reserve(payload.size() + 2);
    crcRegion.push_back(0xFF);
    crcRegion.push_back(type);
    crcRegion.insert(crcRegion.end(), payload.begin(), payload.end());
    uint32_t crc = Crc32(crcRegion.data(), crcRegion.size());
    std::vector<uint8_t> packet;
    packet.reserve(crcRegion.size() + 6);
    packet.push_back('B'); packet.push_back('E');
    packet.push_back((uint8_t)crc); packet.push_back((uint8_t)(crc>>8));
    packet.push_back((uint8_t)(crc>>16)); packet.push_back((uint8_t)(crc>>24));
    packet.insert(packet.end(), crcRegion.begin(), crcRegion.end());
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)m_cfg.port);
    inet_pton(AF_INET, m_cfg.host.c_str(), &addr.sin_addr);
    sendto(m_sock, (const char*)packet.data(), (int)packet.size(), 0, (sockaddr*)&addr, sizeof(addr));
}

bool BeRcon::DoLogin() {
    std::vector<uint8_t> p(m_cfg.password.begin(), m_cfg.password.end());
    SendPacket(0x00, p);
    return true;
}

uint8_t BeRcon::SendAsync(const std::string& cmd) {
    uint8_t seq = m_nextSeq.fetch_add(1);
    std::vector<uint8_t> p;
    p.push_back(seq);
    p.insert(p.end(), cmd.begin(), cmd.end());
    SendPacket(0x01, p);
    return seq;
}

std::string BeRcon::SendSync(const std::string& cmd, int timeoutMs) {
    if (!m_loggedIn) return "";
    std::unique_lock<std::mutex> lk(m_waitMutex);
    uint8_t seq = m_nextSeq.fetch_add(1);
    m_waitSeq = (int)seq;
    m_waitResponse.clear();
    m_waitDone = false;
    std::vector<uint8_t> p;
    p.push_back(seq);
    p.insert(p.end(), cmd.begin(), cmd.end());
    SendPacket(0x01, p);
    m_waitCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]{ return m_waitDone.load(); });
    std::string out = m_waitResponse;
    m_waitSeq = -1;
    return out;
}

void BeRcon::HandlePacket(const uint8_t* data, int len) {
    if (len < 8) return;
    if (data[0] != 'B' || data[1] != 'E') return;
    if (data[6] != 0xFF) return;
    uint8_t type = data[7];
    const uint8_t* payload = data + 8;
    int paylen = len - 8;

    if (type == 0x00) {
        // Login response: 1 byte
        if (paylen >= 1) {
            if (payload[0] == 0x01) {
                m_loggedIn = true;
                m_parent->Log("[Takaro][RCON] login OK");
            } else {
                m_parent->Log("[Takaro][RCON] login REJECTED (bad password?)");
            }
        }
    } else if (type == 0x01) {
        // Command response.
        if (paylen < 1) return;
        uint8_t seq = payload[0];
        // Multi-packet sub-header?
        if (paylen >= 4 && payload[1] == 0x00) {
            uint8_t total = payload[2];
            uint8_t index = payload[3];
            std::string chunk((const char*)(payload + 4), paylen - 4);
            Partial& p = m_partials[seq];
            p.total = total;
            p.chunks[index] = chunk;
            if ((uint8_t)p.chunks.size() == total) {
                std::string full;
                for (uint8_t i = 0; i < total; i++) full += p.chunks[i];
                m_partials.erase(seq);
                std::lock_guard<std::mutex> g(m_waitMutex);
                if (m_waitSeq == (int)seq) {
                    m_waitResponse = full;
                    m_waitDone = true;
                    m_waitCv.notify_all();
                }
            }
            return;
        }
        // Single-packet response
        std::string body((const char*)(payload + 1), paylen - 1);
        std::lock_guard<std::mutex> g(m_waitMutex);
        if (m_waitSeq == (int)seq) {
            m_waitResponse = body;
            m_waitDone = true;
            m_waitCv.notify_all();
        }
    } else if (type == 0x02) {
        // Server-pushed message — ack first, process second.
        if (paylen < 1) return;
        uint8_t seq = payload[0];
        std::vector<uint8_t> ack; ack.push_back(seq);
        SendPacket(0x02, ack);
        std::string body((const char*)(payload + 1), paylen - 1);
        if (onServerMessage) onServerMessage(body);
    }
}

void BeRcon::RunLoop() {
    int retryDelay = 2000;
    while (m_running) {
        if (m_sock == INVALID_SOCKET) {
            m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (m_sock == INVALID_SOCKET) { Sleep(retryDelay); continue; }
            // 5s recv timeout so we can poll m_running
            DWORD timeout = 5000;
            setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            DoLogin();
        }
        uint8_t buf[4096];
        sockaddr_in from{}; int fromLen = sizeof(from);
        int n = recvfrom(m_sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n > 0) {
            HandlePacket(buf, n);
        } else {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                // No data — that's fine, just keepalive will run
            } else if (err == WSAEMSGSIZE) {
                // Truncated, ignore this packet
            } else {
                m_parent->Log("[Takaro][RCON] recv error " + std::to_string(err) + " — reconnecting");
                if (m_sock != INVALID_SOCKET) { closesocket(m_sock); m_sock = INVALID_SOCKET; }
                m_loggedIn = false;
                Sleep(retryDelay);
            }
        }
    }
}

void BeRcon::KeepaliveLoop() {
    int counter = 0;
    while (m_running) {
        Sleep(1000);
        if (!m_loggedIn) continue;
        counter++;
        if (counter >= 30) {
            counter = 0;
            // Empty command = keepalive
            SendAsync("");
        }
    }
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
        else if (k == "beServerCfgPath") beServerCfgPath = v;
    }
    if (identityToken.empty()) identityToken = serverName;
}

bool TakaroDayZ::LoadBeRconConfig(BeRconConfig& out) {
    std::ifstream f(beServerCfgPath);
    if (!f.is_open()) {
        Log("[Takaro][RCON] beserver_x64.cfg not found at " + beServerCfgPath + " — RCON disabled");
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        // BE config is space-delimited: "RConPassword foo", "RConPort 2306"
        // Strip CR and skip comments (//) and empty lines
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        if (line.substr(p, 2) == "//") continue;
        size_t sp = line.find_first_of(" \t", p);
        if (sp == std::string::npos) continue;
        std::string key = line.substr(p, sp - p);
        size_t v = line.find_first_not_of(" \t", sp);
        if (v == std::string::npos) continue;
        std::string val = line.substr(v);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        if (key == "RConPassword") out.password = val;
        else if (key == "RConPort") out.port = atoi(val.c_str());
        else if (key == "RConIP") out.host = val;
    }
    if (out.password.empty()) {
        Log("[Takaro][RCON] beserver_x64.cfg has no RConPassword — RCON disabled");
        return false;
    }
    return true;
}

std::string TakaroDayZ::ExtractStringField(const std::string& json, const std::string& field) {
    std::string s = "\"" + field + "\":\"";
    size_t p = json.find(s);
    if (p == std::string::npos) return "";
    p += s.length();
    size_t e = json.find("\"", p);
    return e == std::string::npos ? "" : json.substr(p, e - p);
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

void TakaroDayZ::HandleExecConsoleCommandRcon(const std::string& requestId, const std::string& argsJson) {
    std::string command = ExtractStringField(argsJson, "command");
    if (command.empty()) {
        // Send back failure
        std::string payload = "{\"rawResult\":\"\",\"success\":false,\"errorMessage\":\"empty command\"}";
        std::string msg = SimpleJSON::object(
            SimpleJSON::pair("type","response") + "," +
            SimpleJSON::pair("requestId", requestId) + "," +
            "\"payload\":" + payload);
        SendRaw(msg);
        return;
    }
    std::string raw;
    if (beRcon && beRcon->LoggedIn()) {
        raw = beRcon->SendSync(command, 5000);
    }
    std::string payload = SimpleJSON::object(
        SimpleJSON::pair("rawResult", raw) + ",\"success\":true"
    );
    std::string msg = SimpleJSON::object(
        SimpleJSON::pair("type","response") + "," +
        SimpleJSON::pair("requestId", requestId) + "," +
        "\"payload\":" + payload
    );
    SendRaw(msg);
    Log("[Takaro][RCON] exec '" + command + "' -> " +
        (raw.empty() ? "(empty)" : raw.substr(0, 80)));
}

void TakaroDayZ::HandleSendMessageRcon(const std::string& requestId, const std::string& argsJson) {
    std::string text = ExtractStringField(argsJson, "message");
    std::string recipient = ExtractStringField(argsJson, "recipientGameId");
    if (text.empty()) {
        std::string msg = SimpleJSON::object(
            SimpleJSON::pair("type","response") + "," +
            SimpleJSON::pair("requestId", requestId) + ",\"payload\":{}");
        SendRaw(msg);
        return;
    }
    if (!beRcon || !beRcon->LoggedIn()) {
        // Fall back to script mod (which uses native broadcast).
        QueueForScriptMod(requestId, "sendMessage", argsJson);
        return;
    }
    // BE syntax: "say -1 <text>" for global broadcast. Per-player whisper via
    // slot number is possible but requires looking up the slot from the BE GUID,
    // which we don't have in v1 — fall back to script mod for whispers.
    if (!recipient.empty()) {
        QueueForScriptMod(requestId, "sendMessage", argsJson);
        return;
    }
    std::string cmd = "say -1 " + text;
    beRcon->SendAsync(cmd);
    std::string msg = SimpleJSON::object(
        SimpleJSON::pair("type","response") + "," +
        SimpleJSON::pair("requestId", requestId) + ",\"payload\":{}");
    SendRaw(msg);
    Log("[Takaro][RCON] say -1 " + text);
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
    // Note: list* actions used to short-circuit to []; now they flow through
    // the script mod so it can walk CfgVehicles for real catalogs.

    // Extract argsJson early so RCON handlers can use it.
    std::string argsJson;
    {
        size_t p = m.find("\"args\":");
        if (p != std::string::npos) {
            p += 7;
            while (p < m.size() && m[p] == ' ') p++;
            if (p < m.size() && m[p] == '"') {
                size_t e = p + 1;
                while (e < m.size() && m[e] != '"') {
                    if (m[e] == '\\' && e + 1 < m.size()) e++;
                    e++;
                }
                argsJson = m.substr(p + 1, e - p - 1);
                std::string un;
                for (size_t i = 0; i < argsJson.size(); i++) {
                    if (argsJson[i] == '\\' && i + 1 < argsJson.size()) { un += argsJson[i + 1]; i++; }
                    else un += argsJson[i];
                }
                argsJson = un;
            } else if (p < m.size() && m[p] == '{') {
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

    // BE-RCON-backed handlers (when RCON is up):
    if (action == "executeConsoleCommand") {
        if (beRcon && beRcon->LoggedIn()) { HandleExecConsoleCommandRcon(requestId, argsJson); return; }
    }
    if (action == "sendMessage") {
        if (beRcon && beRcon->LoggedIn()) { HandleSendMessageRcon(requestId, argsJson); return; }
    }

    // Everything else: hand off to script mod via the local HTTP queue.
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

    // BattlEye RCON — optional. Read beserver_x64.cfg for password+port.
    BeRconConfig beCfg;
    if (LoadBeRconConfig(beCfg)) {
        Log("[Takaro][RCON] using " + beCfg.host + ":" + std::to_string(beCfg.port));
        beRcon = new BeRcon(this, beCfg);
        beRcon->Start();
    }

    Log("[Takaro] threads started");
}

void TakaroDayZ::Stop() {
    running = false;
    connected = false;
    CloseHandles();
    if (beRcon) {
        beRcon->Stop();
        delete beRcon;
        beRcon = nullptr;
    }
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
