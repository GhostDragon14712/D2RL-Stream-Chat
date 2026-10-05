#  D2R Stream Chat

A native in-game stream chat integration for **Diablo II: Resurrected**, powered by the [D2RLoader](https://d2rloader.net/) [Plugin SDK](https://github.com/D2RLoader/PluginSDK).

**D2R Stream Chat** streams your live **Twitch** and **YouTube** chat messages directly into D2R's native in-game chat window—no external overlays, borderless window hacks, or second monitors required. It also allows you to **reply directly to your stream chat from inside the game** using native chat commands!

---

##  Features

- **100% Native In-Game Chat Integration**: Messages render directly in the standard D2R chat box alongside game messages.
- **Twitch Live Chat**:
  - **Two-Way Chat (Replies)**: Optional [OAuth](https://twitchtokengenerator.com/) support to reply to viewers directly from your in-game chat prompt (`/tr <message>`).  
  - **YouTube Live Detection**: Just set your YouTube handle (e.g. `@YourChannel`), and the plugin will automatically detect when you go live!
- ** Safe Trampoline Hooking**:
  - Automatically intercepts in-game `/tr` command so they are sent to your stream instead of broadcasting to your Diablo party or Battle.net.

---

##  Installation

1. Make sure you have **[D2RLoader](https://d2rloader.net/)** installed.
2. Download the latest `d2rl-streamchat.dll` from the [Releases](../../releases) tab.
3. Place `d2rl-streamchat.dll` into your D2RLoader plugins folder:
Start Diablo II: Resurrected through D2RLoader.

---

##  Configuration

On first launch, D2RLoader generates a configuration file at:
d2rloader/config/d2rl-streamchat.toml  
Open d2rl-streamchat.toml in any text editor.

```Toml
# ==============================================================================
# D2R Stream Chat Configuration
# Injects Twitch and YouTube live chat directly into D2R native in-game chat.
# ==============================================================================

[general]
# Chat layout style:
# "single"  = Prefix, name, and message on 1 line (uses prefix_color).
# "twoline" = Prefix + name on line 1 (prefix_color), message indented on line 2 (message_color).
#     This allows distinct colors for name and message with the tradeoff of taking more chat space.
layout = "twoline"

# Filter out automated bot commands starting with '!' (true = hide, false = show)
filter_bot_commands = true

# Platform tag style: "ttv", "twitch", or "none"
prefix_mode = "twitch"

# YouTube platform tag style: "yt", "youtube", or "none"
youtube_prefix_mode = "youtube"

# Enable developer diagnostic and testing commands (/testcolor, /testchat, etc.)
# Anyone can set this to true to help with troubleshooting and testing without recompiling!
enable_dev_commands = false

[twitch]
enabled = true
# Your Twitch channel name (e.g. "ghostdragon14712")
channel = ""
username = ""
# Optional OAuth token to send replies back to Twitch using /tr.
# If left blank, you will still receive live chat in read-only mode anonymously.
# ("https://twitchtokengenerator.com/")
oauth = ""
# Color for the [Twitch] tag and username
# Working options: white, red, green, blue, gold, gray, tan, orange, yellow, dark_green, purple, light_green
# Note: "black" is supported but hard to see against the dark background.
prefix_color = "purple"
# Twitch message text color ( This is ignored if you are in layout single mode. )
message_color = "white"

[youtube]
enabled = true
# Your YouTube handle or channel name (e.g. "@-Ghost.Dragon")
# Automatically detects when you go live without needing a video ID!
channel = ""
# Color for the [YT] tag and username
# Working options: red, gold, white, green, blue, gray, tan, orange, yellow, dark_green, purple, light_green
# Note: "black" is supported but hard to see against the dark background.
prefix_color = "red"
# YouTube message text color ( This is ignored if you are in layout single mode. )
message_color = "white"

```
---

##  In-Game Commands  

You can control the plugin or reply to viewers directly from the in-game chat prompt or   
developer console:  
Chatting with Viewers (Normal In-Game Chat Box)  
Press Enter in-game and type:  
/tr <message> — Send a reply to your Twitch chat (e.g. /tr Thanks for the follow!).  
(Note: Requires configuring your username and oauth token in the TOML file or via /twitch auth). 

---

## Console Commands ( ctrl + ~ )

<img width="1128" height="330" alt="image" src="https://github.com/user-attachments/assets/7ebd5ab1-96f5-4e19-904d-29a9a3586e64" />


---

##  Building from Source  

This project uses CMake and C++20. It automatically downloads and configures the D2RLoader   
PluginSDK during compilation using CMake FetchContent—no manual SDK setup required!  
Prerequisites  
Windows 10/11 (64-bit)  
Visual Studio 2022 (with Desktop development with C++)  
CMake (v3.29 or newer)  

---

The compiled d2rl-streamchat.dll will be located in:  
build/Release/d2rl-streamchat.dll.  

---

##  License  

This project is open-source and distributed under the MIT License. See [LICENSE](LICENSE) for details.  

