# 💬 D2R Stream Chat

A native in-game stream chat integration for **Diablo II: Resurrected**, powered by the [D2RLoader](https://d2rloader.net/) [Plugin SDK](https://github.com/D2RLoader/PluginSDK).

**D2R Stream Chat** streams your live **Twitch** and **YouTube** chat messages directly into D2R's native in-game chat window—no external overlays, borderless window hacks, or second monitors required. It also allows you to **reply directly to your stream chat from inside the game** using native chat commands!

---

## ✨ Features

- **🎮 100% Native In-Game Chat Integration**: Messages render directly in the standard D2R chat box alongside game messages.
- **🟣 Twitch Live Chat**:
  - **Two-Way Chat (Replies)**: Optional [OAuth](https://twitchtokengenerator.com/) support to reply to viewers directly from your in-game chat prompt (`/tr <message>`).
- **🔴 YouTube Live Chat (Zero-Auth / Quota-Free)**:
  - Hooks directly into YouTube's internal InnerTube API—**no Google Cloud accounts, developer tokens, or API quotas required**.
  - **Auto-Live Detection**: Just set your YouTube handle (e.g. `@YourChannel`), and the plugin will automatically detect when you go live!
- **⚡ Safe Trampoline Hooking**:
  - Automatically intercepts in-game `/tr` and `/ttv` commands so they are sent to your stream instead of broadcasting to your Diablo party or Battle.net.

---

## 📥 Installation

1. Make sure you have **[D2RLoader](https://d2rloader.net/)** installed.
2. Download the latest `d2rl-streamchat.dll` from the [Releases](../../releases) tab.
3. Place `d2rl-streamchat.dll` into your D2RLoader plugins folder:
Start Diablo II: Resurrected through D2RLoader.

## ⚙️ Configuration
On first launch, D2RLoader generates a configuration file at:
d2rloader/config/d2rl-streamchat.toml  
Open d2rl-streamchat.toml in any text editor.


```Toml
[general]
# Filter out bot commands starting with '!' (true = hide, false = show)
filter_bot_commands = true

# Prefix displayed before viewer names: "ttv", "twitch", or "none"
# - "ttv"    -> [TTV] Viewer: Hello!
# - "twitch" -> [Twitch] Viewer: Hello!
# - "none"   -> Viewer: Hello!
prefix_mode = "ttv"

[twitch]
enabled = true
# Your Twitch channel name (lowercase, no '#' needed)
channel = "your_twitch_channel"

# Your Twitch username (required ONLY if sending replies via /tr)
username = "your_twitch_username"

# Optional: OAuth token to send in-game chat replies back to Twitch.
# Generate one at: https://twitchtokengenerator.com (Needs 'chat:read' & 'chat:write')
# Format: "oauth:your_token_here"
# Leave blank for anonymous read-only mode!
oauth = ""

[youtube]
enabled = true
# Enter your YouTube handle (e.g. "@YourChannel" or "YourChannel") or a video URL/ID.
# If a handle is used, StreamChat automatically detects when you go live!
channel = "@YourChannel"

```


##⌨️ In-Game Commands
-You can control the plugin or reply to viewers directly from the in-game chat prompt or 
-developer console:
-Chatting with Viewers (Normal In-Game Chat Box)
-Press Enter in-game and type:
-/tr <message> — Send a reply to your Twitch chat (e.g. /tr Thanks for the follow!).
-(Note: Requires configuring your username and oauth token in the TOML file or via /twitch auth).
-Console Commands ( ctrl + ~ )
-streamchat or /sc — Display connection status and message counters.
-streamchat prefix <ttv|twitch|none> — Change the prefix format on the fly.
-streamchat filter <on|off> — Toggle the !command bot filter.
-twitch <channel> — Connect to a Twitch channel live.
-twitch disconnect — Disconnect from Twitch.
-twitch auth <oauth:token> [username] — Set your Twitch OAuth token without restarting.
-youtube <@handle|video_url> — Connect to a YouTube stream or channel.
-youtube disconnect — Disconnect from YouTube.


##🛠️ Building from Source  
-This project uses CMake and C++20. It automatically downloads and configures the D2RLoader   
-PluginSDK during compilation using CMake FetchContent—no manual SDK setup required!  
-Prerequisites  
-Windows 10 / 11 (64-bit)  
-Visual Studio 2022 (with Desktop development with C++)  
-CMake (v3.29 or newer)  
-Build Instructions  

##Clone the repository, Configure the build directory, and compile.  

```powershell
git clone https://github.com/yourusername/D2RStreamChat.git
cd D2RStreamChat
cmake -B build
cmake --build build --config Release
```

The compiled d2rl-streamchat.dll will be located in:  
build/Release/d2rl-streamchat.dll.  
📄 License  
This project is open-source and distributed under the MIT License. See LICENSE for details.  

