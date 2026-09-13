# LiveyChat

LiveyChat is a [Geode](https://geode-sdk.org/) mod for Geometry Dash that
displays a YouTube live stream's chat directly inside the game. When someone
mentions a Geometry Dash level ID in chat, LiveyChat turns it into a green,
clickable number that opens that level in-game.

---

## Features

- **Live chat overlay** shown in the corner of the screen (top-right by
  default, position is configurable).
- **Automatic level-ID detection** inside chat messages. This handles plain
  numbers (`148812853`) as well as comma-separated numbers that people use to
  get around chat filters (`146,175,134` or `1,4,6,1,7,5,1,3,4`). The commas
  are stripped and the number becomes clickable.
- **Click-to-open** - clicking a green level ID opens that level's page in
  Geometry Dash.
- **Fully asynchronous** - chat fetching runs off the game thread, so nothing
  freezes. Requests are rate-limited with a configurable poll interval.
- **Configurable overlay** - toggle it on/off, change its position, adjust the
  background opacity, cap the number of visible messages, hide the title or the
  status line.
- **Clear status and error messages** - the overlay tells you when it is
  connected, waiting, or what exactly is wrong (missing key, not a live stream,
  live chat disabled, etc.).

---

## Requirements

- Geometry Dash **2.2081** or later (the 2.2 Steam version).
- [Geode](https://geode-sdk.org/install) **v5.x** installed and working.
- An internet connection.
- A free **YouTube Data API v3** key (setup steps below).

---

## Installation

### Option A - use a prebuilt release

1. Download `liveychat.youtube-chat.geode` from the
   [Releases](https://github.com/ArthurSimin/LiveyChat/releases) page.
2. Place the file inside Geometry Dash's Geode mods folder:

   - Windows (Steam):
     `C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash\geode\mods\`

   You can also open that folder from inside the game (Geode menu, then the
   "Open Mods Folder" button).
3. Launch Geometry Dash. LiveyChat appears in the Geode mods list.

### Option B - build it yourself

```bash
# Requires the Geode CLI and SDK (https://docs.geode-sdk.org/getting-started/)
geode build
```

This compiles the mod and installs it into your game automatically.

---

## Getting a YouTube Data API key

LiveyChat reads chat through the official YouTube Data API v3, which requires a
free API key.

1. Go to <https://console.cloud.google.com/> and sign in.
2. Create a new project (or select an existing one).
3. Open **APIs & Services -> Library**, search for **YouTube Data API v3**,
   and click **Enable**.
4. Open **APIs & Services -> Credentials -> Create credentials -> API key**.
5. Copy the key. Optionally edit it and restrict it to **YouTube Data API v3**
   only (leave "Application restrictions" set to none, since the request comes
   from your own machine).

For this kind of public-data request, choose the **API key / public
credentials** option, not the OAuth "user credentials" option.

---

## Configuration

Open Geometry Dash, go to the Geode menu, find **LiveyChat**, and open its
settings. The available settings are:

| Setting | Type | Default | Description |
|---|---|---|---|
| `Enabled` | bool | on | Show the chat overlay and start fetching. |
| `YouTube Video ID` | string | empty | The 11-character ID of a **live** stream (from the video URL). |
| `Live Chat ID (optional)` | string | empty | A raw `activeLiveChatId`. Overrides the video ID when set. |
| `YouTube Data API Key` | string | empty | Your YouTube Data API v3 key. |
| `Overlay Position` | choice | top-right | Where the overlay anchors (`top-right`, `top-left`, `bottom-right`, `bottom-left`). |
| `Background Opacity` | float | 0.55 | Opacity of the overlay background (0 to 1). |
| `Max Messages` | int | 7 | How many recent messages are kept on screen (1 to 20). |
| `Poll Interval (seconds)` | float | 5 | How often to fetch new messages (1 to 60). |
| `Show Title` | bool | on | Show the "LIVE CHAT" header. |
| `Show Status` | bool | on | Show the connection/error status line. |

---

## How to use it

1. Fill in the **YouTube Video ID** of a stream that is live right now, and
   your **API key**.
2. Turn on **Enabled**.
3. The overlay appears and its status line changes to `Connected` once the live
   chat is resolved.
4. New messages scroll in. Any message that contains a level ID shows that
   number in green.
5. Click a green number to open that level's page.

---

## Troubleshooting

The overlay's status line reports what is happening:

| Status | Meaning / fix |
|---|---|
| `Not connected` | Overlay started but nothing fetched yet. |
| `Not configured: set a Video ID (or Live Chat ID).` | Fill in the video ID (or chat ID). |
| `YouTube Data API key required` | Fill in the API key. |
| `No live stream found...` | The video is not live, not public, or the ID is wrong. |
| `That video is not a live stream.` | The video is a normal/uploaded video. |
| `Live chat is not active...` | The stream is live but its chat is disabled. |
| `YouTube API error: ...` | Usually an invalid/disabled API key. |

---

## Building from source

Prerequisites:

- Geode CLI and SDK (see the
  [Geode getting-started guide](https://docs.geode-sdk.org/getting-started/)).
- A C++23 toolchain. On Linux, building a Windows mod also requires the
  MinGW-w64 cross-compiler and Windows SDK binaries
  (`geode sdk install-linux`).

```bash
git clone https://github.com/ArthurSimin/LiveyChat.git
cd LiveyChat
geode build
```

The packaged mod is written to `build/liveychat.youtube-chat.geode` and is
installed automatically if a Geode profile is configured.

---

## How it works

- **Chat fetching** uses the YouTube Data API v3: it resolves the video ID to an
  `activeLiveChatId`, then polls `liveChat/messages` with page tokens.
- **Networking** runs as a Geode async task (`web::WebRequest` + `co_await`),
  so the game thread is never blocked; results are applied back on the main
  thread.
- **Rendering** uses a cocos2d `CCLayer` added to Geode's `OverlayManager`, so
  it persists across every scene. Messages are laid out as word-wrapped text,
  and detected IDs are individual `CCMenuItemSpriteExtra` nodes.
- **Level detection** uses a regex, strips commas, and validates the result is
  a 6+ digit number that fits in an int. Clicking an ID fetches the level's
  metadata first, then opens its page.

---

## Notes / limitations

- The free YouTube Data API quota is 10,000 units/day, and each
  `liveChat/messages` call costs 5 units. At the default 5-second interval a
  long stream can exhaust the daily quota in a few hours; increase the poll
  interval for long sessions.
- There is no reliable way to tell a comma-separated level ID apart from a
  large comma-separated number (for example "1,000,000"), so any 6+ digit
  number, with or without commas, is treated as a level ID.

---

## License

Licensed under the MIT License. See [LICENSE](LICENSE).