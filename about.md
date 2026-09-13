# LiveyChat

Displays live YouTube chat messages in-game as a lightweight overlay in the
corner of your screen.

When a chatter's message contains a Geometry Dash level ID (a run of 6+ digits),
that ID is rendered as a clickable chip. Clicking it opens the level page for
that ID directly in Geometry Dash.

## Setup

1. Enable the mod and enter a **YouTube Video ID** (the 11-character ID of a
   live stream) in the mod's settings.
2. Add a free **YouTube Data API v3** key in the settings.
3. The overlay will resolve the active live chat for the stream and start
   fetching messages.

Optionally you can paste a raw `activeLiveChatId` into the **Live Chat ID**
field to skip the video -> chat resolution step.

## Notes

- Requires an internet connection and a YouTube Data API v3 key.
- Polling is rate-limited and asynchronous; it never blocks the game thread.
- Messages are fetched with the official YouTube Data API v3 (`liveChat/messages`).