#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <D2RLPlugin/api.h>
#include <D2RLPlugin/logging.h>
#include <D2RLPlugin/shared_events.h>
#include <D2RLPlugin/widgets.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

namespace {

// ============================================================================
// Plugin Metadata
// ============================================================================
constexpr D2RL::PluginInfo kPluginInfo = {
    .infoSize = sizeof(D2RL::PluginInfo),
    .abiVersion = D2RL_PLUGIN_ABI_VERSION,
    .id = "d2rl-streamchat",
    .name = "D2R Stream Chat",
    .version = "1.1.0",
    .author = "GhostDragon14712",
    .description = "Streams Twitch & YouTube live chat directly into D2R native in-game chat.",
    .flags = D2RL::PluginFlags::Client,
    .reserved = {0, 0, 0, 0},
};

enum class Platform : uint8_t {
    Twitch,
    YouTube,
    System
};

enum class PrefixMode : uint8_t {
    Twitch,  // [Twitch]
    TTV,     // [TTV]
    None     // (no prefix)
};

enum class YtPrefixMode : uint8_t {
    YT,       // [YT]
    YouTube,  // [YouTube]
    None      // (no prefix)
};

struct ChatMessage {
    Platform platform;
    std::string author;
    std::string text;
    std::string timeStr;
    std::chrono::steady_clock::time_point timestamp;
};

// Global State
static const D2RL::PluginContext* g_context = nullptr;
static const D2RL::ThreadService* g_threads = nullptr;
static const D2RL::LifecycleService* g_lifecycle = nullptr;
static const D2RL::WidgetService* g_widgets = nullptr;

static std::atomic<bool> g_inGameSession{false};
static std::atomic<PrefixMode> g_prefixMode{PrefixMode::Twitch};
static std::atomic<YtPrefixMode> g_ytPrefixMode{YtPrefixMode::YT};
static std::atomic<bool> g_botFilter{true};
static std::atomic<bool> g_enableDevCommands{false};

// Layout style: single-line vs two-line
enum class ChatLayoutMode : uint8_t {
    SingleLine,
    TwoLine
};
static std::atomic<ChatLayoutMode> g_chatLayoutMode{ChatLayoutMode::SingleLine};

// Colors configuration for native chat formatting
static std::mutex g_colorMutex;
static std::string g_twitchPrefixColor = "purple";
static std::string g_twitchMessageColor = "white";
static std::string g_youtubePrefixColor = "red";
static std::string g_youtubeMessageColor = "white";

static inline std::string NormalizeColorName(const std::string& input) {
    std::string s = input;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    if (lower == "white" || lower == "0") return "white";
    if (lower == "red" || lower == "1") return "red";
    if (lower == "green" || lower == "bright_green" || lower == "brightgreen" || lower == "2") return "green";
    if (lower == "blue" || lower == "3") return "blue";
    if (lower == "gold" || lower == "4") return "gold";
    if (lower == "gray" || lower == "grey" || lower == "5") return "gray";
    if (lower == "black" || lower == "6") return "black";
    if (lower == "tan" || lower == "khaki" || lower == "7") return "tan";
    if (lower == "orange" || lower == "amber" || lower == "8") return "orange";
    if (lower == "yellow" || lower == "9") return "yellow";
    if (lower == "dark_green" || lower == "darkgreen" || lower == "10" || lower == ":") return "dark_green";
    if (lower == "purple" || lower == "magenta" || lower == "11" || lower == ";") return "purple";
    if (lower == "light_green" || lower == "lightgreen" || lower == "12" || lower == "<") return "light_green";
    if (lower == "none" || lower == "off" || lower == "default") return "default";
    return lower;
}

static inline uint8_t ColorNameToByte(const std::string& colorName, uint8_t defaultByte = 4) {
    std::string s = colorName;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (s == "white" || s == "0") return 0;
    if (s == "red" || s == "1") return 1;
    if (s == "green" || s == "bright_green" || s == "brightgreen" || s == "2") return 2;
    if (s == "blue" || s == "3") return 3;
    if (s == "gold" || s == "4") return 4;
    if (s == "gray" || s == "grey" || s == "5") return 5;
    if (s == "black" || s == "6") return 6;
    if (s == "tan" || s == "khaki" || s == "7") return 7;
    if (s == "orange" || s == "amber" || s == "8") return 8;
    if (s == "yellow" || s == "9") return 9;
    if (s == "dark_green" || s == "darkgreen" || s == "10" || s == ":") return 10;
    if (s == "purple" || s == "magenta" || s == "11" || s == ";") return 11;
    if (s == "light_green" || s == "lightgreen" || s == "12" || s == "<") return 12;
    return defaultByte;
}

static std::mutex g_configMutex;

static std::wstring GetPluginLogFilePath() {
    if (g_context && g_context->pluginLogPath && g_context->pluginLogPath[0] != L'\0') {
        return std::wstring(g_context->pluginLogPath);
    }
    if (g_context && g_context->pluginDirectory && g_context->pluginDirectory[0] != L'\0') {
        std::wstring dir = g_context->pluginDirectory;
        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
            dir += L'\\';
        }
        return dir + L"d2rl-streamchat.log";
    }
    return L"d2rl-streamchat.log";
}

static void WriteLog(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // 1. Send via proper PluginSDK logging mechanism
    if (g_context) {
        g_context->LogInfo(buf);
    }

    // 2. Write directly to d2rl-streamchat.log with timestamp
    std::wstring logPath = GetPluginLogFilePath();
    FILE* f = _wfopen(logPath.c_str(), L"a+");
    if (!f) {
        f = fopen("d2rl-streamchat.log", "a+");
    }
    if (f) {
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char timeBuf[32] = {};
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::fprintf(f, "[%s] %s\n", timeBuf, buf);
        std::fflush(f);
        std::fclose(f);
    }
}

// Dev & Diagnostic logger: Completely bypassed with zero overhead when enable_dev_commands = false
static void WriteDevLog(const char* fmt, ...) {
    if (!g_enableDevCommands.load()) {
        return;
    }
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (g_context) {
        g_context->LogInfo(buf);
    }

    std::wstring logPath = GetPluginLogFilePath();
    FILE* f = _wfopen(logPath.c_str(), L"a+");
    if (!f) {
        f = fopen("d2rl-streamchat.log", "a+");
    }
    if (f) {
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char timeBuf[32] = {};
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::fprintf(f, "[%s] %s\n", timeBuf, buf);
        std::fflush(f);
        std::fclose(f);
    }
}

#pragma pack(push, 8)
struct D2RStringRef {
    const char* data;
    uint64_t length;
};
#pragma pack(pop)

using D2RSubmitChatHandlerFn = void(__fastcall*)(void* widget, D2RStringRef* stringRef, uint64_t channel) noexcept;
using PushClientChatEntryFn = void(__fastcall*)(const D2RStringRef* stringRef, uint8_t color, uint64_t unused, uint64_t arg1, uint64_t arg2, void* ptr1, void* ptr2) noexcept;

constexpr uintptr_t kSubmitChatRva = 0x2E34C0;

static bool PushClientChatDirect(const char* text, uint8_t color = 4) {
    if (!text || !text[0]) return false;

    static PushClientChatEntryFn s_fnPushClient = nullptr;
    static bool s_resolved = false;

    if (!s_resolved) {
        s_resolved = true;
        HMODULE hCore = GetModuleHandleA("D2RCore.dll");
        if (!hCore) hCore = GetModuleHandleA("d2rcore.dll");
        if (hCore) {
            s_fnPushClient = reinterpret_cast<PushClientChatEntryFn>(GetProcAddress(hCore, "PushClientChatEntry"));
        }
    }

    if (!s_fnPushClient) return false;

    D2RStringRef ref{ text, static_cast<uint64_t>(std::strlen(text)) };

    __try {
        s_fnPushClient(&ref, color, 0, 0, 0, nullptr, nullptr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        WriteDevLog("[StreamChat] PushClientChatEntry exception: 0x%08X", GetExceptionCode());
        return false;
    }
}

static bool SubmitChatDirect(const char* text, uint64_t channel = 0) {
    if (!text || !text[0]) return false;
    HMODULE hD2R = GetModuleHandleA(NULL);
    if (!hD2R) return false;
    uint8_t* base = reinterpret_cast<uint8_t*>(hD2R);

    auto fnSubmit = reinterpret_cast<D2RSubmitChatHandlerFn>(base + kSubmitChatRva);

    char buf[512] = {};
    size_t len = std::min<size_t>(std::strlen(text), sizeof(buf) - 1);
    std::memcpy(buf, text, len);
    buf[len] = '\0';

    D2RStringRef ref{ buf, len };

    __try {
        fnSubmit(nullptr, reinterpret_cast<D2RStringRef*>(&ref), channel);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        WriteDevLog("[StreamChat] SubmitChatDirect exception: 0x%08X", GetExceptionCode());
        return false;
    }
}

static bool SendToNativeChat(const char* formattedMessage, uint8_t defaultColor = 4) {
    if (!formattedMessage || !formattedMessage[0]) return false;
    // Exclusively client-side delivery: 0 network packets sent in multiplayer/TCP, impossible to spam or grief other players
    return PushClientChatDirect(formattedMessage, defaultColor);
}

struct QueuedMessage {
    std::string text;
    uint8_t defaultColor = 4;
};

static void __cdecl DeliverMessageCallback(const D2RL::PluginContext*, void* userData) noexcept {
    auto* qm = static_cast<QueuedMessage*>(userData);
    if (qm) {
        SendToNativeChat(qm->text.c_str(), qm->defaultColor);
        delete qm;
    }
}

static void QueueMessageToMainThread(std::string text, uint8_t defaultColor = 4) {
    if (g_context && g_threads && D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
        auto* qm = new QueuedMessage{ std::move(text), defaultColor };
        auto res = g_threads->runOnUiThread(g_context, DeliverMessageCallback, qm);
        if (res == D2RL::Threads::Result::Success) {
            return;
        }
        delete qm;
    }
    SendToNativeChat(text.c_str(), defaultColor);
}

static auto GetCurrentTimeStr() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto timeT = std::chrono::system_clock::to_time_t(now);
    struct tm buf {};
    localtime_s(&buf, &timeT);
    char out[16];
    std::snprintf(out, sizeof(out), "%02d:%02d", buf.tm_hour, buf.tm_min);
    return std::string(out);
}

static std::mutex g_chatMutex;
static std::deque<ChatMessage> g_messages;

static void AddMessage(Platform platform, std::string author, std::string text, bool sendToNative = true) {
    std::lock_guard<std::mutex> lock(g_chatMutex);

    ChatMessage msg;
    msg.platform = platform;
    msg.author = std::move(author);
    msg.text = std::move(text);
    msg.timeStr = GetCurrentTimeStr();
    msg.timestamp = std::chrono::steady_clock::now();

    if (g_botFilter.load() && !msg.text.empty() && msg.text[0] == '!') {
        return;
    }

    std::string prefixColorStr, msgColorStr;
    {
        std::lock_guard<std::mutex> lk(g_colorMutex);
        if (platform == Platform::Twitch) {
            prefixColorStr = g_twitchPrefixColor;
            msgColorStr = g_twitchMessageColor;
        } else if (platform == Platform::YouTube) {
            prefixColorStr = g_youtubePrefixColor;
            msgColorStr = g_youtubeMessageColor;
        } else {
            prefixColorStr = "gold";
            msgColorStr = "white";
        }
    }

    std::string prefix;
    if (platform == Platform::Twitch) {
        PrefixMode pMode = g_prefixMode.load();
        if (pMode == PrefixMode::TTV) prefix = "[TTV] ";
        else if (pMode == PrefixMode::Twitch) prefix = "[Twitch] ";
    } else if (platform == Platform::YouTube) {
        YtPrefixMode ytMode = g_ytPrefixMode.load();
        if (ytMode == YtPrefixMode::YT) prefix = "[YT] ";
        else if (ytMode == YtPrefixMode::YouTube) prefix = "[YouTube] ";
    } else {
        prefix = "[Stream] ";
    }

    // Clean prefix and author to guarantee zero line breaks between prefix and name
    std::string cleanPrefix;
    cleanPrefix.reserve(prefix.length());
    for (char c : prefix) {
        if (c != '\r' && c != '\n') cleanPrefix.push_back(c);
    }

    std::string cleanAuthor;
    cleanAuthor.reserve(msg.author.length());
    for (char c : msg.author) {
        if (c != '\r' && c != '\n') cleanAuthor.push_back(c);
    }
    while (!cleanAuthor.empty() && (cleanAuthor.front() == ' ' || cleanAuthor.front() == '\t')) cleanAuthor.erase(0, 1);
    while (!cleanAuthor.empty() && (cleanAuthor.back() == ' ' || cleanAuthor.back() == '\t')) cleanAuthor.pop_back();

    if (g_context && g_enableDevCommands.load()) {
        char consoleLine[512];
        std::snprintf(consoleLine, sizeof(consoleLine), "%s%s: %s", cleanPrefix.c_str(), cleanAuthor.c_str(), msg.text.c_str());
        g_context->WriteConsoleMessage(consoleLine);
    }

    if (sendToNative && g_inGameSession.load()) {
        uint8_t defaultPfxByte = (platform == Platform::YouTube) ? 1 : (platform == Platform::Twitch ? 11 : 4);
        uint8_t pfxByte = ColorNameToByte(prefixColorStr, defaultPfxByte);
        uint8_t msgByte = ColorNameToByte(msgColorStr, 0);

        if (g_chatLayoutMode.load() == ChatLayoutMode::TwoLine) {
            // Line 1: [Prefix] Author: (unified on one solid line, no automatic line break)
            std::string line1 = cleanPrefix + cleanAuthor + ":";
            QueueMessageToMainThread(std::move(line1), pfxByte);

            // Line 2: Message body (indented, line breaks preserved if multi-line)
            std::string textRemaining = msg.text;
            size_t pos = 0;
            while ((pos = textRemaining.find('\n')) != std::string::npos) {
                std::string lineSegment = "  " + textRemaining.substr(0, pos);
                while (!lineSegment.empty() && lineSegment.back() == '\r') lineSegment.pop_back();
                QueueMessageToMainThread(std::move(lineSegment), msgByte);
                textRemaining.erase(0, pos + 1);
            }
            if (!textRemaining.empty()) {
                std::string lastSegment = "  " + textRemaining;
                while (!lastSegment.empty() && lastSegment.back() == '\r') lastSegment.pop_back();
                QueueMessageToMainThread(std::move(lastSegment), msgByte);
            }
        } else {
            std::string nativeLine = (!cleanPrefix.empty() ? cleanPrefix : "") + cleanAuthor + ": " + msg.text;
            QueueMessageToMainThread(std::move(nativeLine), pfxByte);
        }
    }

    g_messages.push_back(std::move(msg));
    while (g_messages.size() > 80) {
        g_messages.pop_front();
    }
}

// ============================================================================
// High-Performance Twitch IRC Client
// ============================================================================
static std::atomic<bool> g_twitchRunning{false};
static std::atomic<bool> g_twitchConnected{false};
static std::thread g_twitchThread;
static std::string g_activeTwitchChannel;
static std::string g_twitchOAuthToken;
static std::string g_twitchUsername;
static std::atomic<SOCKET> g_twitchSocket{INVALID_SOCKET};

static void StartTwitchClient(std::string channel);

static bool SendTwitchMessageToChannel(const std::string& text) {
    SOCKET s = g_twitchSocket.load();
    if (s == INVALID_SOCKET || !g_twitchConnected.load()) {
        return false;
    }

    std::string chan;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        chan = g_activeTwitchChannel;
    }
    if (chan.empty()) return false;

    std::string cleanText;
    cleanText.reserve(text.length());
    for (char c : text) {
        if (c != '\r' && c != '\n') cleanText.push_back(c);
    }
    if (cleanText.empty()) return false;

    char msgBuf[1024];
    std::snprintf(msgBuf, sizeof(msgBuf), "PRIVMSG #%s :%s\r\n", chan.c_str(), cleanText.c_str());
    int res = send(s, msgBuf, static_cast<int>(std::strlen(msgBuf)), 0);
    return res != SOCKET_ERROR;
}

static std::string CleanTwitchChannel(std::string input) {
    while (!input.empty() && (input.back() == '/' || input.back() == ' ' || input.back() == '\r' || input.back() == '\n')) {
        input.pop_back();
    }
    size_t q = input.find('?');
    if (q != std::string::npos) input = input.substr(0, q);

    size_t slash = input.rfind('/');
    if (slash != std::string::npos) {
        input = input.substr(slash + 1);
    }
    while (!input.empty() && (input.front() == '#' || input.front() == '@' || input.front() == ' ')) {
        input.erase(0, 1);
    }
    for (char& c : input) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return input;
}

static bool ParseTwitchPrivmsg(const std::string& line, std::string& author, std::string& text) {
    size_t priv = line.find(" PRIVMSG #");
    if (priv == std::string::npos) return false;

    size_t textColon = line.find(" :", priv);
    if (textColon == std::string::npos) return false;
    text = line.substr(textColon + 2);

    if (text.starts_with("\x01""ACTION ") && text.ends_with("\x01")) {
        text = text.substr(8, text.length() - 9);
    }

    size_t dn = line.find("display-name=");
    if (dn != std::string::npos && dn < priv) {
        size_t semi = line.find(';', dn);
        if (semi != std::string::npos && semi > dn + 13) {
            author = line.substr(dn + 13, semi - (dn + 13));
        }
    }

    if (author.empty()) {
        size_t colon = line.find(':');
        size_t excl = line.find('!', colon);
        if (colon != std::string::npos && excl != std::string::npos && excl < priv) {
            author = line.substr(colon + 1, excl - (colon + 1));
        }
    }

    return !author.empty() && !text.empty();
}

static void TwitchWorker(std::string channel) {
    while (g_twitchRunning.load()) {
        g_twitchConnected.store(false);

        const char* ports[] = { "6667", "80" };
        SOCKET sock = INVALID_SOCKET;

        for (const char* portStr : ports) {
            if (!g_twitchRunning.load()) break;

            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;

            if (getaddrinfo("irc.chat.twitch.tv", portStr, &hints, &res) != 0 || !res) {
                continue;
            }

            sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (sock == INVALID_SOCKET) {
                freeaddrinfo(res);
                continue;
            }

            g_twitchSocket.store(sock);

            DWORD timeoutMs = 2500;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

            if (connect(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0) {
                freeaddrinfo(res);
                break;
            }

            g_twitchSocket.store(INVALID_SOCKET);
            closesocket(sock);
            sock = INVALID_SOCKET;
            freeaddrinfo(res);
        }

        if (sock == INVALID_SOCKET) {
            if (!g_twitchRunning.load()) break;
            for (int i = 0; i < 50 && g_twitchRunning.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        g_twitchConnected.store(true);

        std::string oauth;
        std::string nick;
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            oauth = g_twitchOAuthToken;
            nick = g_twitchUsername;
        }

        char handshake[1024];
        if (!oauth.empty()) {
            if (!oauth.starts_with("oauth:")) {
                oauth = "oauth:" + oauth;
            }
            if (nick.empty()) nick = channel;

            std::snprintf(handshake, sizeof(handshake),
                "PASS %s\r\nNICK %s\r\nUSER %s 8 * :%s\r\nCAP REQ :twitch.tv/tags twitch.tv/commands\r\nJOIN #%s\r\n",
                oauth.c_str(), nick.c_str(), nick.c_str(), nick.c_str(), channel.c_str()
            );
        } else {
            const uint32_t fanId = 10000 + (static_cast<uint32_t>(std::time(nullptr)) % 89999);
            std::snprintf(handshake, sizeof(handshake),
                "PASS SCHMOOPIE\r\nNICK justinfan%u\r\nUSER justinfan%u 8 * :justinfan\r\nCAP REQ :twitch.tv/tags twitch.tv/commands\r\nJOIN #%s\r\n",
                fanId, fanId, channel.c_str()
            );
        }
        send(sock, handshake, static_cast<int>(std::strlen(handshake)), 0);

        WriteLog("[Twitch] Connected to IRC server irc.chat.twitch.tv. Joined #%s successfully (Auth: %s)",
            channel.c_str(), oauth.empty() ? "Anonymous justinfan" : "OAuth Token");
        AddMessage(Platform::System, "StreamChat", "Connected to Twitch #" + channel);

        std::string recvBuffer;
        recvBuffer.reserve(8192);
        char tempBuf[2048];

        while (g_twitchRunning.load()) {
            int bytes = recv(sock, tempBuf, sizeof(tempBuf) - 1, 0);
            if (bytes > 0) {
                tempBuf[bytes] = '\0';
                recvBuffer.append(tempBuf, bytes);

                size_t lineStart = 0;
                size_t crlf = 0;
                while ((crlf = recvBuffer.find("\r\n", lineStart)) != std::string::npos) {
                    std::string line = recvBuffer.substr(lineStart, crlf - lineStart);
                    lineStart = crlf + 2;

                    if (line.starts_with("PING")) {
                        std::string pong = "PONG" + line.substr(4) + "\r\n";
                        send(sock, pong.c_str(), static_cast<int>(pong.size()), 0);
                    } else {
                        std::string author, text;
                        if (ParseTwitchPrivmsg(line, author, text)) {
                            AddMessage(Platform::Twitch, std::move(author), std::move(text));
                        }
                    }
                }

                if (lineStart > 0) {
                    recvBuffer.erase(0, lineStart);
                }
            } else if (bytes == 0) {
                break;
            } else {
                int err = WSAGetLastError();
                if (err != WSAETIMEDOUT) {
                    break;
                }
            }
        }

        g_twitchConnected.store(false);
        SOCKET s = g_twitchSocket.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) {
            closesocket(s);
        }

        if (!g_twitchRunning.load()) {
            WriteLog("[Twitch] Disconnected from channel #%s (Client stopped).", channel.c_str());
            AddMessage(Platform::System, "StreamChat", "Disconnected from Twitch #" + channel);
            break;
        }

        WriteLog("[Twitch] Connection lost / closed for #%s. Reconnecting in 5s...", channel.c_str());
        AddMessage(Platform::System, "StreamChat", "Twitch disconnected. Reconnecting in 5s...");
        for (int i = 0; i < 50 && g_twitchRunning.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

static void StopTwitchClient() {
    std::string prevChan = g_activeTwitchChannel;
    if (!prevChan.empty() || g_twitchRunning.load()) {
        WriteLog("[Twitch] Stopping Twitch client (Channel: #%s)...", prevChan.empty() ? "None" : prevChan.c_str());
    }
    g_twitchRunning.store(false);
    SOCKET s = g_twitchSocket.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
    if (g_twitchThread.joinable()) {
        g_twitchThread.join();
    }
    g_activeTwitchChannel.clear();
    g_twitchConnected.store(false);
    if (!prevChan.empty()) {
        WriteLog("[Twitch] Twitch client stopped and disconnected.");
    }
}

static void StartTwitchClient(std::string channel) {
    channel = CleanTwitchChannel(channel);
    if (channel.empty()) return;

    StopTwitchClient();
    WriteLog("[Twitch] Connecting to channel: #%s", channel.c_str());
    g_activeTwitchChannel = channel;
    g_twitchRunning.store(true);
    g_twitchThread = std::thread(TwitchWorker, channel);
}

// ============================================================================
// High-Performance YouTube Client (Zero-Latency Disconnect)
// ============================================================================
static std::atomic<bool> g_ytRunning{false};
static std::atomic<bool> g_ytConnected{false};
static std::thread g_ytThread;
static std::string g_activeYtTarget; 

// Atomic handles for instant zero-latency cancellation
static std::atomic<HINTERNET> g_hYtSession{NULL};
static std::atomic<HINTERNET> g_hYtConnect{NULL};
static std::atomic<HINTERNET> g_hYtActiveRequest{NULL};

// Condition variable for 0ms interruptible sleeps
static std::condition_variable g_ytCv;
static std::mutex g_ytCvMutex;

static std::deque<std::string> g_seenYtMsgIds;
static std::mutex g_seenYtMutex;

static bool IsYtMsgSeen(const std::string& id) {
    if (id.empty()) return false;
    std::lock_guard<std::mutex> lock(g_seenYtMutex);
    for (const auto& existing : g_seenYtMsgIds) {
        if (existing == id) return true;
    }
    g_seenYtMsgIds.push_back(id);
    if (g_seenYtMsgIds.size() > 128) {
        g_seenYtMsgIds.pop_front();
    }
    return false;
}

static const wchar_t* kYtInnertubeKey = L"AIzaSyAO_FJ2SlqU8Q4usACBoqdHbptSemRfVo8";

// Sleeps for up to 'ms', but wakes up in 0.01ms if g_ytRunning becomes false
static bool YtSleep(DWORD ms) {
    std::unique_lock<std::mutex> lock(g_ytCvMutex);
    return !g_ytCv.wait_for(lock, std::chrono::milliseconds(ms), [] {
        return !g_ytRunning.load();
    });
}

static std::string ExtractJsonValue(const std::string& json, std::string_view key, size_t startOffset = 0) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle, startOffset);
    if (pos == std::string::npos) return "";

    size_t colon = json.find(':', pos + needle.length());
    if (colon == std::string::npos) return "";

    size_t firstQuote = json.find('\"', colon + 1);
    if (firstQuote == std::string::npos || (firstQuote - colon) > 10) return "";

    size_t secondQuote = json.find('\"', firstQuote + 1);
    if (secondQuote == std::string::npos) return "";

    return json.substr(firstQuote + 1, secondQuote - (firstQuote + 1));
}

static bool ParseYtMessageBlock(const std::string& block, std::string& outAuthor, std::string& outText, std::string& outId) {
    outAuthor.clear();
    outText.clear();
    outId.clear();

    // 1. Extract Message ID ("id":"...")
    outId = ExtractJsonValue(block, "id");

    // 2. Extract Author Name ("authorName":{"simpleText":"..."})
    size_t authorPos = block.find("\"authorName\"");
    if (authorPos != std::string::npos) {
        outAuthor = ExtractJsonValue(block, "simpleText", authorPos);
    }
    if (outAuthor.empty()) {
        outAuthor = ExtractJsonValue(block, "simpleText");
    }

    // 3. Extract Message Text ("message":{"runs":[{"text":"..."}]})
    size_t msgPos = block.find("\"message\"");
    if (msgPos != std::string::npos) {
        size_t runsEnd = block.find("},\"author", msgPos);
        if (runsEnd == std::string::npos) runsEnd = block.length();
        std::string msgSection = block.substr(msgPos, runsEnd - msgPos);

        size_t tPos = 0;
        while ((tPos = msgSection.find("\"text\"", tPos)) != std::string::npos) {
            std::string fragment = ExtractJsonValue(msgSection, "text", tPos);
            if (!fragment.empty()) {
                if (!outText.empty()) outText += " ";
                outText += fragment;
            }
            tPos += 6;
        }
    }

    if (outText.empty()) {
        outText = ExtractJsonValue(block, "text");
    }

    return !outAuthor.empty() && !outText.empty();
}

static std::string YtExecuteGet(HINTERNET hConnect, const std::wstring& path) {
    if (!hConnect || !g_ytRunning.load()) return "";

    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                        NULL, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) return "";

    g_hYtActiveRequest.store(hReq);

    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(hReq, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));

    std::string response;
    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hReq, NULL)) {
        
        char buf[8192];
        DWORD bytesRead = 0;
        while (g_ytRunning.load() && WinHttpReadData(hReq, buf, sizeof(buf), &bytesRead) && bytesRead > 0) {
            response.append(buf, bytesRead);
        }
    }

    g_hYtActiveRequest.store(NULL);
    WinHttpCloseHandle(hReq);
    return response;
}

static std::string YtExecutePost(HINTERNET hConnect, const std::wstring& path, const std::string& jsonBody) {
    if (!hConnect || !g_ytRunning.load()) return "";

    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
                                        NULL, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) return "";

    g_hYtActiveRequest.store(hReq);

    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(hReq, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));

    LPCWSTR headers = L"Content-Type: application/json\r\n"
                      L"X-YouTube-Client-Name: 1\r\n"
                      L"X-YouTube-Client-Version: 2.20230622.01.00\r\n"
                      L"Origin: https://www.youtube.com\r\n";

    std::string response;
    if (WinHttpSendRequest(hReq, headers, (DWORD)-1, (LPVOID)jsonBody.c_str(), (DWORD)jsonBody.length(), (DWORD)jsonBody.length(), 0) &&
        WinHttpReceiveResponse(hReq, NULL)) {
        
        char buf[8192];
        DWORD bytesRead = 0;
        while (g_ytRunning.load() && WinHttpReadData(hReq, buf, sizeof(buf), &bytesRead) && bytesRead > 0) {
            response.append(buf, bytesRead);
        }
    }

    g_hYtActiveRequest.store(NULL);
    WinHttpCloseHandle(hReq);
    return response;
}

static std::string ResolveLiveVideoId(HINTERNET hConnect, const std::string& channelHandle) {
    std::string handle = channelHandle;
    while (!handle.empty() && (handle.front() == ' ' || handle.front() == '/')) handle.erase(0, 1);
    if (!handle.starts_with("@") && !handle.starts_with("channel/")) {
        handle = "@" + handle;
    }

    std::wstring path = L"/" + std::wstring(handle.begin(), handle.end()) + L"/live";
    std::string html = YtExecuteGet(hConnect, path);
    if (html.empty()) return "";

    const std::string canonNeedle = "rel=\"canonical\" href=\"https://www.youtube.com/watch?v=";
    size_t pos = html.find(canonNeedle);
    if (pos != std::string::npos) {
        size_t start = pos + canonNeedle.length();
        size_t end = html.find('\"', start);
        if (end != std::string::npos && (end - start) == 11) {
            std::string vid = html.substr(start, 11);
            if (html.find("\"isLive\":true") != std::string::npos || html.find("\"isLiveContent\":true") != std::string::npos) {
                return vid;
            }
        }
    }
    return "";
}

static void YouTubeWorker(std::string target) {
    g_ytConnected.store(false);

    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return;
    g_hYtSession.store(hSession);

    WinHttpSetTimeouts(hSession, 1500, 1500, 2000, 2000);

    HINTERNET hConnect = WinHttpConnect(hSession, L"www.youtube.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        HINTERNET s = g_hYtSession.exchange(NULL);
        if (s) WinHttpCloseHandle(s);
        return;
    }
    g_hYtConnect.store(hConnect);

    bool isDirectVideoId = (target.length() == 11 && target.find('/') == std::string::npos && target.find('@') == std::string::npos);
    std::string currentVideoId = isDirectVideoId ? target : "";
    bool announcedWaiting = false;

    while (g_ytRunning.load()) {
        if (!isDirectVideoId) {
            std::string resolved = ResolveLiveVideoId(hConnect, target);
            if (resolved.empty()) {
                if (!announcedWaiting) {
                    announcedWaiting = true;
                    WriteLog("[YouTube] Target '%s' is offline or no active live stream found. Monitoring...", target.c_str());
                    AddMessage(Platform::System, "StreamChat", "YouTube channel is offline. Monitoring for stream...");
                }
                // Sleep for 30s interruptibly (returns immediately if user disconnects)
                if (!YtSleep(30000)) break;
                continue;
            }
            currentVideoId = resolved;
            announcedWaiting = false;
        }

        std::wstring nextPath = L"/youtubei/v1/next?key=" + std::wstring(kYtInnertubeKey);
        std::string initPayload = 
            "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"2.20230622.01.00\",\"hl\":\"en\",\"gl\":\"US\"}},"
            "\"videoId\":\"" + currentVideoId + "\"}";

        std::string nextResp = YtExecutePost(hConnect, nextPath, initPayload);
        std::string continuation;
        size_t chatPos = nextResp.find("liveChatRenderer");
        if (chatPos != std::string::npos) {
            continuation = ExtractJsonValue(nextResp, "continuation", chatPos);
        }
        if (continuation.empty()) {
            continuation = ExtractJsonValue(nextResp, "continuation");
        }

        if (continuation.empty()) {
            if (isDirectVideoId) {
                WriteLog("[YouTube] Live stream offline or chat disabled for video: %s (Target: '%s')", currentVideoId.c_str(), target.c_str());
                AddMessage(Platform::System, "StreamChat", "Stream is offline or chat disabled for: " + currentVideoId);
                break;
            }
            if (!YtSleep(5000)) break;
            continue;
        }

        g_ytConnected.store(true);
        WriteLog("[YouTube] Connected to YouTube Live Chat successfully! (Target: '%s', Video: %s)", target.c_str(), currentVideoId.c_str());
        AddMessage(Platform::System, "StreamChat", "Connected to YouTube Live Chat! (Video: " + currentVideoId + ")");

        std::wstring pollPath = L"/youtubei/v1/live_chat/get_live_chat?key=" + std::wstring(kYtInnertubeKey);
        bool firstRun = true;

        while (g_ytRunning.load()) {
            std::string pollPayload = 
                "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"2.20230622.01.00\"}},"
                "\"continuation\":\"" + continuation + "\"}";

            std::string resp = YtExecutePost(hConnect, pollPath, pollPayload);

            std::string nextCont = ExtractJsonValue(resp, "continuation");
            if (!nextCont.empty()) {
                continuation = nextCont;
            } else {
                break;
            }

            DWORD sleepMs = 3000;
            size_t timeoutPos = resp.find("\"timeoutMs\":");
            if (timeoutPos != std::string::npos) {
                DWORD parsed = std::strtoul(resp.c_str() + timeoutPos + 12, nullptr, 10);
                if (parsed >= 1000 && parsed <= 10000) sleepMs = parsed;
            }

            if (firstRun) {
                firstRun = false;
            } else {
                size_t pos = 0;
                while ((pos = resp.find("liveChatTextMessageRenderer", pos)) != std::string::npos && g_ytRunning.load()) {
                    size_t blockEnd = resp.find("liveChatTextMessageRenderer", pos + 30);
                    if (blockEnd == std::string::npos) blockEnd = resp.length();

                    std::string block = resp.substr(pos, blockEnd - pos);
                    std::string author, text, msgId;

                    if (ParseYtMessageBlock(block, author, text, msgId)) {
                        if (msgId.empty() || !IsYtMsgSeen(msgId)) {
                            AddMessage(Platform::YouTube, std::move(author), std::move(text));
                            if (!YtSleep(40)) break; // Paces message delivery without hitching
                        }
                    }

                    pos = blockEnd;
                }
            }

            if (!YtSleep(sleepMs)) break;
        }

        g_ytConnected.store(false);
        WriteLog("[YouTube] Disconnected from live chat (Target: '%s', Video: %s). %s",
            target.c_str(), currentVideoId.c_str(), g_ytRunning.load() ? "Monitoring for stream..." : "Client stopped.");
        AddMessage(Platform::System, "StreamChat", "YouTube stream disconnected.");

        if (isDirectVideoId) break;
        if (!YtSleep(10000)) break;
    }

    HINTERNET c = g_hYtConnect.exchange(NULL);
    if (c) WinHttpCloseHandle(c);
    HINTERNET s = g_hYtSession.exchange(NULL);
    if (s) WinHttpCloseHandle(s);
}

static void StopYouTubeClient() {
    std::string prevTarget = g_activeYtTarget;
    if (!prevTarget.empty() || g_ytRunning.load()) {
        WriteLog("[YouTube] Stopping YouTube client (Target: '%s')...", prevTarget.empty() ? "None" : prevTarget.c_str());
    }
    g_ytRunning.store(false);

    // 1. Instantly wake up any sleeping worker (0ms delay)
    g_ytCv.notify_all();

    // 2. Instantly cancel any in-flight network requests
    HINTERNET hReq = g_hYtActiveRequest.exchange(NULL);
    if (hReq) WinHttpCloseHandle(hReq);

    HINTERNET hConn = g_hYtConnect.exchange(NULL);
    if (hConn) WinHttpCloseHandle(hConn);

    HINTERNET hSess = g_hYtSession.exchange(NULL);
    if (hSess) WinHttpCloseHandle(hSess);

    // 3. Thread joins with zero lag
    if (g_ytThread.joinable()) {
        g_ytThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(g_seenYtMutex);
        g_seenYtMsgIds.clear();
    }
    g_activeYtTarget.clear();
    g_ytConnected.store(false);
    if (!prevTarget.empty()) {
        WriteLog("[YouTube] YouTube client stopped and disconnected.");
    }
}

static void StartYouTubeClient(std::string target) {
    size_t vPos = target.find("v=");
    if (vPos != std::string::npos) {
        target = target.substr(vPos + 2);
        size_t amp = target.find('&');
        if (amp != std::string::npos) target = target.substr(0, amp);
    }
    size_t livePos = target.find("live/");
    if (livePos != std::string::npos) {
        target = target.substr(livePos + 5);
        size_t q = target.find('?');
        if (q != std::string::npos) target = target.substr(0, q);
    }
    size_t ytCom = target.find("youtube.com/");
    if (ytCom != std::string::npos) {
        target = target.substr(ytCom + 12);
    }

    StopYouTubeClient();
    WriteLog("[YouTube] Connecting to target: '%s'", target.c_str());
    g_activeYtTarget = target;
    g_ytRunning.store(true);
    g_ytThread = std::thread(YouTubeWorker, target);
}

// ============================================================================
// Trampoline Hook for In-Game /tr Chat Replies
// ============================================================================
static uint8_t g_savedBytes[16] = {};
static void* g_trampolinePtr = nullptr;

static void __fastcall DetourSubmitChatHandler(void* widget, D2RStringRef* stringRef, uint64_t channel) noexcept {
    if (stringRef && stringRef->data && stringRef->data[0] == '/') {
        std::string msg(stringRef->data);
        if (msg.starts_with("/tr ")) {
            std::string reply = msg.substr(4);
            bool sent = SendTwitchMessageToChannel(reply);
            if (g_context) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "[Twitch Reply] %s: '%s'", sent ? "Sent" : "Failed", reply.c_str());
                g_context->WriteConsoleMessage(buf);
            }
            stringRef->data = "";
            stringRef->length = 0;
            return;
        }
    }

    if (g_trampolinePtr) {
        auto fnOriginal = reinterpret_cast<D2RSubmitChatHandlerFn>(g_trampolinePtr);
        fnOriginal(widget, stringRef, channel);
    }
}

static void InstallSubmitChatHook() {
    HMODULE hD2R = GetModuleHandleA(NULL);
    if (!hD2R) {
        WriteLog("[StreamChat] Failed to install SubmitChat hook (D2R module handle null).");
        return;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(hD2R);
    uint8_t* target = base + kSubmitChatRva;
    size_t patchSize = 14;

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 32, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        WriteLog("[StreamChat] Failed to protect SubmitChat memory at RVA 0x%X.", kSubmitChatRva);
        return;
    }

    std::memcpy(g_savedBytes, target, patchSize);

    static uint8_t trampoline[64];
    DWORD oldProt2 = 0;
    VirtualProtect(trampoline, sizeof(trampoline), PAGE_EXECUTE_READWRITE, &oldProt2);

    std::memcpy(trampoline, g_savedBytes, patchSize);

    uint8_t* jumpBack = trampoline + patchSize;
    jumpBack[0] = 0x48;
    jumpBack[1] = 0xB8;
    *reinterpret_cast<uint8_t**>(jumpBack + 2) = target + patchSize;
    jumpBack[10] = 0xFF;
    jumpBack[11] = 0xE0;

    g_trampolinePtr = trampoline;

    target[0] = 0x48;
    target[1] = 0xB8;
    *reinterpret_cast<void**>(target + 2) = reinterpret_cast<void*>(DetourSubmitChatHandler);
    target[10] = 0xFF;
    target[11] = 0xE0;
    target[12] = 0x90;
    target[13] = 0x90;

    VirtualProtect(target, 32, oldProtect, &oldProtect);
    WriteLog("[StreamChat] Trampoline hook installed at RVA 0x%X for /tr and /ttv chat replies.", kSubmitChatRva);
}

static void UninstallSubmitChatHook() {
    if (!g_trampolinePtr) return;
    HMODULE hD2R = GetModuleHandleA(NULL);
    if (!hD2R) return;

    uint8_t* target = reinterpret_cast<uint8_t*>(hD2R) + kSubmitChatRva;
    DWORD oldProtect = 0;
    if (VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(target, g_savedBytes, 14);
        VirtualProtect(target, 14, oldProtect, &oldProtect);
    }
    g_trampolinePtr = nullptr;
    WriteLog("[StreamChat] Trampoline hook uninstalled.");
}

// ============================================================================
// DEVELOPER TOOLS & DIAGNOSTICS (Controlled by g_enableDevCommands / TOML)
// ============================================================================

static auto TestChatCommand(
    D2R::Game::Client*,
    const D2RL::ConsoleCommandContext* cmd,
    void*
) noexcept -> D2RL::ConsoleCommandResult {
    if (!cmd || !cmd->plugin) return D2RL::ConsoleCommandResult::Failed;
    if (!g_enableDevCommands.load()) {
        WriteDevLog("[DevCommand] /testchat submitted -> BLOCKED (Dev commands disabled in TOML / runtime)");
        cmd->plugin->WriteConsoleMessage("[StreamChat] Dev commands are disabled. Set 'enable_dev_commands = true' in d2rl-streamchat.toml or run '/streamchat dev on' to enable.");
        return D2RL::ConsoleCommandResult::Handled;
    }
    char user[64] {};
    char text[256] {};
    if (std::sscanf((cmd->args ? cmd->args : ""), "%63s %255[^\n]", user, text) >= 2) {
        WriteDevLog("[DevCommand] /testchat submitted (user='%s', text='%s') -> Result: Dispatched to chat", user, text);
        AddMessage(Platform::Twitch, user, text, true);
        return D2RL::ConsoleCommandResult::Handled;
    }
    WriteDevLog("[DevCommand] /testchat submitted with invalid arguments: '%s'", cmd->args ? cmd->args : "");
    cmd->plugin->WriteConsoleMessage("Usage: /testchat <user> <message>");
    return D2RL::ConsoleCommandResult::InvalidArguments;
}

static void RunColorPalettePreview(const D2RL::ConsoleCommandContext* cmd) {
    if (!cmd || !cmd->plugin) return;
    const char* names[] = {
        "white (0)", "red (1)", "green (2)", "blue (3)",
        "gold (4)", "gray (5)", "black (6)", "tan (7)",
        "orange (8)", "yellow (9)", "dark_green (10)", "purple (11)",
        "light_green (12)"
    };
    for (uint8_t i = 0; i < 13; ++i) {
        char tb[128];
        std::snprintf(tb, sizeof(tb), "[Color %u: %s] Chat line preview in this color", i, names[i]);
        SendToNativeChat(tb, i);
    }
    cmd->plugin->WriteConsoleMessage("[StreamChat] Pushed all 13 color previews to in-game chat.");
}

static void RunColorDiagnosticTest(const std::string& mode, const D2RL::ConsoleCommandContext* cmd) {
    if (!cmd || !cmd->plugin) return;

    std::string m = mode;
    std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (m.empty() || m == "colors" || m == "bytes" || m == "palette") {
        RunColorPalettePreview(cmd);
        return;
    }

    if (!g_enableDevCommands.load()) {
        WriteDevLog("[DevCommand] /testcolor submitted -> BLOCKED (Dev commands disabled in TOML / runtime)");
        cmd->plugin->WriteConsoleMessage("[StreamChat] Advanced dev diagnostic mode is disabled. Set 'enable_dev_commands = true' in d2rl-streamchat.toml or run 'streamchat dev on' to enable.");
        return;
    }

    WriteDevLog("=================================================");
    WriteDevLog("START COLOR DIAGNOSTIC TEST (Mode: '%s')", m.c_str());
    WriteDevLog("=================================================");

    auto testAndLog = [&](const char* label, const char* text, uint8_t colorByte) {
        std::string hexDump;
        for (const char* p = text; *p; ++p) {
            char hb[8];
            std::snprintf(hb, sizeof(hb), "%02X ", static_cast<unsigned char>(*p));
            hexDump += hb;
        }
        WriteDevLog("[Test: %s] text='%s' | colorByte=%u | hex: %s", label, text, (unsigned)colorByte, hexDump.c_str());

        bool ok = SendToNativeChat(text, colorByte);
        WriteDevLog("[Test: %s] SendToNativeChat Result: %s", label, ok ? "SUCCESS" : "FAILED");
        char consoleMsg[256];
        std::snprintf(consoleMsg, sizeof(consoleMsg), "[Test: %s] %s (colorByte %u)", label, ok ? "Pushed OK" : "Failed", (unsigned)colorByte);
        cmd->plugin->WriteConsoleMessage(consoleMsg);
    };

    if (m == "utf8" || m == "d2r" || m == "all") {
        // UTF-8 encoded Diablo 2 legacy color codes (0xC3 0xBF is UTF-8 for ÿ)
        const char utf8Legacy[] = "\xC3\xBF" "c;[Twitch] " "\xC3\xBF" "c4Streamer: " "\xC3\xBF" "c0UTF-8 ÿc test";
        testAndLog("UTF8-ÿc", utf8Legacy, 0);

        const char utf8Short[] = "\xC3\xBF" "1[Twitch] " "\xC3\xBF" "4Streamer: " "\xC3\xBF" "0UTF-8 ÿ# test";
        testAndLog("UTF8-ÿ#", utf8Short, 0);
    }

    if (m == "pipe" || m == "all") {
        // Blizzard pipe format: |c<AARRGGBB>text|r
        const char* pipeMsg = "|cFFBF00FF[Twitch]|r |cFFFFD700Streamer:|r |cFFFFFFFFPipe format test|r";
        testAndLog("Pipe", pipeMsg, 0);
    }

    if (m == "section" || m == "all") {
        // Section sign § (0xC2 0xA7 in UTF-8)
        const char secMsg[] = "\xC2\xA7" "c;[Twitch] " "\xC2\xA7" "c4Streamer: " "\xC2\xA7" "c0Section § test";
        testAndLog("Section-§", secMsg, 0);
    }

    if (m == "legacy" || m == "d2" || m == "all") {
        // Raw byte 0xFF legacy D2 color codes: \xFF c <char>
        const char rawLegacy[] = "\xFF" "c;[Twitch] " "\xFF" "c4Streamer: " "\xFF" "c0Raw 0xFF legacy test";
        testAndLog("Legacy0xFF", rawLegacy, 0);
    }

    if (m == "xml" || m == "tag" || m == "all") {
        // XML / HTML tag format
        const char* xmlMsg = "<color=purple>[Twitch]</color> <color=gold>Streamer:</color> XML tag test";
        testAndLog("XmlTag", xmlMsg, 0);

        const char* xmlHex = "<color=#BF00FF>[Twitch]</color> <color=#FFD700>Streamer:</color> Hex XML test";
        testAndLog("XmlHex", xmlHex, 0);

        const char* fontTag = "<font color=\"#BF00FF\">[Twitch]</font> <font color=\"#FFD700\">Streamer:</font> Font tag test";
        testAndLog("FontTag", fontTag, 0);
    }

    if (m == "bracket" || m == "all") {
        // BBCode bracket format
        const char* bbMsg = "[c:purple][Twitch][/c] [c:gold]Streamer:[/c] Bracket test";
        testAndLog("BBCode", bbMsg, 0);
    }

    if (m == "ansi" || m == "escape" || m == "all") {
        // ANSI escape sequence
        const char* ansiMsg = "\x1B[35m[Twitch] \x1B[33mStreamer: \x1B[37mANSI ESC test\x1B[0m";
        testAndLog("ANSI-ESC", ansiMsg, 0);
    }

    if (m == "twoline" || m == "all") {
        // Two-line layout: prefix/author on line 1 in prefix color (purple=11), message on line 2 in message color (white=0)
        testAndLog("TwoLine-1", "[Twitch] Streamer:", 11);
        testAndLog("TwoLine-2", "   Two-line layout message test", 0);
    }

    if (m == "colors" || m == "bytes" || m == "palette" || m == "all") {
        const char* names[] = {
            "white (0)", "red (1)", "green (2)", "blue (3)",
            "gold (4)", "gray (5)", "black (6)", "tan (7)",
            "orange (8)", "yellow (9)", "dark_green (10)", "purple (11)",
            "light_green (12)"
        };
        for (uint8_t i = 0; i < 13; ++i) {
            char tb[128];
            std::snprintf(tb, sizeof(tb), "[Color %u: %s] Chat line preview in this color", i, names[i]);
            testAndLog(names[i], tb, i);
        }
    }

    if (m == "log") {
        std::wstring logPath = GetPluginLogFilePath();
        FILE* f = _wfopen(logPath.c_str(), L"r");
        if (!f) {
            f = fopen("d2rl-streamchat.log", "r");
        }
        if (!f) {
            cmd->plugin->WriteConsoleMessage("[StreamChat] Log file 'd2rl-streamchat.log' not found or empty.");
            return;
        }
        std::vector<std::string> lines;
        char lineBuf[512];
        while (std::fgets(lineBuf, sizeof(lineBuf), f)) {
            size_t l = std::strlen(lineBuf);
            while (l > 0 && (lineBuf[l-1] == '\r' || lineBuf[l-1] == '\n')) {
                lineBuf[--l] = '\0';
            }
            if (l > 0) {
                lines.emplace_back(lineBuf);
            }
        }
        std::fclose(f);
        cmd->plugin->WriteConsoleMessage("[StreamChat Log - Last entries]:");
        size_t start = lines.size() > 15 ? lines.size() - 15 : 0;
        for (size_t i = start; i < lines.size(); ++i) {
            cmd->plugin->WriteConsoleMessage(lines[i].c_str());
        }
        return;
    }

    WriteDevLog("END COLOR DIAGNOSTIC TEST");
    WriteDevLog("=================================================");
    cmd->plugin->WriteConsoleMessage("[StreamChat] Diagnostic test complete. Results written to 'd2rl-streamchat.log'.");
}

static auto TestColorCommand(
    D2R::Game::Client*,
    const D2RL::ConsoleCommandContext* cmd,
    void*
) noexcept -> D2RL::ConsoleCommandResult {
    if (!cmd || !cmd->plugin) return D2RL::ConsoleCommandResult::Failed;
    const std::string_view args(cmd->args ? cmd->args : "");
    std::string mode(args);
    while (!mode.empty() && mode.front() == ' ') mode.erase(0, 1);
    while (!mode.empty() && mode.back() == ' ') mode.pop_back();

    if (mode.starts_with("color ")) {
        mode = mode.substr(6);
        while (!mode.empty() && mode.front() == ' ') mode.erase(0, 1);
    } else if (mode == "color") {
        mode = "colors";
    }

    WriteDevLog("[Command] /testcolor submitted with mode='%s'", mode.c_str());
    RunColorDiagnosticTest(mode, cmd);
    return D2RL::ConsoleCommandResult::Handled;
}

static auto TestClientChatCommand(
    D2R::Game::Client*,
    const D2RL::ConsoleCommandContext* cmd,
    void*
) noexcept -> D2RL::ConsoleCommandResult {
    if (!cmd || !cmd->plugin) return D2RL::ConsoleCommandResult::Failed;
    if (!g_enableDevCommands.load()) {
        WriteDevLog("[DevCommand] /testclientchat submitted -> BLOCKED (Dev commands disabled in TOML / runtime)");
        cmd->plugin->WriteConsoleMessage("[StreamChat] Dev commands are disabled. Set 'enable_dev_commands = true' in d2rl-streamchat.toml or run 'streamchat dev on' to enable.");
        return D2RL::ConsoleCommandResult::Handled;
    }
    const char* text = (cmd->args && cmd->args[0]) ? cmd->args : "[Test] Client-Side Native Chat";

    bool ok = PushClientChatDirect(text, 4);
    WriteDevLog("[DevCommand] /testclientchat submitted (text='%s') -> Result: %s", text, ok ? "PushClientChatEntry SUCCESS" : "PushClientChatEntry FAILED");
    cmd->plugin->WriteConsoleMessage(ok ? 
        "[StreamChat] PushClientChatEntry called successfully!" : 
        "[StreamChat] PushClientChatEntry failed or function not found.");
    return D2RL::ConsoleCommandResult::Handled;
}

// ============================================================================
// Configuration & TOML Persistence
// ============================================================================
static std::string ExtractTomlValue(const std::string& text, const std::string& key) {
    size_t pos = 0;
    while (pos < text.length()) {
        size_t lineEnd = text.find('\n', pos);
        if (lineEnd == std::string::npos) lineEnd = text.length();

        std::string line = text.substr(pos, lineEnd - pos);
        size_t hashPos = line.find('#');
        if (hashPos != std::string::npos) line = line.substr(0, hashPos);

        size_t eqPos = line.find('=');
        if (eqPos != std::string::npos) {
            std::string k = line.substr(0, eqPos);
            while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.erase(0, 1);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();

            if (k == key) {
                std::string v = line.substr(eqPos + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
                while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();

                if (v.length() >= 2 && v.front() == '"' && v.back() == '"') {
                    return v.substr(1, v.length() - 2);
                }
                return v;
            }
        }
        pos = lineEnd + 1;
    }
    return "";
}

struct FeaturedCreator {
    const char* twitch;
    const char* youtube;
};

static const FeaturedCreator kFeaturedCreators[] = {
    { "ghostdragon14712",   "@-Ghost.Dragon" },
    { "mrllamasc",          "@MrLlamaSC" },
    { "dbrunski125",        "@Dbrunski125" },
    { "macrobioboi",        "@MacroBioBoi" },
    { "coooley",            "@Coooley" },
    { "kano",               "@kano_au" },
    { "sweet_phil",         "@SweetPhil" },
    { "gingergamingmentor", "@GingerGamingMentor" },
    { "wudijo",             "@wudijo" },
    { "raxxanterax",        "@Raxxanterax" },
    { "rhykker",            "@Rhykker" },
    { "rob2628",            "@Rob2628" },
    { "btneandertha1",      "@BTNeanderthal" },
    { "luckyghost",         "@LuckyGhost" },
    { "donthecrown",        "@DonTheCrown" },
    { "echohack",           "@echohack" },
    { "lexyu",              "@Lexyu" },
    { "barricade",          "@barricadettv" },
    { "annacakelive",       "@annacakelive" },
    { "darkhumility",       "@darkhumility4915" },
    { "carbotanimations",   "@CarbotAnimations" },
    { "scottieknowzmods",   "@ScottieKnowz" },
    { "",                   "@Dimentio_" },
    { "",                   "@RegularSchmuck" },
    { "hellomischa",        "@hellomischa" },
    { "Zeegers",            "@ZeegersTV" },
    { "indrek",             "@IndrekLeede" },
    { "kvothed2",           "@KvotheD" },
    { "",                   "@calef_xd" },
    { "",                   "@theicemanye" },
    { "",                   "@Futureal81" },
    { "looter_game",        "@looter_game" },
    { "zarfen",             "@zarfen" },
    { "ilovemfTV",          "@ilovemf" },
    { "xtimus",             "@xtimus" }
};

static const FeaturedCreator& GetRandomFeaturedCreator() {
    static std::atomic<size_t> counter{0};
    size_t count = sizeof(kFeaturedCreators) / sizeof(kFeaturedCreators[0]);
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    size_t idx = (static_cast<size_t>(now) + counter.fetch_add(1)) % count;
    return kFeaturedCreators[idx];
}

static const FeaturedCreator& GetRandomTwitchCreator() {
    size_t count = sizeof(kFeaturedCreators) / sizeof(kFeaturedCreators[0]);
    static std::atomic<size_t> counter{0};
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    size_t start = (static_cast<size_t>(now) + counter.fetch_add(1)) % count;
    for (size_t i = 0; i < count; ++i) {
        const auto& c = kFeaturedCreators[(start + i) % count];
        if (c.twitch && c.twitch[0] != '\0') return c;
    }
    return kFeaturedCreators[0];
}

static const FeaturedCreator& GetRandomYouTubeCreator() {
    size_t count = sizeof(kFeaturedCreators) / sizeof(kFeaturedCreators[0]);
    static std::atomic<size_t> counter{0};
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    size_t start = (static_cast<size_t>(now) + counter.fetch_add(1)) % count;
    for (size_t i = 0; i < count; ++i) {
        const auto& c = kFeaturedCreators[(start + i) % count];
        if (c.youtube && c.youtube[0] != '\0') return c;
    }
    return kFeaturedCreators[0];
}

static std::string GenerateDefaultTomlString(size_t creatorIndex = static_cast<size_t>(-1)) {
    (void)creatorIndex;
    const auto& twCreator = GetRandomTwitchCreator();
    const auto& ytCreator = GetRandomYouTubeCreator();
    std::string twExample = twCreator.twitch;
    std::string ytExample = ytCreator.youtube;

    std::string out;
    out.reserve(2048);
    out += "# ==============================================================================\n";
    out += "# D2R Stream Chat Configuration\n";
    out += "# Injects Twitch and YouTube live chat directly into D2R native in-game chat.\n";
    out += "# ==============================================================================\n\n";
    out += "[general]\n";
    out += "# Chat layout style:\n";
    out += "# \"single\"  = Prefix, name, and message on 1 line (uses prefix_color).\n";
    out += "# \"twoline\" = Prefix + name on line 1 (prefix_color), message indented on line 2 (message_color).\n";
    out += "#     This allows distinct colors for name and message with the tradeoff of taking more chat space.\n";
    out += "layout = \"twoline\"\n\n";
    out += "# Filter out automated bot commands starting with '!' (true = hide, false = show)\n";
    out += "filter_bot_commands = true\n\n";
    out += "# Platform tag style: \"ttv\", \"twitch\", or \"none\"\n";
    out += "prefix_mode = \"twitch\"\n\n";
    out += "# YouTube platform tag style: \"yt\", \"youtube\", or \"none\"\n";
    out += "youtube_prefix_mode = \"youtube\"\n\n";
    out += "# Enable developer diagnostic and testing commands (/testcolor, /testchat, etc.)\n";
    out += "# Anyone can set this to true to help with troubleshooting and testing without recompiling!\n";
    out += "enable_dev_commands = false\n\n";
    out += "[twitch]\n";
    out += "enabled = true\n";
    out += "# Your Twitch channel name (e.g. \"";
    out += twExample;
    out += "\")\n";
    out += "channel = \"\"\n";
    out += "username = \"\"\n";
    out += "# Optional OAuth token to send replies back to Twitch using /tr.\n";
    out += "# If left blank, you will still receive live chat in read-only mode anonymously.\n";
    out += "# (\"https://twitchtokengenerator.com/\")\n";
    out += "oauth = \"\"\n";
    out += "# Color for the [Twitch] tag and username\n";
    out += "# Working options: white, red, green, blue, gold, gray, tan, orange, yellow, dark_green, purple, light_green\n";
    out += "# Note: \"black\" is supported but hard to see against the dark background.\n";
    out += "prefix_color = \"purple\"\n";
    out += "# Twitch message text color ( This is ignored if you are in layout single mode. )\n";
    out += "message_color = \"white\"\n\n";
    out += "[youtube]\n";
    out += "enabled = true\n";
    out += "# Your YouTube handle or channel name (e.g. \"";
    out += ytExample;
    out += "\")\n";
    out += "# Automatically detects when you go live without needing a video ID!\n";
    out += "channel = \"\"\n";
    out += "# Color for the [YT] tag and username\n";
    out += "# Working options: red, gold, white, green, blue, gray, tan, orange, yellow, dark_green, purple, light_green\n";
    out += "# Note: \"black\" is supported but hard to see against the dark background.\n";
    out += "prefix_color = \"red\"\n";
    out += "# YouTube message text color ( This is ignored if you are in layout single mode. )\n";
    out += "message_color = \"white\"\n";
    return out;
}

static bool HasTomlKey(const std::string& section, const std::string& key) {
    size_t pos = 0;
    while (pos < section.length()) {
        size_t lineEnd = section.find('\n', pos);
        if (lineEnd == std::string::npos) lineEnd = section.length();
        std::string line = section.substr(pos, lineEnd - pos);
        size_t hashPos = line.find('#');
        if (hashPos != std::string::npos) line = line.substr(0, hashPos);
        size_t eqPos = line.find('=');
        if (eqPos != std::string::npos) {
            std::string k = line.substr(0, eqPos);
            while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.erase(0, 1);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            if (k == key) return true;
        }
        pos = lineEnd + 1;
    }
    return false;
}

static std::string GetTomlSection(const std::string& text, const std::string& sectionName) {
    std::string tag = "[" + sectionName + "]";
    size_t s = text.find(tag);
    if (s == std::string::npos) return "";
    size_t nextS = text.find("\n[", s + tag.length());
    if (nextS == std::string::npos) return text.substr(s);
    return text.substr(s, nextS - s);
}

static bool UpdateTomlKeyValue(std::string& tomlText, const std::string& sectionName, const std::string& key, const std::string& newValue) {
    size_t searchStart = 0;
    size_t searchEnd = tomlText.length();

    if (!sectionName.empty()) {
        std::string secTag = "[" + sectionName + "]";
        size_t s = tomlText.find(secTag);
        if (s != std::string::npos) {
            searchStart = s;
            size_t nextS = tomlText.find("\n[", s + secTag.length());
            if (nextS != std::string::npos) {
                searchEnd = nextS;
            }
        }
    }

    size_t pos = searchStart;
    while (pos < searchEnd) {
        size_t lineEnd = tomlText.find('\n', pos);
        if (lineEnd == std::string::npos || lineEnd > searchEnd) lineEnd = searchEnd;

        std::string line = tomlText.substr(pos, lineEnd - pos);
        size_t hashPos = line.find('#');
        std::string codeLine = (hashPos != std::string::npos) ? line.substr(0, hashPos) : line;

        size_t eqPos = codeLine.find('=');
        if (eqPos != std::string::npos) {
            std::string k = codeLine.substr(0, eqPos);
            while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.erase(0, 1);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();

            if (k == key) {
                std::string comment = (hashPos != std::string::npos) ? line.substr(hashPos) : "";
                std::string leadingSpaces;
                for (char c : line) {
                    if (c == ' ' || c == '\t') leadingSpaces += c;
                    else break;
                }

                std::string newLine = leadingSpaces + key + " = " + newValue;
                if (!comment.empty()) {
                    newLine += "  " + comment;
                }

                tomlText.replace(pos, lineEnd - pos, newLine);
                return true;
            }
        }
        pos = lineEnd + 1;
    }

    if (!sectionName.empty()) {
        std::string secTag = "[" + sectionName + "]";
        size_t s = tomlText.find(secTag);
        if (s != std::string::npos) {
            size_t insertPos = tomlText.find('\n', s);
            if (insertPos != std::string::npos) {
                std::string toInsert = "\n" + key + " = " + newValue;
                tomlText.insert(insertPos, toInsert);
                return true;
            }
        }
    }

    tomlText += "\n" + key + " = " + newValue + "\n";
    return true;
}

static std::string ReadPluginConfig(const D2RL::PluginContext* context) {
    if (!context) return "";
    std::vector<char> configText(16384, 0);
    uint32_t reqSize = 0;
    if (!context->ReadConfig(configText.data(), static_cast<uint32_t>(configText.size()), &reqSize)) {
        if (reqSize > configText.size()) {
            configText.assign(reqSize + 1, 0);
            if (!context->ReadConfig(configText.data(), static_cast<uint32_t>(configText.size()), &reqSize)) {
                return "";
            }
        } else {
            return "";
        }
    }
    return std::string(configText.data());
}

static void SaveSettingToToml(const std::string& section, const std::string& key, const std::string& valueStr) {
    if (!g_context) return;
    std::string cfg = ReadPluginConfig(g_context);
    if (cfg.empty()) {
        cfg = GenerateDefaultTomlString();
    }

    if (UpdateTomlKeyValue(cfg, section, key, valueStr)) {
        (void)g_context->WriteConfig(cfg.c_str());
        WriteLog("[StreamChat] Saved to TOML: [%s] %s = %s", section.empty() ? "general" : section.c_str(), key.c_str(), valueStr.c_str());
    }
}

static bool CheckAndStripSetToToml(std::string& arg) {
    while (!arg.empty() && (arg.back() == ' ' || arg.back() == '\t')) arg.pop_back();
    std::string lower = arg;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    
    if (lower.ends_with(" settotoml")) {
        arg.erase(arg.length() - 10);
        while (!arg.empty() && (arg.back() == ' ' || arg.back() == '\t')) arg.pop_back();
        return true;
    }
    return false;
}

static void SyncAndMigrateTomlConfig(const D2RL::PluginContext* context, std::string& cfg);

static void RotateTomlExamples(const D2RL::PluginContext* context) {
    if (!context) return;
    std::lock_guard<std::mutex> lock(g_configMutex);
    
    std::string cfg = ReadPluginConfig(context);
    if (cfg.empty()) {
        SyncAndMigrateTomlConfig(context, cfg);
        return;
    }

    const auto& twCreator = GetRandomTwitchCreator();
    const auto& ytCreator = GetRandomYouTubeCreator();

    std::string twitchComment = "# Your Twitch channel name (e.g. \"" + std::string(twCreator.twitch) + "\")";
    std::string ytComment = "# Your YouTube handle or channel name (e.g. \"" + std::string(ytCreator.youtube) + "\")";

    bool modified = false;
    std::string newCfg;
    newCfg.reserve(cfg.size() + 128);

    size_t pos = 0;
    while (pos < cfg.length()) {
        size_t lineEnd = cfg.find('\n', pos);
        std::string line = (lineEnd == std::string::npos) ? cfg.substr(pos) : cfg.substr(pos, lineEnd - pos);
        
        std::string trimmed = line;
        if (!trimmed.empty() && trimmed.back() == '\r') {
            trimmed.pop_back();
        }

        if (trimmed.starts_with("# Your Twitch channel name (e.g.")) {
            if (trimmed != twitchComment) {
                line = twitchComment;
                modified = true;
            }
        } else if (trimmed.starts_with("# Your YouTube handle or channel name (e.g.")) {
            if (trimmed != ytComment) {
                line = ytComment;
                modified = true;
            }
        }

        newCfg += line;
        if (lineEnd != std::string::npos) {
            newCfg += "\n";
            pos = lineEnd + 1;
        } else {
            break;
        }
    }

    if (modified) {
        (void)context->WriteConfig(newCfg.c_str());
        WriteDevLog("[StreamChat] Rotated TOML example comments (Twitch: '%s' | YouTube: '%s')", twCreator.twitch, ytCreator.youtube);
    }
}

static void SyncAndMigrateTomlConfig(const D2RL::PluginContext* context, std::string& cfg) {
    if (cfg.empty()) {
        cfg = GenerateDefaultTomlString();
        if (context) {
            (void)context->EnsureConfig(cfg.c_str());
            (void)context->WriteConfig(cfg.c_str());
        }
        WriteLog("[StreamChat] Created new default configuration file with featured creator examples.");
        return;
    }

    std::string additions;
    int missingCount = 0;

    if (!HasTomlKey(cfg, "layout")) {
        additions += "\n# Chat layout style: \"single\" or \"twoline\"\nlayout = \"twoline\"\n";
        missingCount++;
    }
    if (!HasTomlKey(cfg, "filter_bot_commands")) {
        additions += "# Filter out automated bot commands starting with '!'\nfilter_bot_commands = true\n";
        missingCount++;
    }
    if (!HasTomlKey(cfg, "prefix_mode") && !HasTomlKey(cfg, "twitch_prefix")) {
        additions += "# Platform tag style: \"ttv\", \"twitch\", or \"none\"\nprefix_mode = \"twitch\"\n";
        missingCount++;
    }
    if (!HasTomlKey(cfg, "youtube_prefix_mode") && !HasTomlKey(cfg, "youtube_prefix") && !HasTomlKey(cfg, "yt_prefix")) {
        additions += "# YouTube platform tag style: \"yt\", \"youtube\", or \"none\"\nyoutube_prefix_mode = \"youtube\"\n";
        missingCount++;
    }
    if (!HasTomlKey(cfg, "enable_dev_commands") && !HasTomlKey(cfg, "dev_commands")) {
        additions += "# Enable developer diagnostic and testing commands\nenable_dev_commands = false\n";
        missingCount++;
    }

    // Check [twitch] section
    std::string twSection = GetTomlSection(cfg, "twitch");
    if (!twSection.empty()) {
        if (!HasTomlKey(twSection, "prefix_color") && !HasTomlKey(twSection, "name_color") && !HasTomlKey(twSection, "color") && !HasTomlKey(cfg, "twitch_prefix_color")) {
            additions += "\n[twitch]\nprefix_color = \"purple\"\n";
            missingCount++;
        }
        if (!HasTomlKey(twSection, "message_color") && !HasTomlKey(cfg, "twitch_message_color")) {
            if (additions.rfind("[twitch]\n") == std::string::npos) additions += "\n[twitch]\n";
            additions += "message_color = \"white\"\n";
            missingCount++;
        }
    }

    // Check [youtube] section
    std::string ytSection = GetTomlSection(cfg, "youtube");
    if (!ytSection.empty()) {
        if (!HasTomlKey(ytSection, "prefix_color") && !HasTomlKey(ytSection, "name_color") && !HasTomlKey(ytSection, "color") && !HasTomlKey(cfg, "youtube_prefix_color")) {
            additions += "\n[youtube]\nprefix_color = \"red\"\n";
            missingCount++;
        }
        if (!HasTomlKey(ytSection, "message_color") && !HasTomlKey(cfg, "youtube_message_color")) {
            if (additions.rfind("[youtube]\n") == std::string::npos) additions += "\n[youtube]\n";
            additions += "message_color = \"white\"\n";
            missingCount++;
        }
    }

    if (missingCount > 0) {
        cfg += "\n# ------------------------------------------------------------------------------\n";
        cfg += "# Automatically appended missing configuration options (User settings preserved)\n";
        cfg += "# ------------------------------------------------------------------------------\n";
        cfg += additions;

        if (context) {
            (void)context->WriteConfig(cfg.c_str());
        }
        WriteLog("[StreamChat] Config migration: Added %d missing configuration options without modifying existing user settings.", missingCount);
    }
}

// ============================================================================
// User Console Commands
// ============================================================================
static auto StreamChatCommand(
    D2R::Game::Client*,
    const D2RL::ConsoleCommandContext* cmd,
    void*
) noexcept -> D2RL::ConsoleCommandResult {
    if (!cmd || !cmd->plugin) return D2RL::ConsoleCommandResult::Failed;
    std::string args(cmd->args ? cmd->args : "");
    bool saveToToml = CheckAndStripSetToToml(args);

    if (args.empty() || args == "status") {
        char buf[512];
        std::lock_guard<std::mutex> lock(g_configMutex);
        std::lock_guard<std::mutex> lk(g_colorMutex);
        const char* layoutStr = (g_chatLayoutMode.load() == ChatLayoutMode::TwoLine) ? "TwoLine" : "SingleLine";
        const char* devStr = g_enableDevCommands.load() ? "ON" : "OFF";
        const char* twitchPfx = (g_prefixMode.load() == PrefixMode::TTV) ? "[TTV]" :
                                (g_prefixMode.load() == PrefixMode::Twitch) ? "[Twitch]" : "None";
        const char* ytPfx = (g_ytPrefixMode.load() == YtPrefixMode::YT) ? "[YT]" :
                            (g_ytPrefixMode.load() == YtPrefixMode::YouTube) ? "[YouTube]" : "None";
        std::snprintf(buf, sizeof(buf),
            "[StreamChat] InGame: %s | Layout: %s | Dev: %s | Twitch: %s (%s, Pfx: %s [%s], Msg: [%s]) | YouTube: %s (%s, Pfx: %s [%s], Msg: [%s])",
            g_inGameSession.load() ? "Yes" : "No",
            layoutStr,
            devStr,
            g_activeTwitchChannel.empty() ? "None" : g_activeTwitchChannel.c_str(),
            g_twitchConnected.load() ? "Connected" : "Disconnected",
            twitchPfx,
            g_twitchPrefixColor.c_str(),
            g_twitchMessageColor.c_str(),
            g_activeYtTarget.empty() ? "None" : g_activeYtTarget.c_str(),
            g_ytConnected.load() ? "Connected" : "Monitoring",
            ytPfx,
            g_youtubePrefixColor.c_str(),
            g_youtubeMessageColor.c_str()
        );
        cmd->plugin->WriteConsoleMessage(buf);
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args.starts_with("dev ")) {
        std::string mode(args.substr(4));
        while (!mode.empty() && mode.front() == ' ') mode.erase(0, 1);
        if (mode == "on") {
            g_enableDevCommands.store(true);
            if (saveToToml) SaveSettingToToml("general", "enable_dev_commands", "true");
            WriteDevLog("[DevCommand] /streamchat dev on submitted -> Dev commands ENABLED");
            cmd->plugin->WriteConsoleMessage(saveToToml ? 
                "[StreamChat] Dev commands: ENABLED (Saved to TOML)" : 
                "[StreamChat] Dev commands: ENABLED");
        } else if (mode == "off") {
            g_enableDevCommands.store(false);
            if (saveToToml) SaveSettingToToml("general", "enable_dev_commands", "false");
            cmd->plugin->WriteConsoleMessage(saveToToml ? 
                "[StreamChat] Dev commands: DISABLED (Saved to TOML)" : 
                "[StreamChat] Dev commands: DISABLED");
        } else {
            cmd->plugin->WriteConsoleMessage("Usage: streamchat dev <on | off> [settotoml]");
        }
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args.starts_with("layout ")) {
        std::string mode(args.substr(7));
        while (!mode.empty() && mode.front() == ' ') mode.erase(0, 1);
        if (mode == "twoline") {
            g_chatLayoutMode.store(ChatLayoutMode::TwoLine);
            if (saveToToml) SaveSettingToToml("general", "layout", "\"twoline\"");
            WriteDevLog("[Command] /streamchat layout twoline submitted -> Layout set to TwoLine");
            cmd->plugin->WriteConsoleMessage(saveToToml ? 
                "[StreamChat] Chat Layout set to: TwoLine (Saved to TOML)" : 
                "[StreamChat] Chat Layout set to: TwoLine (Prefix+Name on line 1, Message on line 2)");
        } else if (mode == "single") {
            g_chatLayoutMode.store(ChatLayoutMode::SingleLine);
            if (saveToToml) SaveSettingToToml("general", "layout", "\"single\"");
            WriteDevLog("[Command] /streamchat layout single submitted -> Layout set to SingleLine");
            cmd->plugin->WriteConsoleMessage(saveToToml ? 
                "[StreamChat] Chat Layout set to: SingleLine (Saved to TOML)" : 
                "[StreamChat] Chat Layout set to: SingleLine (Prefix+Name + Message on 1 line)");
        } else {
            cmd->plugin->WriteConsoleMessage("Usage: streamchat layout <single | twoline> [settotoml]");
        }
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args == "test") {
        if (!g_enableDevCommands.load()) {
            WriteDevLog("[DevCommand] /streamchat test submitted -> BLOCKED (Dev commands disabled in TOML / runtime)");
            cmd->plugin->WriteConsoleMessage("[StreamChat] Dev commands are disabled. Set 'enable_dev_commands = true' in d2rl-streamchat.toml or run 'streamchat dev on' to enable.");
            return D2RL::ConsoleCommandResult::Handled;
        }
        WriteDevLog("[DevCommand] /streamchat test submitted -> Result: Dispatched Twitch & YouTube sample messages (Layout: %s)",
            (g_chatLayoutMode.load() == ChatLayoutMode::TwoLine) ? "TwoLine" : "SingleLine");
        AddMessage(Platform::Twitch, "TwitchUser", "Testing Twitch prefix and message colors!", true);
        AddMessage(Platform::YouTube, "YouTubeUser", "Testing YouTube prefix and message colors!", true);
        cmd->plugin->WriteConsoleMessage("[StreamChat] Sent test messages for Twitch and YouTube to native chat.");
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args.starts_with("testcolor ")) {
        std::string sub(args.substr(10));
        while (!sub.empty() && sub.front() == ' ') sub.erase(0, 1);
        WriteDevLog("[Command] /streamchat testcolor %s submitted", sub.c_str());
        RunColorDiagnosticTest(sub, cmd);
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args.starts_with("color")) {
        std::string sub(args.length() > 5 ? args.substr(5) : "");
        while (!sub.empty() && sub.front() == ' ') sub.erase(0, 1);

        if (sub.empty() || sub == "status") {
            char buf[384];
            std::lock_guard<std::mutex> lk(g_colorMutex);
            std::snprintf(buf, sizeof(buf),
                "[StreamChat Colors] Twitch: [Prefix: %s, Message: %s] | YouTube: [Prefix: %s, Message: %s]",
                g_twitchPrefixColor.c_str(), g_twitchMessageColor.c_str(),
                g_youtubePrefixColor.c_str(), g_youtubeMessageColor.c_str());
            cmd->plugin->WriteConsoleMessage(buf);
            cmd->plugin->WriteConsoleMessage("Type 'streamchat color list' to view all available colors or 'streamchat color test' to preview in chat.");
            return D2RL::ConsoleCommandResult::Handled;
        }

        if (sub == "list") {
            cmd->plugin->WriteConsoleMessage("[Available Colors] white (0), red (1), green (2), blue (3), gold (4), gray (5), black (6), tan (7), orange (8), yellow (9), dark_green (10), purple (11), light_green (12)");
            cmd->plugin->WriteConsoleMessage("Note: 'black' is supported but hard to see on dark backgrounds.");
            cmd->plugin->WriteConsoleMessage("Usage: streamchat color test");
            cmd->plugin->WriteConsoleMessage("       streamchat color twitch prefix <color> [settotoml]");
            cmd->plugin->WriteConsoleMessage("       streamchat color twitch message <color> [settotoml]");
            cmd->plugin->WriteConsoleMessage("       streamchat color youtube prefix <color> [settotoml]");
            cmd->plugin->WriteConsoleMessage("       streamchat color youtube message <color> [settotoml]");
            return D2RL::ConsoleCommandResult::Handled;
        }

        if (sub == "test") {
            RunColorPalettePreview(cmd);
            return D2RL::ConsoleCommandResult::Handled;
        }

        if (sub.starts_with("twitch ")) {
            std::string rest = sub.substr(7);
            while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
            if (rest.starts_with("prefix ")) {
                std::string col = NormalizeColorName(rest.substr(7));
                {
                    std::lock_guard<std::mutex> lk(g_colorMutex);
                    g_twitchPrefixColor = col;
                }
                if (saveToToml) SaveSettingToToml("twitch", "prefix_color", "\"" + col + "\"");
                WriteDevLog("[Command] /streamchat color twitch prefix set to: %s", col.c_str());
                char buf[160];
                std::snprintf(buf, sizeof(buf), "[StreamChat] Twitch prefix color set to: %s%s", col.c_str(), saveToToml ? " (Saved to TOML)" : "");
                cmd->plugin->WriteConsoleMessage(buf);
                if (col == "black") {
                    cmd->plugin->WriteConsoleMessage("[StreamChat Warning] Black is hard to see against the dark background. You might want to consider a different color.");
                }
                return D2RL::ConsoleCommandResult::Handled;
            } else if (rest.starts_with("message ")) {
                std::string col = NormalizeColorName(rest.substr(8));
                {
                    std::lock_guard<std::mutex> lk(g_colorMutex);
                    g_twitchMessageColor = col;
                }
                if (saveToToml) SaveSettingToToml("twitch", "message_color", "\"" + col + "\"");
                WriteDevLog("[Command] /streamchat color twitch message set to: %s", col.c_str());
                char buf[160];
                std::snprintf(buf, sizeof(buf), "[StreamChat] Twitch message color set to: %s%s", col.c_str(), saveToToml ? " (Saved to TOML)" : "");
                cmd->plugin->WriteConsoleMessage(buf);
                if (col == "black") {
                    cmd->plugin->WriteConsoleMessage("[StreamChat Warning] Black is hard to see against the dark background. You might want to consider a different color.");
                }
                return D2RL::ConsoleCommandResult::Handled;
            }
        }

        if (sub.starts_with("youtube ")) {
            std::string rest = sub.substr(8);
            while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
            if (rest.starts_with("prefix ")) {
                std::string col = NormalizeColorName(rest.substr(7));
                {
                    std::lock_guard<std::mutex> lk(g_colorMutex);
                    g_youtubePrefixColor = col;
                }
                if (saveToToml) SaveSettingToToml("youtube", "prefix_color", "\"" + col + "\"");
                WriteDevLog("[Command] /streamchat color youtube prefix set to: %s", col.c_str());
                char buf[160];
                std::snprintf(buf, sizeof(buf), "[StreamChat] YouTube prefix color set to: %s%s", col.c_str(), saveToToml ? " (Saved to TOML)" : "");
                cmd->plugin->WriteConsoleMessage(buf);
                if (col == "black") {
                    cmd->plugin->WriteConsoleMessage("[StreamChat Warning] Black is hard to see against the dark background. You might want to consider a different color.");
                }
                return D2RL::ConsoleCommandResult::Handled;
            } else if (rest.starts_with("message ")) {
                std::string col = NormalizeColorName(rest.substr(8));
                {
                    std::lock_guard<std::mutex> lk(g_colorMutex);
                    g_youtubeMessageColor = col;
                }
                if (saveToToml) SaveSettingToToml("youtube", "message_color", "\"" + col + "\"");
                WriteDevLog("[Command] /streamchat color youtube message set to: %s", col.c_str());
                char buf[160];
                std::snprintf(buf, sizeof(buf), "[StreamChat] YouTube message color set to: %s%s", col.c_str(), saveToToml ? " (Saved to TOML)" : "");
                cmd->plugin->WriteConsoleMessage(buf);
                if (col == "black") {
                    cmd->plugin->WriteConsoleMessage("[StreamChat Warning] Black is hard to see against the dark background. You might want to consider a different color.");
                }
                return D2RL::ConsoleCommandResult::Handled;
            }
        }

        cmd->plugin->WriteConsoleMessage("Usage: streamchat color [twitch|youtube] [prefix|message] <color> [settotoml]");
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args.starts_with("prefix ")) {
        std::string mode(args.substr(7));
        while (!mode.empty() && mode.front() == ' ') mode.erase(0, 1);

        if (mode.starts_with("youtube ")) {
            std::string subMode(mode.substr(8));
            while (!subMode.empty() && subMode.front() == ' ') subMode.erase(0, 1);
            if (subMode == "yt") {
                g_ytPrefixMode.store(YtPrefixMode::YT);
                if (saveToToml) SaveSettingToToml("general", "youtube_prefix_mode", "\"yt\"");
                WriteDevLog("[Command] /streamchat prefix youtube set to: [YT]");
                cmd->plugin->WriteConsoleMessage(saveToToml ? "[StreamChat] YouTube prefix set to: [YT] (Saved to TOML)" : "[StreamChat] YouTube prefix set to: [YT]");
            } else if (subMode == "youtube") {
                g_ytPrefixMode.store(YtPrefixMode::YouTube);
                if (saveToToml) SaveSettingToToml("general", "youtube_prefix_mode", "\"youtube\"");
                WriteDevLog("[Command] /streamchat prefix youtube set to: [YouTube]");
                cmd->plugin->WriteConsoleMessage(saveToToml ? "[StreamChat] YouTube prefix set to: [YouTube] (Saved to TOML)" : "[StreamChat] YouTube prefix set to: [YouTube]");
            } else if (subMode == "none") {
                g_ytPrefixMode.store(YtPrefixMode::None);
                if (saveToToml) SaveSettingToToml("general", "youtube_prefix_mode", "\"none\"");
                WriteDevLog("[Command] /streamchat prefix youtube set to: None");
                cmd->plugin->WriteConsoleMessage(saveToToml ? "[StreamChat] YouTube prefix set to: None (Saved to TOML)" : "[StreamChat] YouTube prefix set to: None");
            } else {
                cmd->plugin->WriteConsoleMessage("Usage: streamchat prefix youtube <youtube | yt | none> [settotoml]");
            }
            return D2RL::ConsoleCommandResult::Handled;
        }

        if (mode.starts_with("twitch ")) {
            std::string subMode(mode.substr(7));
            while (!subMode.empty() && subMode.front() == ' ') subMode.erase(0, 1);
            if (subMode == "twitch") {
                g_prefixMode.store(PrefixMode::Twitch);
                if (saveToToml) SaveSettingToToml("general", "prefix_mode", "\"twitch\"");
                WriteDevLog("[Command] /streamchat prefix twitch set to: [Twitch]");
                cmd->plugin->WriteConsoleMessage(saveToToml ? "[StreamChat] Twitch prefix set to: [Twitch] (Saved to TOML)" : "[StreamChat] Twitch prefix set to: [Twitch]");
            } else if (subMode == "ttv") {
                g_prefixMode.store(PrefixMode::TTV);
                if (saveToToml) SaveSettingToToml("general", "prefix_mode", "\"ttv\"");
                WriteDevLog("[Command] /streamchat prefix twitch set to: [TTV]");
                cmd->plugin->WriteConsoleMessage(saveToToml ? "[StreamChat] Twitch prefix set to: [TTV] (Saved to TOML)" : "[StreamChat] Twitch prefix set to: [TTV]");
            } else if (subMode == "none") {
                g_prefixMode.store(PrefixMode::None);
                if (saveToToml) SaveSettingToToml("general", "prefix_mode", "\"none\"");
                WriteDevLog("[Command] /streamchat prefix twitch set to: None");
                cmd->plugin->WriteConsoleMessage(saveToToml ? "[StreamChat] Twitch prefix set to: None (Saved to TOML)" : "[StreamChat] Twitch prefix set to: None");
            } else {
                cmd->plugin->WriteConsoleMessage("Usage: streamchat prefix twitch <twitch | ttv | none> [settotoml]");
            }
            return D2RL::ConsoleCommandResult::Handled;
        }

        cmd->plugin->WriteConsoleMessage("Usage: streamchat prefix [twitch <twitch|ttv|none> | youtube <youtube|yt|none>] [settotoml]");
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args.starts_with("filter ")) {
        std::string mode(args.substr(7));
        while (!mode.empty() && mode.front() == ' ') mode.erase(0, 1);
        if (mode == "on") {
            g_botFilter.store(true);
            if (saveToToml) SaveSettingToToml("general", "filter_bot_commands", "true");
            WriteDevLog("[Command] /streamchat filter on executed");
            cmd->plugin->WriteConsoleMessage(saveToToml ? "[StreamChat] Bot Filter: ON (Saved to TOML)" : "[StreamChat] Bot Filter: ON");
        } else if (mode == "off") {
            g_botFilter.store(false);
            if (saveToToml) SaveSettingToToml("general", "filter_bot_commands", "false");
            WriteDevLog("[Command] /streamchat filter off executed");
            cmd->plugin->WriteConsoleMessage(saveToToml ? "[StreamChat] Bot Filter: OFF (Saved to TOML)" : "[StreamChat] Bot Filter: OFF");
        } else {
            cmd->plugin->WriteConsoleMessage("Usage: streamchat filter <on | off> [settotoml]");
        }
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args == "help" || args == "?") {
        const auto& twCreator = GetRandomTwitchCreator();
        const auto& ytCreator = GetRandomYouTubeCreator();
        char twBuf[256];
        char ytBuf[256];
        std::snprintf(twBuf, sizeof(twBuf), "  twitch <channel> [settotoml] | twitch stop  (e.g. twitch %s)", twCreator.twitch);
        std::snprintf(ytBuf, sizeof(ytBuf), "  youtube <@handle> [settotoml] | youtube stop (e.g. youtube %s)", ytCreator.youtube);

        cmd->plugin->WriteConsoleMessage("=== D2R Stream Chat Commands ===");
        cmd->plugin->WriteConsoleMessage("  streamchat status");
        cmd->plugin->WriteConsoleMessage("  streamchat test");
        cmd->plugin->WriteConsoleMessage("  streamchat layout <single | twoline> [settotoml]");
        cmd->plugin->WriteConsoleMessage("  streamchat color list");
        cmd->plugin->WriteConsoleMessage("  streamchat color test");
        cmd->plugin->WriteConsoleMessage("  streamchat color twitch prefix <color> [settotoml]");
        cmd->plugin->WriteConsoleMessage("  streamchat color twitch message <color> [settotoml]");
        cmd->plugin->WriteConsoleMessage("  streamchat color youtube prefix <color> [settotoml]");
        cmd->plugin->WriteConsoleMessage("  streamchat color youtube message <color> [settotoml]");
        cmd->plugin->WriteConsoleMessage("  streamchat prefix twitch <twitch|ttv|none> [settotoml]");
        cmd->plugin->WriteConsoleMessage("  streamchat prefix youtube <youtube|yt|none> [settotoml]");
        cmd->plugin->WriteConsoleMessage("  streamchat filter <on | off> [settotoml]");
        cmd->plugin->WriteConsoleMessage("  streamchat dev <on | off> [settotoml]");
        cmd->plugin->WriteConsoleMessage(twBuf);
        cmd->plugin->WriteConsoleMessage(ytBuf);
        cmd->plugin->WriteConsoleMessage("  /tr <message> (In-game reply to Twitch live chat)");
        cmd->plugin->WriteConsoleMessage("Note: 'settotoml' is an optional argument you can append to any command to permanently save that setting to d2rl-streamchat.toml!");
        return D2RL::ConsoleCommandResult::Handled;
    }

    cmd->plugin->WriteConsoleMessage("Usage: streamchat [status | layout | color | prefix | filter | dev | test | help]");
    cmd->plugin->WriteConsoleMessage("Type 'streamchat help' for detailed usage information.");
    return D2RL::ConsoleCommandResult::Handled;
}

static auto TwitchCommand(
    D2R::Game::Client*,
    const D2RL::ConsoleCommandContext* cmd,
    void*
) noexcept -> D2RL::ConsoleCommandResult {
    if (!cmd || !cmd->plugin) return D2RL::ConsoleCommandResult::Failed;
    std::string args(cmd->args ? cmd->args : "");
    bool saveToToml = CheckAndStripSetToToml(args);

    if (args == "help" || args == "?") {
        const auto& creator = GetRandomTwitchCreator();
        char twBuf[256];
        std::snprintf(twBuf, sizeof(twBuf), "  twitch <channel> [settotoml]     - Connect to a Twitch channel (e.g. twitch %s)", creator.twitch);

        cmd->plugin->WriteConsoleMessage("=== Twitch Commands ===");
        cmd->plugin->WriteConsoleMessage(twBuf);
        cmd->plugin->WriteConsoleMessage("  twitch stop                      - Disconnect from current Twitch channel");
        cmd->plugin->WriteConsoleMessage("  twitch status                    - View current Twitch connection state");
        cmd->plugin->WriteConsoleMessage("  twitch auth <token> [username]   - Configure OAuth token for in-game replies");
        cmd->plugin->WriteConsoleMessage("  /tr <message>                     - Send in-game reply to Twitch live chat");
        cmd->plugin->WriteConsoleMessage("Note: 'settotoml' is an optional argument you can append to any command to permanently save that setting to d2rl-streamchat.toml!");
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args.empty() || args == "status") {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "[Twitch] Channel: #%s | Status: %s",
            g_activeTwitchChannel.empty() ? "None" : g_activeTwitchChannel.c_str(),
            g_twitchConnected.load() ? "Connected" : "Disconnected"
        );
        cmd->plugin->WriteConsoleMessage(buf);
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args.starts_with("auth ")) {
        std::string authArg(args.substr(5));
        char tokBuf[256] = {};
        char userBuf[128] = {};
        if (std::sscanf(authArg.c_str(), "%255s %127s", tokBuf, userBuf) >= 1) {
            {
                std::lock_guard<std::mutex> lock(g_configMutex);
                g_twitchOAuthToken = tokBuf;
                if (userBuf[0] != '\0') g_twitchUsername = userBuf;
            }
            if (saveToToml) {
                SaveSettingToToml("twitch", "oauth", "\"" + std::string(tokBuf) + "\"");
                if (userBuf[0] != '\0') SaveSettingToToml("twitch", "username", "\"" + std::string(userBuf) + "\"");
            }
            WriteDevLog("[Twitch Auth] OAuth token and username '%s' saved.", userBuf[0] != '\0' ? userBuf : "default");
            cmd->plugin->WriteConsoleMessage(saveToToml ? "[Twitch Auth] Token saved (Saved to TOML)." : "[Twitch Auth] Token saved.");
            if (!g_activeTwitchChannel.empty()) {
                StartTwitchClient(g_activeTwitchChannel);
            }
        }
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args == "disconnect" || args == "stop") {
        WriteDevLog("[Command] /twitch disconnect executed");
        StopTwitchClient();
        cmd->plugin->WriteConsoleMessage("[Twitch] Disconnected.");
        return D2RL::ConsoleCommandResult::Handled;
    }

    std::string channelStr = (args.starts_with("connect ") ? std::string(args.substr(8)) : std::string(args));
    std::string channel = CleanTwitchChannel(channelStr);
    if (!channel.empty()) {
        if (saveToToml) SaveSettingToToml("twitch", "channel", "\"" + channel + "\"");
        WriteDevLog("[Command] /twitch connect #%s executed", channel.c_str());
        char buf[128];
        std::snprintf(buf, sizeof(buf), "[Twitch] Connecting to #%s%s...", channel.c_str(), saveToToml ? " (Saved as default in TOML)" : "");
        cmd->plugin->WriteConsoleMessage(buf);
        StartTwitchClient(channel);
    }
    return D2RL::ConsoleCommandResult::Handled;
}

static auto YouTubeCommand(
    D2R::Game::Client*,
    const D2RL::ConsoleCommandContext* cmd,
    void*
) noexcept -> D2RL::ConsoleCommandResult {
    if (!cmd || !cmd->plugin) return D2RL::ConsoleCommandResult::Failed;
    std::string args(cmd->args ? cmd->args : "");
    bool saveToToml = CheckAndStripSetToToml(args);

    if (args == "help" || args == "?") {
        const auto& creator = GetRandomYouTubeCreator();
        char ytBuf[256];
        std::snprintf(ytBuf, sizeof(ytBuf), "  youtube <@handle> [settotoml] - Set YouTube handle and monitor live stream (e.g. youtube %s)", creator.youtube);

        cmd->plugin->WriteConsoleMessage("=== YouTube Commands ===");
        cmd->plugin->WriteConsoleMessage(ytBuf);
        cmd->plugin->WriteConsoleMessage("  youtube stop                  - Disconnect from YouTube live monitoring");
        cmd->plugin->WriteConsoleMessage("  youtube status                - View current YouTube monitoring state");
        cmd->plugin->WriteConsoleMessage("Note: 'settotoml' is an optional argument you can append to any command to permanently save that setting to d2rl-streamchat.toml!");
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args.empty() || args == "status") {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "[YouTube] Target: %s | Status: %s",
            g_activeYtTarget.empty() ? "None" : g_activeYtTarget.c_str(),
            g_ytConnected.load() ? "Connected" : "Monitoring"
        );
        cmd->plugin->WriteConsoleMessage(buf);
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args == "disconnect" || args == "stop") {
        WriteDevLog("[Command] /youtube disconnect executed");
        StopYouTubeClient();
        cmd->plugin->WriteConsoleMessage("[YouTube] Disconnected.");
        return D2RL::ConsoleCommandResult::Handled;
    }

    std::string target = (args.starts_with("connect ") ? std::string(args.substr(8)) : std::string(args));
    if (saveToToml) SaveSettingToToml("youtube", "channel", "\"" + target + "\"");
    WriteDevLog("[Command] /youtube connect '%s' executed", target.c_str());
    StartYouTubeClient(target);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "[YouTube] Target set to %s. Monitoring live stream%s...", target.c_str(), saveToToml ? " (Saved as default in TOML)" : "");
    cmd->plugin->WriteConsoleMessage(buf);
    return D2RL::ConsoleCommandResult::Handled;
}

static void __cdecl OnLocalPlayerReady(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent*, void*) noexcept {
    g_inGameSession.store(true);
    WriteDevLog("[StreamChat] Local player ready (Entered game session).");
    RotateTomlExamples(g_context);
}

static void __cdecl OnGameLeft(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent*, void*) noexcept {
    g_inGameSession.store(false);
    WriteDevLog("[StreamChat] Player left game session.");
    RotateTomlExamples(g_context);
}

} // namespace

// ============================================================================
// Plugin Exports
// ============================================================================
D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &kPluginInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!context) return false;
    g_context = context;

    WriteLog("=================================================");
    WriteLog("[StreamChat] Initializing D2R Stream Chat v1.1.0...");

    WSADATA wsaData;
    (void)WSAStartup(MAKEWORD(2, 2), &wsaData);

    (void)context->QueryService(&g_threads);
    auto lifecycleRes = context->QueryService(&g_lifecycle);
    if (lifecycleRes == D2RL::ServiceQueryResult::Success &&
        D2RL::HasLifecycleServiceField(g_lifecycle, D2RL::LifecycleServiceRequiredSize)) {
        
        const D2RL::Lifecycle::GameplayEventListener readyListener {
            .structSize = D2RL::Lifecycle::GameplayEventListenerSize,
            .flags      = 0,
            .kind       = D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady,
            .reserved   = 0,
            .callback   = OnLocalPlayerReady,
            .userData   = nullptr,
        };
        D2RL::Lifecycle::ListenerHandle readyHandle = D2RL::Lifecycle::InvalidHandle;
        (void)g_lifecycle->registerGameplayEventListener(context, &readyListener, &readyHandle);

        const D2RL::Lifecycle::GameplayEventListener leftListener {
            .structSize = D2RL::Lifecycle::GameplayEventListenerSize,
            .flags      = 0,
            .kind       = D2RL::Lifecycle::GameplayEventKind::GameLeft,
            .reserved   = 0,
            .callback   = OnGameLeft,
            .userData   = nullptr,
        };
        D2RL::Lifecycle::ListenerHandle leftHandle = D2RL::Lifecycle::InvalidHandle;
        (void)g_lifecycle->registerGameplayEventListener(context, &leftListener, &leftHandle);
    }

    (void)context->QueryService(&g_widgets);

    // Register User Commands
    (void)context->RegisterConsoleCommand("streamchat", StreamChatCommand, "Configure StreamChat options, layout, colors, and status (streamchat help).");
    (void)context->RegisterConsoleCommand("twitch", TwitchCommand, "Manage Twitch live chat connection (twitch <channel> | stop | status | auth).");
    (void)context->RegisterConsoleCommand("youtube", YouTubeCommand, "Manage YouTube Live chat connection (youtube <@handle> | stop | status).");

    // Diagnostic & Dev Commands (Protected at runtime by g_enableDevCommands / TOML)
    (void)context->RegisterConsoleCommand("testchat", TestChatCommand, "[DEV] Simulate incoming Twitch chat message (testchat <user> <message>).");
    (void)context->RegisterConsoleCommand("testcolor", TestColorCommand, "[DEV] Run color diagnostic tests (testcolor <all|utf8|tag|bracket|ansi>).");
    (void)context->RegisterConsoleCommand("testclientchat", TestClientChatCommand, "[DEV] Test local-only client chat push (testclientchat <text>).");

    // Trampoline hook for in-game /tr chat replies
    InstallSubmitChatHook();

    AddMessage(Platform::System, "StreamChat", "D2R Stream Chat v1.1.0 loaded!", false);

    // Read, auto-generate, and migrate config if missing new options
    std::string defaultCh;
    std::string defaultYt;

    std::string cfg = ReadPluginConfig(context);
    SyncAndMigrateTomlConfig(context, cfg);

    if (!cfg.empty()) {
        defaultCh = ExtractTomlValue(cfg, "channel");
        std::string oauth = ExtractTomlValue(cfg, "oauth");
        std::string user = ExtractTomlValue(cfg, "username");
        
        defaultYt = ExtractTomlValue(cfg, "channel");
        if (defaultYt.empty() || defaultYt == defaultCh) {
            size_t ytSection = cfg.find("[youtube]");
            if (ytSection != std::string::npos) {
                std::string ytSub = cfg.substr(ytSection);
                defaultYt = ExtractTomlValue(ytSub, "channel");
                if (defaultYt.empty()) defaultYt = ExtractTomlValue(ytSub, "youtube_video");
            }
        }

        std::string pMode = ExtractTomlValue(cfg, "prefix_mode");
        if (pMode.empty()) pMode = ExtractTomlValue(cfg, "twitch_prefix");
        if (pMode == "none" || pMode == "off") {
            g_prefixMode.store(PrefixMode::None);
        } else if (pMode == "twitch") {
            g_prefixMode.store(PrefixMode::Twitch);
        } else if (pMode == "ttv") {
            g_prefixMode.store(PrefixMode::TTV);
        }

        std::string ytPMode = ExtractTomlValue(cfg, "youtube_prefix_mode");
        if (ytPMode.empty()) ytPMode = ExtractTomlValue(cfg, "youtube_prefix");
        if (ytPMode.empty()) ytPMode = ExtractTomlValue(cfg, "yt_prefix");
        if (ytPMode == "none" || ytPMode == "off") {
            g_ytPrefixMode.store(YtPrefixMode::None);
        } else if (ytPMode == "youtube") {
            g_ytPrefixMode.store(YtPrefixMode::YouTube);
        } else if (ytPMode == "yt") {
            g_ytPrefixMode.store(YtPrefixMode::YT);
        }

        std::string layoutCfg = ExtractTomlValue(cfg, "layout");
        if (layoutCfg == "twoline" || layoutCfg == "2" || layoutCfg == "two") {
            g_chatLayoutMode.store(ChatLayoutMode::TwoLine);
        } else if (layoutCfg == "single" || layoutCfg == "1" || layoutCfg == "singleline") {
            g_chatLayoutMode.store(ChatLayoutMode::SingleLine);
        }

        std::string devCfg = ExtractTomlValue(cfg, "enable_dev_commands");
        if (devCfg.empty()) devCfg = ExtractTomlValue(cfg, "dev_commands");
        if (devCfg == "true" || devCfg == "1" || devCfg == "on") {
            g_enableDevCommands.store(true);
        } else {
            g_enableDevCommands.store(false);
        }

        // Color configuration: Prefix/Name color and Message color
        std::string twPfxColor = ExtractTomlValue(cfg, "twitch_prefix_color");
        std::string twMsgColor = ExtractTomlValue(cfg, "twitch_message_color");
        size_t twSection = cfg.find("[twitch]");
        if (twSection != std::string::npos) {
            std::string twSub = cfg.substr(twSection);
            size_t nextSection = twSub.find("\n[", 1);
            if (nextSection != std::string::npos) twSub = twSub.substr(0, nextSection);
            if (twPfxColor.empty()) twPfxColor = ExtractTomlValue(twSub, "prefix_color");
            if (twPfxColor.empty()) twPfxColor = ExtractTomlValue(twSub, "name_color");
            if (twPfxColor.empty()) twPfxColor = ExtractTomlValue(twSub, "color");
            if (twMsgColor.empty()) twMsgColor = ExtractTomlValue(twSub, "message_color");
        }

        std::string ytPfxColor = ExtractTomlValue(cfg, "youtube_prefix_color");
        std::string ytMsgColor = ExtractTomlValue(cfg, "youtube_message_color");
        size_t ytSectionForColors = cfg.find("[youtube]");
        if (ytSectionForColors != std::string::npos) {
            std::string ytSub = cfg.substr(ytSectionForColors);
            size_t nextSection = ytSub.find("\n[", 1);
            if (nextSection != std::string::npos) ytSub = ytSub.substr(0, nextSection);
            if (ytPfxColor.empty()) ytPfxColor = ExtractTomlValue(ytSub, "prefix_color");
            if (ytPfxColor.empty()) ytPfxColor = ExtractTomlValue(ytSub, "name_color");
            if (ytPfxColor.empty()) ytPfxColor = ExtractTomlValue(ytSub, "color");
            if (ytMsgColor.empty()) ytMsgColor = ExtractTomlValue(ytSub, "message_color");
        }

        {
            std::lock_guard<std::mutex> lk(g_colorMutex);
            if (!twPfxColor.empty()) g_twitchPrefixColor = NormalizeColorName(twPfxColor);
            if (!twMsgColor.empty()) g_twitchMessageColor = NormalizeColorName(twMsgColor);
            if (!ytPfxColor.empty()) g_youtubePrefixColor = NormalizeColorName(ytPfxColor);
            if (!ytMsgColor.empty()) g_youtubeMessageColor = NormalizeColorName(ytMsgColor);
        }

        std::string botCfg = ExtractTomlValue(cfg, "filter_bot_commands");
        if (botCfg == "false" || botCfg == "0" || botCfg == "off") {
            g_botFilter.store(false);
        } else if (botCfg == "true" || botCfg == "1" || botCfg == "on") {
            g_botFilter.store(true);
        }

        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            if (!oauth.empty()) g_twitchOAuthToken = oauth;
            if (!user.empty()) g_twitchUsername = user;
        }

        const char* pfxTwitchStr = (g_prefixMode.load() == PrefixMode::TTV) ? "[TTV]" :
                                   (g_prefixMode.load() == PrefixMode::Twitch) ? "[Twitch]" : "None";
        const char* pfxYtStr = (g_ytPrefixMode.load() == YtPrefixMode::YT) ? "[YT]" :
                               (g_ytPrefixMode.load() == YtPrefixMode::YouTube) ? "[YouTube]" : "None";

        WriteLog("[StreamChat] Config loaded: Layout=%s | DevCommands=%s | BotFilter=%s",
            (g_chatLayoutMode.load() == ChatLayoutMode::TwoLine) ? "TwoLine" : "SingleLine",
            g_enableDevCommands.load() ? "ENABLED" : "DISABLED",
            g_botFilter.load() ? "ON" : "OFF");
        WriteLog("[StreamChat] Twitch Config: Channel='#%s', Prefix=%s, PrefixColor='%s', MsgColor='%s'",
            defaultCh.c_str(), pfxTwitchStr, g_twitchPrefixColor.c_str(), g_twitchMessageColor.c_str());
        WriteLog("[StreamChat] YouTube Config: Target='%s', Prefix=%s, PrefixColor='%s', MsgColor='%s'",
            defaultYt.c_str(), pfxYtStr, g_youtubePrefixColor.c_str(), g_youtubeMessageColor.c_str());
    }

    if (!defaultCh.empty()) {
        WriteLog("[StreamChat] Auto-connecting to Twitch channel #%s from config...", defaultCh.c_str());
        StartTwitchClient(defaultCh);
    }
    if (!defaultYt.empty()) {
        WriteLog("[StreamChat] Auto-connecting to YouTube target '%s' from config...", defaultYt.c_str());
        StartYouTubeClient(defaultYt);
    }

    WriteLog("[StreamChat] D2R Stream Chat v1.1.0 loaded successfully!");
    WriteLog("=================================================");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    WriteLog("=================================================");
    WriteLog("[StreamChat] Unloading D2R Stream Chat plugin...");
    UninstallSubmitChatHook();
    StopTwitchClient();
    StopYouTubeClient();
    WSACleanup();
    WriteLog("[StreamChat] D2R Stream Chat plugin unloaded successfully.");
    WriteLog("=================================================");
}
