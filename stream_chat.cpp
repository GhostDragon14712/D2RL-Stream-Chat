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

// ============================================================================
// DEV SWITCH: Set to 1 to enable diagnostic scanners, memory dumpers, and tests.
// Set to 0 for normal, crash-proof gameplay.
// ============================================================================
#define ENABLE_DEV_COMMANDS 0

namespace {

constexpr D2RL::PluginInfo kPluginInfo = {
    .infoSize = sizeof(D2RL::PluginInfo),
    .abiVersion = D2RL_PLUGIN_ABI_VERSION,
    .id = "d2rl-streamchat",
    .name = "D2R Stream Chat",
    .version = "1.9.8",
    .author = "D2R Community",
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

struct ChatMessage {
    Platform platform;
    std::string author;
    std::string text;
    std::string timeStr;
    std::chrono::steady_clock::time_point timestamp;
};

static const D2RL::PluginContext* g_context = nullptr;
static const D2RL::ThreadService* g_threads = nullptr;
static const D2RL::LifecycleService* g_lifecycle = nullptr;
static const D2RL::WidgetService* g_widgets = nullptr;

static std::atomic<bool> g_inGameSession{false};
static std::atomic<PrefixMode> g_prefixMode{PrefixMode::TTV};
static std::atomic<bool> g_botFilter{true};
static std::mutex g_configMutex;

static void WriteLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    FILE* f = fopen("d2rl-streamchat.log", "a+");
    if (f) {
        std::fprintf(f, "%s\n", buf);
        std::fclose(f);
    }
}

#pragma pack(push, 8)
struct D2RStringRef {
    char* data;
    uint64_t length;
};
#pragma pack(pop)

using D2RSubmitChatHandlerFn = void(__fastcall*)(void* widget, D2RStringRef* stringRef, uint64_t channel) noexcept;

// Verified SubmitChatHandler entrypoint
constexpr uintptr_t kSubmitChatRva = 0x2E34C0;

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
        fnSubmit(nullptr, &ref, channel);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        WriteLog("[StreamChat] SubmitChatDirect exception: 0x%08X", GetExceptionCode());
        return false;
    }
}

static bool SendToNativeChat(const char* formattedMessage) {
    if (!formattedMessage || !formattedMessage[0]) return false;
    return SubmitChatDirect(formattedMessage, 0);
}

struct QueuedMessage {
    std::string text;
};

static void __cdecl DeliverMessageCallback(const D2RL::PluginContext*, void* userData) noexcept {
    auto* qm = static_cast<QueuedMessage*>(userData);
    if (qm) {
        SendToNativeChat(qm->text.c_str());
        delete qm;
    }
}

static void QueueMessageToMainThread(std::string text) {
    if (g_context && g_threads && D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize)) {
        auto* qm = new QueuedMessage{ std::move(text) };
        auto res = g_threads->runOnUiThread(g_context, DeliverMessageCallback, qm);
        if (res == D2RL::Threads::Result::Success) {
            return;
        }
        delete qm;
    }
    SendToNativeChat(text.c_str());
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

    std::string prefix;
    PrefixMode pMode = g_prefixMode.load();
    if (platform == Platform::Twitch) {
        if (pMode == PrefixMode::TTV) prefix = "[TTV] ";
        else if (pMode == PrefixMode::Twitch) prefix = "[Twitch] ";
    } else if (platform == Platform::YouTube) {
        prefix = "[YT] ";
    } else {
        prefix = "[Stream] ";
    }

    if (g_context) {
        char consoleLine[512];
        std::snprintf(consoleLine, sizeof(consoleLine), "%s%s: %s", prefix.c_str(), msg.author.c_str(), msg.text.c_str());
        g_context->WriteConsoleMessage(consoleLine);
    }

    if (sendToNative && g_inGameSession.load()) {
        char nativeLine[512];
        std::snprintf(nativeLine, sizeof(nativeLine), "%s%s: %s", prefix.c_str(), msg.author.c_str(), msg.text.c_str());
        QueueMessageToMainThread(nativeLine);
    }

    g_messages.push_back(std::move(msg));
    while (g_messages.size() > 80) {
        g_messages.pop_front();
    }
}

// ============================================================================
// Twitch Client
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

    char msgBuf[1024];
    std::snprintf(msgBuf, sizeof(msgBuf), "PRIVMSG #%s :%s\r\n", chan.c_str(), text.c_str());
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
    g_twitchConnected.store(false);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;

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

        DWORD timeoutMs = 2000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

        if (connect(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0) {
            freeaddrinfo(res);
            break;
        }

        closesocket(sock);
        sock = INVALID_SOCKET;
        freeaddrinfo(res);
    }

    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    g_twitchSocket.store(sock);
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

    AddMessage(Platform::System, "StreamChat", "Connected to Twitch #" + channel);

    std::string recvBuffer;
    recvBuffer.reserve(4096);
    char tempBuf[1024];

    while (g_twitchRunning.load()) {
        int bytes = recv(sock, tempBuf, sizeof(tempBuf) - 1, 0);
        if (bytes > 0) {
            tempBuf[bytes] = '\0';
            recvBuffer.append(tempBuf, bytes);

            size_t pos = 0;
            while ((pos = recvBuffer.find("\r\n")) != std::string::npos) {
                std::string line = recvBuffer.substr(0, pos);
                recvBuffer.erase(0, pos + 2);

                if (line.starts_with("PING")) {
                    std::string pong = "PONG :tmi.twitch.tv\r\n";
                    send(sock, pong.c_str(), static_cast<int>(pong.size()), 0);
                } else {
                    std::string author, text;
                    if (ParseTwitchPrivmsg(line, author, text)) {
                        AddMessage(Platform::Twitch, std::move(author), std::move(text));
                    }
                }
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
    g_twitchSocket.store(INVALID_SOCKET);
    closesocket(sock);
    WSACleanup();

    AddMessage(Platform::System, "StreamChat", "Disconnected from Twitch #" + channel);
}

static void StopTwitchClient() {
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
}

static void StartTwitchClient(std::string channel) {
    channel = CleanTwitchChannel(channel);
    if (channel.empty()) return;

    StopTwitchClient();
    g_activeTwitchChannel = channel;
    g_twitchRunning.store(true);
    g_twitchThread = std::thread(TwitchWorker, channel);
}

// ============================================================================
// YouTube InnerTube Client
// ============================================================================
static std::atomic<bool> g_ytRunning{false};
static std::atomic<bool> g_ytConnected{false};
static std::thread g_ytThread;
static std::string g_activeYtTarget; 

static const wchar_t* kYtInnertubeKey = L"AIzaSyAO_FJ2SlqU8Q4usACBoqdHbptSemRfVo8";

static std::string ExtractJsonValue(const std::string& json, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";

    size_t colon = json.find(':', pos + needle.length());
    if (colon == std::string::npos) return "";

    size_t firstQuote = json.find('\"', colon + 1);
    if (firstQuote == std::string::npos || (firstQuote - colon) > 10) return "";

    size_t secondQuote = json.find('\"', firstQuote + 1);
    if (secondQuote == std::string::npos) return "";

    return json.substr(firstQuote + 1, secondQuote - (firstQuote + 1));
}

static std::string YtHttpGet(const std::wstring& path) {
    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    HINTERNET hConnect = WinHttpConnect(hSession, L"www.youtube.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                        NULL, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

    std::string response;
    if (hReq) {
        DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        WinHttpSetOption(hReq, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));

        if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hReq, NULL)) {
            
            char buf[8192];
            DWORD bytesRead = 0;
            while (WinHttpReadData(hReq, buf, sizeof(buf), &bytesRead) && bytesRead > 0) {
                response.append(buf, bytesRead);
            }
        }
        WinHttpCloseHandle(hReq);
    }

    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

static std::string YtHttpPostInnerTube(const std::wstring& endpointPath, const std::string& jsonBody) {
    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    HINTERNET hConnect = WinHttpConnect(hSession, L"www.youtube.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", endpointPath.c_str(),
                                        NULL, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

    std::string response;
    if (hReq) {
        DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        WinHttpSetOption(hReq, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));

        LPCWSTR headers = L"Content-Type: application/json\r\n"
                          L"X-YouTube-Client-Name: 1\r\n"
                          L"X-YouTube-Client-Version: 2.20230622.01.00\r\n"
                          L"Origin: https://www.youtube.com\r\n";

        if (WinHttpSendRequest(hReq, headers, (DWORD)-1, (LPVOID)jsonBody.c_str(), (DWORD)jsonBody.length(), (DWORD)jsonBody.length(), 0) &&
            WinHttpReceiveResponse(hReq, NULL)) {
            
            char buf[8192];
            DWORD bytesRead = 0;
            while (WinHttpReadData(hReq, buf, sizeof(buf), &bytesRead) && bytesRead > 0) {
                response.append(buf, bytesRead);
            }
        }
        WinHttpCloseHandle(hReq);
    }

    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

static std::string ResolveLiveVideoId(const std::string& channelHandle) {
    std::string handle = channelHandle;
    while (!handle.empty() && (handle.front() == ' ' || handle.front() == '/')) handle.erase(0, 1);
    if (!handle.starts_with("@") && !handle.starts_with("channel/")) {
        handle = "@" + handle;
    }

    std::wstring path = L"/" + std::wstring(handle.begin(), handle.end()) + L"/live";
    std::string html = YtHttpGet(path);
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

    bool isDirectVideoId = (target.length() == 11 && target.find('/') == std::string::npos && target.find('@') == std::string::npos);
    std::string currentVideoId = isDirectVideoId ? target : "";
    bool announcedWaiting = false;

    while (g_ytRunning.load()) {
        if (!isDirectVideoId) {
            std::string resolved = ResolveLiveVideoId(target);
            if (resolved.empty()) {
                if (!announcedWaiting) {
                    announcedWaiting = true;
                    AddMessage(Platform::System, "StreamChat", "YouTube channel is offline. Monitoring for stream...");
                }
                for (int i = 0; i < 300 && g_ytRunning.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
            currentVideoId = resolved;
            announcedWaiting = false;
        }

        std::wstring nextPath = L"/youtubei/v1/next?key=" + std::wstring(kYtInnertubeKey);
        std::string initPayload = 
            "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"2.20230622.01.00\",\"hl\":\"en\",\"gl\":\"US\"}},"
            "\"videoId\":\"" + currentVideoId + "\"}";

        std::string nextResp = YtHttpPostInnerTube(nextPath, initPayload);
        std::string continuation;
        size_t chatPos = nextResp.find("liveChatRenderer");
        if (chatPos != std::string::npos) {
            continuation = ExtractJsonValue(nextResp.substr(chatPos, 2048), "continuation");
        }
        if (continuation.empty()) {
            continuation = ExtractJsonValue(nextResp, "continuation");
        }

        if (continuation.empty()) {
            if (isDirectVideoId) {
                AddMessage(Platform::System, "StreamChat", "Stream is offline or chat disabled for: " + currentVideoId);
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        g_ytConnected.store(true);
        AddMessage(Platform::System, "StreamChat", "Connected to YouTube Live Chat! (Video: " + currentVideoId + ")");

        std::wstring pollPath = L"/youtubei/v1/live_chat/get_live_chat?key=" + std::wstring(kYtInnertubeKey);
        bool firstRun = true;

        while (g_ytRunning.load()) {
            std::string pollPayload = 
                "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"2.20230622.01.00\"}},"
                "\"continuation\":\"" + continuation + "\"}";

            std::string resp = YtHttpPostInnerTube(pollPath, pollPayload);

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
                while ((pos = resp.find("liveChatTextMessageRenderer", pos)) != std::string::npos) {
                    size_t blockEnd = resp.find("liveChatTextMessageRenderer", pos + 30);
                    if (blockEnd == std::string::npos) blockEnd = resp.length();

                    std::string block = resp.substr(pos, blockEnd - pos);
                    std::string author = ExtractJsonValue(block, "simpleText");
                    std::string text = ExtractJsonValue(block, "text");

                    if (!author.empty() && !text.empty()) {
                        AddMessage(Platform::YouTube, std::move(author), std::move(text));
                    }

                    pos += 30;
                }
            }

            for (DWORD elapsed = 0; elapsed < sleepMs && g_ytRunning.load(); elapsed += 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        g_ytConnected.store(false);
        AddMessage(Platform::System, "StreamChat", "YouTube stream disconnected.");

        if (isDirectVideoId) break;
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

static void StopYouTubeClient() {
    g_ytRunning.store(false);
    if (g_ytThread.joinable()) {
        g_ytThread.join();
    }
    g_activeYtTarget.clear();
    g_ytConnected.store(false);
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
    g_activeYtTarget = target;
    g_ytRunning.store(true);
    g_ytThread = std::thread(YouTubeWorker, target);
}

// ============================================================================
// Trampoline Hook for In-Game /tr and /ttv Chat Replies
// ============================================================================
static uint8_t g_savedBytes[16] = {};
static void* g_trampolinePtr = nullptr;

static void __fastcall DetourSubmitChatHandler(void* widget, D2RStringRef* stringRef, uint64_t channel) noexcept {
    if (stringRef && stringRef->data && stringRef->data[0] == '/') {
        std::string msg(stringRef->data);
        if (msg.starts_with("/tr ") || msg.starts_with("/ttv ")) {
            std::string reply = msg.substr(msg.starts_with("/tr ") ? 4 : 5);
            bool sent = SendTwitchMessageToChannel(reply);
            if (g_context) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "[Twitch Reply] %s: '%s'", sent ? "Sent" : "Failed", reply.c_str());
                g_context->WriteConsoleMessage(buf);
            }
            stringRef->data[0] = '\0';
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
    if (!hD2R) return;

    uint8_t* base = reinterpret_cast<uint8_t*>(hD2R);
    uint8_t* target = base + kSubmitChatRva;
    size_t patchSize = 14;

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 32, PAGE_EXECUTE_READWRITE, &oldProtect)) return;

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
}

// ============================================================================
// DEVELOPER TOOLS & DIAGNOSTICS (Active only when ENABLE_DEV_COMMANDS == 1)
// ============================================================================
#if ENABLE_DEV_COMMANDS

static void DumpBytes(const char* label, const uint8_t* ptr, size_t count = 24) {
    if (!ptr) return;
    char hex[128] = {};
    int pos = 0;
    for (size_t i = 0; i < count; ++i) {
        pos += std::snprintf(hex + pos, sizeof(hex) - pos, "%02X ", ptr[i]);
    }
    if (g_context) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "[DevDump] %s: %s", label, hex);
        g_context->WriteConsoleMessage(buf);
    }
}

static auto DumpChatCommand(
    D2R::Game::Client*,
    const D2RL::ConsoleCommandContext* cmd,
    void*
) noexcept -> D2RL::ConsoleCommandResult {
    if (!cmd || !cmd->plugin) return D2RL::ConsoleCommandResult::Failed;
    HMODULE hD2R = GetModuleHandleA(NULL);
    if (!hD2R) return D2RL::ConsoleCommandResult::Failed;
    uint8_t* base = reinterpret_cast<uint8_t*>(hD2R);

    DumpBytes("0x2E34C0 (SubmitChatHandler)", base + kSubmitChatRva, 24);
    return D2RL::ConsoleCommandResult::Handled;
}

static auto TestChatCommand(
    D2R::Game::Client*,
    const D2RL::ConsoleCommandContext* cmd,
    void*
) noexcept -> D2RL::ConsoleCommandResult {
    if (!cmd || !cmd->plugin) return D2RL::ConsoleCommandResult::Failed;
    char user[64] {};
    char text[256] {};
    if (std::sscanf((cmd->args ? cmd->args : ""), "%63s %255[^\n]", user, text) >= 2) {
        AddMessage(Platform::Twitch, user, text, true);
        return D2RL::ConsoleCommandResult::Handled;
    }
    cmd->plugin->WriteConsoleMessage("Usage: /testchat <user> <message>");
    return D2RL::ConsoleCommandResult::InvalidArguments;
}

#endif // ENABLE_DEV_COMMANDS

// ============================================================================
// User Console Commands
// ============================================================================
static auto StreamChatCommand(
    D2R::Game::Client*,
    const D2RL::ConsoleCommandContext* cmd,
    void*
) noexcept -> D2RL::ConsoleCommandResult {
    if (!cmd || !cmd->plugin) return D2RL::ConsoleCommandResult::Failed;
    const std::string_view args(cmd->args ? cmd->args : "");

    if (args.empty() || args == "info" || args == "status") {
        char buf[384];
        std::lock_guard<std::mutex> lock(g_configMutex);
        std::snprintf(buf, sizeof(buf),
            "[StreamChat] InGame: %s | Twitch: %s (%s) | YouTube: %s (%s) | Prefix: %s | Filter: %s",
            g_inGameSession.load() ? "Yes" : "No",
            g_activeTwitchChannel.empty() ? "None" : g_activeTwitchChannel.c_str(),
            g_twitchConnected.load() ? "Connected" : "Disconnected",
            g_activeYtTarget.empty() ? "None" : g_activeYtTarget.c_str(),
            g_ytConnected.load() ? "Connected" : "Monitoring",
            (g_prefixMode.load() == PrefixMode::TTV) ? "[TTV]" :
            (g_prefixMode.load() == PrefixMode::Twitch) ? "[Twitch]" : "None",
            g_botFilter.load() ? "ON" : "OFF"
        );
        cmd->plugin->WriteConsoleMessage(buf);
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args.starts_with("prefix ")) {
        std::string mode(args.substr(7));
        if (mode == "ttv" || mode == "1") {
            g_prefixMode.store(PrefixMode::TTV);
            cmd->plugin->WriteConsoleMessage("[StreamChat] Prefix set to: [TTV]");
        } else if (mode == "twitch" || mode == "2") {
            g_prefixMode.store(PrefixMode::Twitch);
            cmd->plugin->WriteConsoleMessage("[StreamChat] Prefix set to: [Twitch]");
        } else if (mode == "none" || mode == "off" || mode == "0") {
            g_prefixMode.store(PrefixMode::None);
            cmd->plugin->WriteConsoleMessage("[StreamChat] Prefix set to: None");
        }
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args.starts_with("filter ")) {
        std::string mode(args.substr(7));
        g_botFilter.store(mode == "on" || mode == "1" || mode == "true");
        cmd->plugin->WriteConsoleMessage(g_botFilter.load() ? "[StreamChat] Bot Filter: ON" : "[StreamChat] Bot Filter: OFF");
        return D2RL::ConsoleCommandResult::Handled;
    }

    cmd->plugin->WriteConsoleMessage("Usage: /streamchat [status | prefix <ttv|twitch|none> | filter <on|off>]");
    return D2RL::ConsoleCommandResult::Handled;
}

static auto TwitchCommand(
    D2R::Game::Client*,
    const D2RL::ConsoleCommandContext* cmd,
    void*
) noexcept -> D2RL::ConsoleCommandResult {
    if (!cmd || !cmd->plugin) return D2RL::ConsoleCommandResult::Failed;
    const std::string_view args(cmd->args ? cmd->args : "");

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
            cmd->plugin->WriteConsoleMessage("[Twitch Auth] Token saved.");
            if (!g_activeTwitchChannel.empty()) {
                StartTwitchClient(g_activeTwitchChannel);
            }
        }
        return D2RL::ConsoleCommandResult::Handled;
    }

    if (args == "disconnect" || args == "stop") {
        StopTwitchClient();
        cmd->plugin->WriteConsoleMessage("[Twitch] Disconnected.");
        return D2RL::ConsoleCommandResult::Handled;
    }

    std::string channelStr = (args.starts_with("connect ") ? std::string(args.substr(8)) : std::string(args));
    std::string channel = CleanTwitchChannel(channelStr);
    if (!channel.empty()) {
        cmd->plugin->WriteConsoleMessage("[Twitch] Connecting...");
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
    const std::string_view args(cmd->args ? cmd->args : "");

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
        StopYouTubeClient();
        cmd->plugin->WriteConsoleMessage("[YouTube] Disconnected.");
        return D2RL::ConsoleCommandResult::Handled;
    }

    std::string target = (args.starts_with("connect ") ? std::string(args.substr(8)) : std::string(args));
    StartYouTubeClient(target);
    cmd->plugin->WriteConsoleMessage("[YouTube] Target set. Connecting / Monitoring in background...");
    return D2RL::ConsoleCommandResult::Handled;
}

static void __cdecl OnLocalPlayerReady(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent*, void*) noexcept {
    g_inGameSession.store(true);
}

static void __cdecl OnGameLeft(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent*, void*) noexcept {
    g_inGameSession.store(false);
}

static std::string ExtractTomlValue(const std::string& text, const std::string& key) {
    std::string keyDQ = key + " = \"";
    size_t pos = text.find(keyDQ);
    if (pos != std::string::npos) {
        size_t start = pos + keyDQ.length();
        size_t end = text.find("\"", start);
        if (end != std::string::npos && end > start) {
            return text.substr(start, end - start);
        }
    }
    return "";
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

    // Register Safe User Commands
    (void)context->RegisterConsoleCommand("streamchat", StreamChatCommand, "StreamChat configuration and status.");
    (void)context->RegisterConsoleCommand("sc", StreamChatCommand, "Shortcut for /streamchat.");
    (void)context->RegisterConsoleCommand("twitch", TwitchCommand, "Connect to Twitch stream chat.");
    (void)context->RegisterConsoleCommand("youtube", YouTubeCommand, "Connect to YouTube Live chat or set channel handle.");
    (void)context->RegisterConsoleCommand("yt", YouTubeCommand, "Shortcut for /youtube.");

#if ENABLE_DEV_COMMANDS
    (void)context->RegisterConsoleCommand("dumpchat", DumpChatCommand, "[DEV] Dump 0x2E34C0 function bytes.");
    (void)context->RegisterConsoleCommand("testchat", TestChatCommand, "[DEV] Simulate incoming chat message.");
#endif

    // Install trampoline hook for in-game /tr & /ttv chat replies
    InstallSubmitChatHook();

    AddMessage(Platform::System, "StreamChat", "D2R Stream Chat v1.9.8 loaded!", false);

    // Read config
    std::vector<char> configText(8192, 0);
    uint32_t reqSize = 0;
    std::string defaultCh;
    std::string defaultYt;

    if (context->ReadConfig(configText.data(), static_cast<uint32_t>(configText.size()), &reqSize)) {
        std::string cfg(configText.data());
        
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
        if (pMode == "none" || pMode == "off") {
            g_prefixMode.store(PrefixMode::None);
        } else if (pMode == "twitch") {
            g_prefixMode.store(PrefixMode::Twitch);
        } else if (pMode == "ttv") {
            g_prefixMode.store(PrefixMode::TTV);
        }

        if (cfg.find("filter_bot_commands = false") != std::string::npos) {
            g_botFilter.store(false);
        } else if (cfg.find("filter_bot_commands = true") != std::string::npos) {
            g_botFilter.store(true);
        }

        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            if (!oauth.empty()) g_twitchOAuthToken = oauth;
            if (!user.empty()) g_twitchUsername = user;
        }
    }

    if (!defaultCh.empty()) {
        StartTwitchClient(defaultCh);
    }
    if (!defaultYt.empty()) {
        StartYouTubeClient(defaultYt);
    }

    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    UninstallSubmitChatHook();
    StopTwitchClient();
    StopYouTubeClient();
}