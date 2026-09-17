#include <Geode/Geode.hpp>

#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/OverlayManager.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <arc/time/Sleep.hpp>

#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/LevelInfoLayer.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/GJSearchObject.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/LevelManagerDelegate.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Overlay geometry ("points"). Width shrinks to fit content; height is dynamic.
constexpr float kMaxPanelWidth = 360.0f;   // hard cap so very long messages wrap
constexpr float kMinPanelWidth = 150.0f;
constexpr float kTextWrapWidth = 320.0f;   // wrap width for message text
constexpr float kPad           = 10.0f;    // outer padding (default when headers are visible)
constexpr float kRowGap        = 3.0f;     // vertical gap between message rows
constexpr float kLineHeight    = 11.0f;    // flow-layout line height
constexpr float kWordGap       = 4.0f;     // horizontal gap between words/IDs

// Vibrant streamer username color palette (Twitch / Kick / Streamlabs style)
static const ccColor3B kStreamerColors[] = {
    { 255, 75, 75 },    // Bright Red
    { 59, 130, 246 },   // Royal / Dodger Blue
    { 34, 197, 94 },    // Spring Green
    { 168, 85, 247 },   // Violet / Purple
    { 249, 115, 22 },   // Vibrant Orange
    { 234, 179, 8 },    // Amber / Gold
    { 6, 182, 212 },    // Cyan / Teal
    { 132, 204, 22 },   // Lime Green
    { 236, 72, 153 },   // Hot Pink
    { 56, 189, 248 },   // Sky Blue
    { 234, 88, 12 },    // Deep Orange
    { 16, 185, 129 },   // Emerald / Mint
    { 192, 132, 252 },  // Lavender
    { 244, 63, 94 },    // Rose
    { 20, 184, 166 },   // Dark Teal
    { 250, 204, 21 },   // Yellow
};

static ccColor3B getAuthorColor(std::string const& author) {
    uint32_t hash = 5381;
    for (unsigned char c : author) {
        hash = ((hash << 5) + hash) + static_cast<uint32_t>(static_cast<unsigned char>(std::tolower(c)));
    }
    constexpr size_t count = sizeof(kStreamerColors) / sizeof(kStreamerColors[0]);
    return kStreamerColors[hash % count];
}

// Realistic diffused drop shadow (emulating Photopea / Photoshop external drop shadow blur)
struct ShadowTap {
    float dx;
    float dy;
    GLubyte opacity;
};

static const ShadowTap kPhotopeaShadow[] = {
    {  0.5f, -1.2f,  90 }, // Core center shadow (120° light angle)
    {  0.5f, -0.6f,  45 }, // Up feather
    {  0.5f, -1.8f,  45 }, // Down feather
    { -0.1f, -1.2f,  45 }, // Left feather
    {  1.1f, -1.2f,  45 }, // Right feather
    {  1.0f, -1.7f,  30 }, // Down-right diagonal feather
    {  0.0f, -1.7f,  30 }, // Down-left diagonal feather
};

// Creates a natural clean text label with a realistic blurred external drop shadow
static CCNode* createShadowedLabel(
    std::string const& text,
    const char* font,
    float scale,
    ccColor3B color,
    float* outWidth = nullptr,
    float* outHeight = nullptr,
    bool isClickable = false
) {
    auto* container = CCNode::create();
    container->ignoreAnchorPointForPosition(false);

    // Realistic diffused drop shadow taps (soft Gaussian-like falloff)
    for (auto const& tap : kPhotopeaShadow) {
        auto* shadow = CCLabelBMFont::create(text.c_str(), font);
        shadow->setScale(scale);
        shadow->setColor(ccc3(0, 0, 0));
        shadow->setOpacity(tap.opacity);
        shadow->setAnchorPoint(ccp(0.0f, 0.0f));
        shadow->setPosition(ccp(tap.dx, tap.dy));
        container->addChild(shadow);
    }

    // Main text label (clean, crisp, natural font weight - no artificial bold)
    auto* base = CCLabelBMFont::create(text.c_str(), font);
    base->setScale(scale);
    base->setColor(color);
    base->setAnchorPoint(ccp(0.0f, 0.0f));
    base->setPosition(ccp(0.0f, 0.0f));
    container->addChild(base);

    float w = base->getContentSize().width * scale;
    float h = base->getContentSize().height * scale;
    container->setContentSize(CCSize(w, h));

    if (isClickable) {
        container->setAnchorPoint(ccp(0.5f, 0.5f));
    } else {
        container->setAnchorPoint(ccp(0.0f, 0.0f));
    }

    if (outWidth) *outWidth = w;
    if (outHeight) *outHeight = h;

    return container;
}

// Regex used to detect candidate level IDs inside a chat message.
// Matches a run of digits (optionally separated by commas, since people insert
// commas to dodge chat filters), starting and ending with a digit. Results are
// then validated: commas are stripped and only 6+ digit numbers count as IDs
// (so "100%", "1,234 likes", etc. are ignored).
static const std::regex kLevelIDRegex(R"(\b\d[\d,]*\d\b)");

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

struct ChatMessage {
    std::string id;
    std::string author;
    std::string channelId;
    std::string text;
};

struct FetchResult {
    std::vector<ChatMessage> newMessages;
    std::vector<std::string> deletedMessageIds;
    std::vector<std::string> bannedChannelIds;
    std::vector<std::string> bannedUserNames;
    std::string status;     // short human-readable state ("Connected", errors…)
    uint64_t suggestedPollIntervalMs = 0;
};

// A snapshot of config read on the main thread, consumed by network coroutines.
struct Config {
    std::string videoId;
    std::string chatId;
    std::string apiKey;
    double pollInterval;
};

// ---------------------------------------------------------------------------
// Message tokenization (splits text into words + clickable level IDs)
// ---------------------------------------------------------------------------

enum class ChatTokenType { Word, LevelID, Newline };

struct Token {
    ChatTokenType type = ChatTokenType::Word;
    std::string text;
};

static void appendWords(std::vector<Token>& out, std::string const& chunk) {
    std::string cur;
    for (char c : chunk) {
        if (c == '\r') {
            continue;
        } else if (c == '\n') {
            if (!cur.empty()) {
                out.push_back(Token{ChatTokenType::Word, cur});
                cur.clear();
            }
            out.push_back(Token{ChatTokenType::Newline, "\n"});
        } else if (c == ' ' || c == '\t') {
            if (!cur.empty()) {
                out.push_back(Token{ChatTokenType::Word, cur});
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        out.push_back(Token{ChatTokenType::Word, cur});
}

static std::string stripCommas(std::string const& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != ',')
            out.push_back(c);
    return out;
}

static std::vector<Token> tokenizeMessage(std::string const& text) {
    std::vector<Token> out;
    size_t pos = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), kLevelIDRegex), end;
         it != end; ++it) {
        size_t start = static_cast<size_t>(it->position());
        size_t len   = static_cast<size_t>(it->length());
        if (start > pos)
            appendWords(out, text.substr(pos, start - pos));

        std::string raw      = it->str();
        std::string stripped = stripCommas(raw);

        if (stripped.size() >= 6) {
            try {
                (void)std::stoi(stripped);   // out of int range => not a GD level ID
                out.push_back(Token{ChatTokenType::LevelID, stripped});
            }
            catch (std::exception const&) {
                out.push_back(Token{ChatTokenType::Word, raw});
            }
        } else {
            out.push_back(Token{ChatTokenType::Word, raw});
        }
        pos = start + len;
    }
    if (pos < text.size())
        appendWords(out, text.substr(pos));
    return out;
}

// ---------------------------------------------------------------------------
// Configuration (runs on the main thread)
// ---------------------------------------------------------------------------

Config readConfig() {
    Config c;
    c.videoId      = Mod::get()->getSettingValue<std::string>("video-id");
    c.chatId       = Mod::get()->getSettingValue<std::string>("chat-id");
    c.apiKey       = Mod::get()->getSettingValue<std::string>("api-key");
    c.pollInterval = Mod::get()->getSettingValue<double>("poll-interval");
    return c;
}

// ---------------------------------------------------------------------------
// YouTube Data API v3 helpers (coroutines, run off the main thread)
// ---------------------------------------------------------------------------

// Extracts a YouTube video ID from any common input form, so users can paste
// whatever they have copied instead of trimming it manually:
//   "dQw4w9WgXcQ"
//   "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
//   "https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=120s"
//   "https://youtu.be/dQw4w9WgXcQ"
//   "https://www.youtube.com/live/dQw4w9WgXcQ"
static std::string extractVideoId(std::string input) {
    // Trim surrounding whitespace.
    size_t b = input.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = input.find_last_not_of(" \t\r\n");
    input = input.substr(b, e - b + 1);

    // Bare ID: no URL characters present; return as-is.
    if (input.find_first_of("/?&.=") == std::string::npos)
        return input;

    // "<anything>?v=ID" (watch URL, any host).
    if (auto p = input.find("v="); p != std::string::npos) {
        auto start = p + 2;
        auto end = input.find_first_of("&# ", start);
        return input.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

    // "youtu.be/ID", "/live/ID", "/shorts/ID", "/embed/ID".
    const std::array<std::string_view, 4> prefixes = {
        "youtu.be/", "/live/", "/shorts/", "/embed/"
    };
    for (auto pat : prefixes) {
        if (auto p = input.find(pat); p != std::string::npos) {
            auto start = p + pat.size();
            auto end = input.find_first_of("&#? /", start);
            return input.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
    }

    return "";
}

static arc::Future<Result<std::string, std::string>> resolveChatId(Config const& cfg) {
    auto videoId = extractVideoId(cfg.videoId);
    if (videoId.empty())
        co_return Err("Could not parse a video ID from that input. (ID like \"dQw4w9WgXcQ\" or a full YouTube link)");

    auto url = fmt::format(
        "https://www.googleapis.com/youtube/v3/videos?part=liveStreamingDetails&id={}&key={}",
        videoId, cfg.apiKey);

    web::WebRequest req;
    auto res = co_await req.get(url);
    if (!res.ok()) {
        std::string errDetail;
        auto parsed = matjson::parse(res.string().unwrapOr("{}"));
        if (parsed) {
            auto root = parsed.unwrap();
            if (root.contains("error") && root["error"].contains("message")) {
                errDetail = root["error"]["message"].asString().unwrapOr("");
            }
        }
        if (!errDetail.empty()) {
            co_return Err(fmt::format("YouTube error (HTTP {}): {}", res.code(), errDetail));
        }
        co_return Err(fmt::format("YouTube request failed (HTTP {})", res.code()));
    }

    auto parsed = matjson::parse(res.string().unwrapOr("{}"));
    if (!parsed)
        co_return Err("Invalid JSON from YouTube.");

    auto root = parsed.unwrap();
    if (root.contains("error")) {
        std::string msg = "unknown";
        if (root["error"].contains("message"))
            msg = root["error"]["message"].asString().unwrapOr("unknown");
        co_return Err(fmt::format("YouTube API error: {}", msg));
    }

    if (!root.contains("items") || !root["items"].isArray() || root["items"].size() == 0)
        co_return Err("No live stream found for that video (is it public and live?).");

    auto const& item = root["items"][0];
    if (!item.contains("liveStreamingDetails"))
        co_return Err("That video is not a live stream.");

    auto const& details = item["liveStreamingDetails"];
    if (!details.contains("activeLiveChatId")) {
        if (details.contains("actualEndTime"))
            co_return Err("That stream has already ended (chat is only readable while live).");
        co_return Err("Live chat is not active for that stream (it may have chat disabled).");
    }

    auto id = details["activeLiveChatId"].asString();
    if (!id)
        co_return Err("Failed to read the live chat id.");
    co_return Ok(id.unwrap());
}

static arc::Future<Result<FetchResult, std::string>> fetchChatMessages(
    Config const& cfg, std::string const& chatId, std::string& pageToken)
{
    FetchResult out;

    std::string url = fmt::format(
        "https://www.googleapis.com/youtube/v3/liveChat/messages?part=snippet,authorDetails&liveChatId={}&key={}",
        chatId, cfg.apiKey);
    if (!pageToken.empty())
        url += "&pageToken=" + pageToken;

    web::WebRequest req;
    auto res = co_await req.get(url);
    if (!res.ok()) {
        std::string errDetail;
        auto parsed = matjson::parse(res.string().unwrapOr("{}"));
        if (parsed) {
            auto root = parsed.unwrap();
            if (root.contains("error") && root["error"].contains("message")) {
                errDetail = root["error"]["message"].asString().unwrapOr("");
            }
        }
        if (!errDetail.empty()) {
            co_return Err(fmt::format("YouTube error (HTTP {}): {}", res.code(), errDetail));
        }
        co_return Err(fmt::format("Chat request failed (HTTP {})", res.code()));
    }

    auto parsed = matjson::parse(res.string().unwrapOr("{}"));
    if (!parsed)
        co_return Err("Invalid JSON from the chat endpoint.");

    auto root = parsed.unwrap();
    if (root.contains("error")) {
        std::string msg = "unknown";
        if (root["error"].contains("message"))
            msg = root["error"]["message"].asString().unwrapOr("unknown");
        co_return Err(fmt::format("YouTube API error: {}", msg));
    }

    if (root.contains("pollingIntervalMillis")) {
        if (auto ms = root["pollingIntervalMillis"].asInt()) {
            out.suggestedPollIntervalMs = static_cast<uint64_t>(ms.unwrap());
        }
    }

    // Always advance pagination forward monotonically
    if (root.contains("nextPageToken")) {
        if (auto tok = root["nextPageToken"].asString())
            pageToken = tok.unwrap();
    }

    if (root.contains("items") && root["items"].isArray()) {
        auto items = root["items"].asArray();
        if (items) {
            for (auto const& it : items.unwrap()) {
                std::string itemId = it.contains("id") ? it["id"].asString().unwrapOr("") : "";

                std::string type = "textMessageEvent";
                if (it.contains("snippet") && it["snippet"].contains("type")) {
                    type = it["snippet"]["type"].asString().unwrapOr("textMessageEvent");
                }

                // 1. Message deleted event
                if (type == "messageDeletedEvent") {
                    if (it.contains("snippet") && it["snippet"].contains("messageDeletedDetails")) {
                        auto const& del = it["snippet"]["messageDeletedDetails"];
                        if (del.contains("deletedMessageId")) {
                            std::string delId = del["deletedMessageId"].asString().unwrapOr("");
                            if (!delId.empty()) out.deletedMessageIds.push_back(delId);
                        }
                    }
                    continue;
                }

                // 2. Message retracted event
                if (type == "messageRetractedEvent") {
                    if (it.contains("snippet") && it["snippet"].contains("messageRetractedDetails")) {
                        auto const& ret = it["snippet"]["messageRetractedDetails"];
                        if (ret.contains("retractedMessageId")) {
                            std::string retId = ret["retractedMessageId"].asString().unwrapOr("");
                            if (!retId.empty()) out.deletedMessageIds.push_back(retId);
                        }
                    }
                    continue;
                }

                // 3. User banned / hidden event
                if (type == "userBannedEvent") {
                    if (it.contains("snippet") && it["snippet"].contains("userBannedDetails")) {
                        auto const& ban = it["snippet"]["userBannedDetails"];
                        if (ban.contains("bannedUserDetails")) {
                            auto const& user = ban["bannedUserDetails"];
                            if (user.contains("channelId")) {
                                std::string chId = user["channelId"].asString().unwrapOr("");
                                if (!chId.empty()) out.bannedChannelIds.push_back(chId);
                            }
                            if (user.contains("displayName")) {
                                std::string name = user["displayName"].asString().unwrapOr("");
                                if (!name.empty()) out.bannedUserNames.push_back(name);
                            }
                        }
                    }
                    continue;
                }

                // Standard message
                ChatMessage msg;
                msg.id = itemId;

                if (it.contains("authorDetails")) {
                    auto const& auth = it["authorDetails"];
                    if (auth.contains("displayName"))
                        msg.author = auth["displayName"].asString().unwrapOr("Viewer");
                    else
                        msg.author = "Viewer";
                    if (auth.contains("channelId"))
                        msg.channelId = auth["channelId"].asString().unwrapOr("");
                } else if (it.contains("snippet") && it["snippet"].contains("authorChannelId")) {
                    msg.channelId = it["snippet"]["authorChannelId"].asString().unwrapOr("");
                    msg.author = "Viewer";
                } else {
                    msg.author = "Viewer";
                }

                if (it.contains("snippet") && it["snippet"].contains("displayMessage"))
                    msg.text = it["snippet"]["displayMessage"].asString().unwrapOr("");

                if (msg.text.empty())
                    continue;

                out.newMessages.push_back(std::move(msg));
            }
        }
    }

    out.status = out.newMessages.empty() ? "Connected - waiting for messages..." : "Connected";
    co_return Ok(out);
}

static arc::Future<FetchResult> doFetch(Config const& cfg, std::string& chatId, std::string& pageToken) {
    FetchResult out;

    // Resolve the active chat id on first run only (or if it changed).
    if (chatId.empty()) {
        chatId = cfg.chatId;
        if (chatId.empty()) {
            if (cfg.videoId.empty()) {
                out.status = "Not configured: set a Video ID (or Live Chat ID).";
                co_return out;
            }
            if (cfg.apiKey.empty()) {
                out.status = "YouTube Data API key required (add it in settings).";
                co_return out;
            }
            auto resolved = co_await resolveChatId(cfg);
            if (!resolved) {
                out.status = resolved.unwrapErr();
                co_return out;
            }
            chatId = resolved.unwrap();
            pageToken.clear();
        }
    }

    if (cfg.apiKey.empty()) {
        out.status = "YouTube Data API key required (add it in settings).";
        co_return out;
    }

    auto fetched = co_await fetchChatMessages(cfg, chatId, pageToken);
    if (!fetched) {
        out.status = fetched.unwrapErr();
        co_return out;
    }
    co_return fetched.unwrap();
}

// ---------------------------------------------------------------------------
// The overlay
// ---------------------------------------------------------------------------

class LiveyChatOverlay : public CCLayer, public LevelManagerDelegate {
public:
    static LiveyChatOverlay* create();
    static LiveyChatOverlay* get() { return m_instance; }

    void applyEnabled(bool on);
    void applyOpacity();
    void applyPosition();
    void applyVisibilityFlags();
    void relayout();
    void restartPolling();

    // LevelManagerDelegate: called when the requested level's metadata arrives.
    void loadLevelsFinished(cocos2d::CCArray* levels, char const* key) override;
    void loadLevelsFailed(char const* key) override;
    void loadLevelsFinished(cocos2d::CCArray* levels, char const* key, int type) override;
    void loadLevelsFailed(char const* key, int type) override;

private:
    bool init() override;

    // Networking -----------------------------------------------------------
    void startPolling();
    void stopPolling();
    arc::Future<void> pollLoop();

    // Rendering ------------------------------------------------------------
    void handleFetchResult(FetchResult const& f);
    CCNode* buildMessageRow(ChatMessage const& message);
    CCMenuItemSpriteExtra* buildIdItem(int levelID);
    void setStatus(std::string const& status);

    // Callbacks ------------------------------------------------------------
    void onLevelIDClicked(CCObject* sender);
    void openLevel(int levelID);
    void handleLevelsLoaded(cocos2d::CCArray* levels);
    void restoreLevelManagerDelegate();

    // Members ---------------------------------------------------------------
    struct RowEntry {
        CCNode* node = nullptr;
        std::string messageId;
        std::string author;
        std::string channelId;
    };

    CCLayerColor* m_bg      = nullptr;
    CCLabelBMFont* m_title  = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCNode* m_content       = nullptr;

    std::vector<RowEntry> m_rows;   // oldest at front (top), newest at back (bottom)
    std::unordered_set<std::string> m_bannedChannelIds;
    std::unordered_set<std::string> m_bannedUserNames;
    std::unordered_set<std::string> m_seenMessageIds;

    LevelManagerDelegate* m_prevManagerDelegate = nullptr;

    arc::TaskHandle<void> m_pollTask;

    static LiveyChatOverlay* m_instance;
};

LiveyChatOverlay* LiveyChatOverlay::m_instance = nullptr;

LiveyChatOverlay* LiveyChatOverlay::create() {
    auto ret = new LiveyChatOverlay();
    if (ret && ret->init()) {
        ret->autorelease();
        m_instance = ret;
        return ret;
    }
    delete ret;
    return nullptr;
}

bool LiveyChatOverlay::init() {
    if (!CCLayer::init())
        return false;

    // Keep the layer anchored at its bottom-left corner; we position it
    // manually per the "position" setting so all four corners work.
    this->setAnchorPoint(ccp(0.0f, 0.0f));

    m_bg = CCLayerColor::create(ccc4(0, 0, 0, 140), kMaxPanelWidth, 100.0f);
    this->addChild(m_bg, -1);

    m_title = CCLabelBMFont::create("LIVE CHAT", "bigFont.fnt");
    m_title->setScale(0.55f);
    m_title->setAnchorPoint(ccp(0.0f, 1.0f));
    this->addChild(m_title);

    m_status = CCLabelBMFont::create("Not connected", "chatFont.fnt");
    m_status->setScale(0.4f);
    m_status->setAnchorPoint(ccp(0.0f, 1.0f));
    this->addChild(m_status);

    m_content = CCNode::create();
    m_content->ignoreAnchorPointForPosition(false);
    m_content->setAnchorPoint(ccp(0.0f, 0.0f));
    this->addChild(m_content);

    this->applyOpacity();
    this->applyVisibilityFlags();

    return true;
}

// --- Networking ------------------------------------------------------------

void LiveyChatOverlay::startPolling() {
    this->stopPolling();
    auto self = this;
    m_pollTask = async::spawn([self]() -> arc::Future<void> {
        co_await self->pollLoop();
    });
    m_pollTask.setName("liveychat-poll");
}

void LiveyChatOverlay::stopPolling() {
    if (m_pollTask) {
        m_pollTask.abort();
        m_pollTask = {};
    }
}

arc::Future<void> LiveyChatOverlay::pollLoop() {
    std::string chatId;     // resolved once per polling session
    std::string pageToken;

    while (true) {
        double delay = 5.0;
        try {
            auto cfg = co_await async::waitForMainThread<Config>([] { return readConfig(); });
            if (!cfg)
                co_return;

            auto fetch = co_await doFetch(*cfg, chatId, pageToken);

            if (fetch.suggestedPollIntervalMs > 0) {
                double suggestedSec = static_cast<double>(fetch.suggestedPollIntervalMs) / 1000.0;
                delay = std::max(cfg->pollInterval, suggestedSec);
            } else {
                delay = cfg->pollInterval;
            }

            co_await async::waitForMainThread<void>([this, f = std::move(fetch)]() mutable {
                this->setStatus(f.status);
                this->handleFetchResult(f);
            });
        }
        catch (std::exception const& e) {
            log::warn("LiveyChat poll error: {}", e.what());
            delay = 5.0;
        }

        co_await arc::sleep(asp::Duration::fromMillis(static_cast<std::uint64_t>(delay * 1000.0)));
    }
    co_return;
}

// --- Settings --------------------------------------------------------------

void LiveyChatOverlay::applyEnabled(bool on) {
    this->setVisible(on);
    if (on) {
        this->startPolling();
    } else {
        this->stopPolling();
    }
}

void LiveyChatOverlay::applyOpacity() {
    double op = Mod::get()->getSettingValue<double>("opacity");
    if (m_bg)
        m_bg->setOpacity(static_cast<GLubyte>(255.0 * op));
}

void LiveyChatOverlay::applyVisibilityFlags() {
    if (m_title)
        m_title->setVisible(Mod::get()->getSettingValue<bool>("show-title"));
    if (m_status)
        m_status->setVisible(Mod::get()->getSettingValue<bool>("show-status"));
    this->applyPosition();
}

void LiveyChatOverlay::applyPosition() {
    this->relayout();
    auto vs = CCDirector::sharedDirector()->getWinSize();
    float margin = 16.0f;

    std::string pos = Mod::get()->getSettingValue<std::string>("position");
    if (pos.empty()) pos = "top-right";

    float w = this->getContentSize().width;
    float h = this->getContentSize().height;

    CCPoint p;
    if (pos == "top-left")            p = ccp(margin, vs.height - margin - h);
    else if (pos == "bottom-right")   p = ccp(vs.width - margin - w, margin);
    else if (pos == "bottom-left")    p = ccp(margin, margin);
    else                              p = ccp(vs.width - margin - w, vs.height - margin - h); // top-right

    this->setPosition(p);
}

void LiveyChatOverlay::restartPolling() {
    if (this->isVisible())
        this->startPolling();
}

// --- Rendering -------------------------------------------------------------

void LiveyChatOverlay::handleFetchResult(FetchResult const& f) {
    bool changed = false;

    // 1. Remove deleted messages by message ID
    for (auto const& delId : f.deletedMessageIds) {
        auto it = std::find_if(m_rows.begin(), m_rows.end(), [&](RowEntry const& r) {
            return !r.messageId.empty() && r.messageId == delId;
        });
        if (it != m_rows.end()) {
            it->node->removeFromParentAndCleanup(true);
            m_rows.erase(it);
            changed = true;
        }
        if (!delId.empty()) {
            m_seenMessageIds.insert(delId);
        }
    }

    // 2. Track banned / hidden users and immediately purge their messages
    for (auto const& chId : f.bannedChannelIds) {
        if (!chId.empty()) m_bannedChannelIds.insert(chId);
    }
    for (auto const& name : f.bannedUserNames) {
        if (!name.empty()) m_bannedUserNames.insert(name);
    }

    if (!f.bannedChannelIds.empty() || !f.bannedUserNames.empty()) {
        auto it = m_rows.begin();
        while (it != m_rows.end()) {
            bool isBanned = false;
            if (!it->channelId.empty() && m_bannedChannelIds.count(it->channelId)) isBanned = true;
            if (!it->author.empty() && m_bannedUserNames.count(it->author)) isBanned = true;
            if (isBanned) {
                if (!it->messageId.empty()) m_seenMessageIds.insert(it->messageId);
                it->node->removeFromParentAndCleanup(true);
                it = m_rows.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }

    // 3. Add new messages (deduplicated against all previously seen messages)
    int64_t maxRaw = Mod::get()->getSettingValue<int64_t>("max-messages");
    int maxMsgs = static_cast<int>(std::clamp<int64_t>(maxRaw, 1, 20));

    for (auto const& message : f.newMessages) {
        // Drop messages from banned users
        if (!message.channelId.empty() && m_bannedChannelIds.count(message.channelId)) continue;
        if (!message.author.empty() && m_bannedUserNames.count(message.author)) continue;

        // Prevent duplicate or previously seen messages from ever re-appearing
        if (!message.id.empty()) {
            if (m_seenMessageIds.count(message.id)) continue;
            m_seenMessageIds.insert(message.id);
        }

        auto* rowNode = this->buildMessageRow(message);
        m_content->addChild(rowNode);
        m_rows.push_back(RowEntry{ rowNode, message.id, message.author, message.channelId });
        changed = true;
    }

    // 4. Enforce max-messages (oldest at top are removed first)
    while (static_cast<int>(m_rows.size()) > maxMsgs) {
        auto const& first = m_rows.front();
        first.node->removeFromParentAndCleanup(true);
        m_rows.erase(m_rows.begin());
        changed = true;
    }

    if (changed) {
        this->applyPosition();
    }
}

CCNode* LiveyChatOverlay::buildMessageRow(ChatMessage const& message) {
    struct LineItem {
        CCNode* node = nullptr;
        bool isMenuItem = false;
        float x = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
    };

    struct RowLine {
        std::vector<LineItem> items;
        float width = 0.0f;
    };

    std::vector<RowLine> lines;
    RowLine currentLine;
    float curX = 0.0f;

    // 1. Author (colored according to username, Streamlabs Clean style: no colon)
    float authorW = 0.0f, authorH = 0.0f;
    ccColor3B authorColor = getAuthorColor(message.author);
    auto* authorNode = createShadowedLabel(
        message.author, "chatFont.fnt", 0.5f, authorColor, &authorW, &authorH, false
    );
    currentLine.items.push_back(LineItem{ authorNode, false, curX, authorW, authorH });
    curX += authorW + kWordGap;
    currentLine.width = curX;

    // 2. Message tokens (clean white text, clickable green level IDs with realistic shadow)
    for (auto const& tok : tokenizeMessage(message.text)) {
        if (tok.type == ChatTokenType::Newline) {
            lines.push_back(std::move(currentLine));
            currentLine = RowLine{};
            curX = 0.0f;
            continue;
        }

        if (tok.type == ChatTokenType::LevelID) {
            int levelID = 0;
            try { levelID = std::stoi(tok.text); } catch (...) {}
            auto* item = this->buildIdItem(levelID);
            float w = item->getContentSize().width;
            float h = item->getContentSize().height;
            if (curX > 0.0f && curX + w > kTextWrapWidth) {
                lines.push_back(std::move(currentLine));
                currentLine = RowLine{};
                curX = 0.0f;
            }
            currentLine.items.push_back(LineItem{ item, true, curX, w, h });
            curX += w + kWordGap;
            currentLine.width = curX;
        } else {
            float w = 0.0f, h = 0.0f;
            auto* wordNode = createShadowedLabel(
                tok.text, "chatFont.fnt", 0.5f, ccc3(255, 255, 255), &w, &h, false
            );
            if (curX > 0.0f && curX + w > kTextWrapWidth) {
                lines.push_back(std::move(currentLine));
                currentLine = RowLine{};
                curX = 0.0f;
            }
            currentLine.items.push_back(LineItem{ wordNode, false, curX, w, h });
            curX += w + kWordGap;
            currentLine.width = curX;
        }
    }

    if (!currentLine.items.empty()) {
        lines.push_back(std::move(currentLine));
    }

    if (lines.empty()) {
        lines.push_back(RowLine{});
    }

    size_t numLines = lines.size();
    float rowH = static_cast<float>(numLines) * kLineHeight;
    float maxExtent = 0.0f;
    for (auto const& l : lines) {
        maxExtent = std::max(maxExtent, l.width);
    }
    float rowW = std::min(maxExtent, kTextWrapWidth);

    auto* row = CCNode::create();
    row->ignoreAnchorPointForPosition(false);
    row->setAnchorPoint(ccp(0.0f, 0.0f));
    row->setContentSize(CCSize(rowW, rowH));

    auto* menu = CCMenu::create();
    menu->ignoreAnchorPointForPosition(false);
    menu->setAnchorPoint(ccp(0.0f, 0.0f));
    menu->setPosition(ccp(0.0f, 0.0f));
    menu->setContentSize(CCSize(rowW, rowH));
    row->addChild(menu);

    for (size_t lineIdx = 0; lineIdx < numLines; ++lineIdx) {
        // lineIdx 0 is top line; lineIdx (numLines - 1) is bottom line
        float lineBottom = rowH - static_cast<float>(lineIdx + 1) * kLineHeight;
        for (auto const& item : lines[lineIdx].items) {
            if (item.isMenuItem) {
                item.node->setPosition(ccp(item.x + item.width / 2.0f, lineBottom + item.height / 2.0f));
                menu->addChild(item.node);
            } else {
                item.node->setPosition(ccp(item.x, lineBottom));
                row->addChild(item.node);
            }
        }
    }

    return row;
}

CCMenuItemSpriteExtra* LiveyChatOverlay::buildIdItem(int levelID) {
    float w = 0.0f, h = 0.0f;
    auto* labelNode = createShadowedLabel(
        std::to_string(levelID), "chatFont.fnt", 0.5f, ccc3(90, 240, 90), &w, &h, true
    );

    auto* item = CCMenuItemSpriteExtra::create(
        labelNode, this, menu_selector(LiveyChatOverlay::onLevelIDClicked));
    item->setTag(levelID);
    item->setContentSize(CCSize(w, h));
    return item;
}

void LiveyChatOverlay::relayout() {
    bool titleVisible  = m_title  && m_title->isVisible();
    bool statusVisible = m_status && m_status->isVisible();
    bool hasHeaders    = titleVisible || statusVisible;

    // When all headers are disabled and there are no messages,
    // collapse completely and hide the background (eliminates awkward empty box)
    if (!hasHeaders && m_rows.empty()) {
        this->setContentSize(CCSize(0.0f, 0.0f));
        if (m_bg) {
            m_bg->setVisible(false);
            m_bg->setContentSize(CCSize(0.0f, 0.0f));
        }
        return;
    }

    if (m_bg) {
        m_bg->setVisible(true);
    }

    // Streamlabs style: tight, clean vertical padding when headers are hidden
    float padTop    = hasHeaders ? 6.0f : 4.0f;
    float padBottom = hasHeaders ? 6.0f : 4.0f;
    float padLeft   = hasHeaders ? 8.0f : 6.0f;
    float padRight  = hasHeaders ? 8.0f : 6.0f;

    // Compute total content height dynamically based on each row's actual height
    float contentH = 0.0f;
    float maxRowW = 0.0f;
    for (size_t i = 0; i < m_rows.size(); ++i) {
        float rh = m_rows[i].node->getContentSize().height;
        float rw = m_rows[i].node->getContentSize().width;
        contentH += rh;
        if (i + 1 < m_rows.size()) {
            contentH += kRowGap;
        }
        maxRowW = std::max(maxRowW, rw);
    }

    // Stack rows from top to bottom:
    // m_rows[0] is oldest (at the top)
    // m_rows.back() is newest (at the bottom)
    float curY = contentH;
    for (auto const& entry : m_rows) {
        float rh = entry.node->getContentSize().height;
        curY -= rh;
        entry.node->setPosition(ccp(padLeft, curY));
        curY -= kRowGap;
    }

    float titleH  = titleVisible  ? m_title->getContentSize().height  * m_title->getScaleY()  : 0.0f;
    float titleW  = titleVisible  ? m_title->getContentSize().width   * m_title->getScaleX()  : 0.0f;
    float statusH = statusVisible ? m_status->getContentSize().height * m_status->getScaleY() : 0.0f;
    float statusW = statusVisible ? m_status->getContentSize().width  * m_status->getScaleX() : 0.0f;

    float minW = hasHeaders ? kMinPanelWidth : (m_rows.empty() ? 0.0f : 40.0f);
    float panelW = std::clamp(
        std::max({ maxRowW, titleW, statusW }) + padLeft + padRight,
        minW, kMaxPanelWidth);

    float headerH = 0.0f;
    if (titleVisible)  headerH += titleH;
    if (titleVisible && statusVisible) headerH += 2.0f;
    if (statusVisible) headerH += statusH;
    if (hasHeaders && !m_rows.empty()) headerH += 4.0f;

    float totalH = padTop + headerH + contentH + padBottom;

    this->setContentSize(CCSize(panelW, totalH));
    if (m_bg)
        m_bg->setContentSize(CCSize(panelW, totalH));

    // Position header elements from the top down.
    float top = totalH - padTop;
    if (titleVisible) {
        m_title->setPosition(ccp(padLeft, top));
        top -= titleH;
        if (statusVisible) top -= 2.0f;
    }
    if (statusVisible) {
        m_status->setPosition(ccp(padLeft, top));
        top -= statusH;
    }
    if (hasHeaders && !m_rows.empty()) {
        top -= 4.0f;
    }
    if (m_content) {
        m_content->setContentSize(CCSize(panelW, contentH));
        m_content->setPosition(ccp(0.0f, top - contentH));
    }
}

void LiveyChatOverlay::setStatus(std::string const& status) {
    if (m_status) {
        m_status->setString(status.c_str());
        if (m_status->isVisible()) {
            this->applyPosition();
        }
    }
}

// --- Callbacks -------------------------------------------------------------

void LiveyChatOverlay::onLevelIDClicked(CCObject* sender) {
    int levelID = sender->getTag();
    this->openLevel(levelID);
}

void LiveyChatOverlay::openLevel(int levelID) {
    auto glm = GameLevelManager::sharedState();
    m_prevManagerDelegate = glm->m_levelManagerDelegate;
    glm->m_levelManagerDelegate = this;

    // Fetch the level's metadata (name, creator, difficulty, ...) before
    // opening its page, so the info screen isn't blank. `gameVersion=22` forces
    // GD 2.2 level data format (same as Geode's own markdown level links).
    auto search = GJSearchObject::create(
        SearchType::Type19, std::to_string(levelID) + "&gameVersion=22");
    glm->getOnlineLevels(search);
}

void LiveyChatOverlay::restoreLevelManagerDelegate() {
    auto glm = GameLevelManager::sharedState();
    glm->m_levelManagerDelegate = m_prevManagerDelegate;
    m_prevManagerDelegate = nullptr;
}

void LiveyChatOverlay::handleLevelsLoaded(cocos2d::CCArray* levels) {
    this->restoreLevelManagerDelegate();

    if (levels && levels->count() > 0) {
        auto* level = static_cast<GJGameLevel*>(levels->objectAtIndex(0));
        auto* scene = LevelInfoLayer::scene(level, false);
        CCDirector::sharedDirector()->pushScene(scene);
    } else {
        log::warn("LiveyChat: level not found");
    }
}

void LiveyChatOverlay::loadLevelsFinished(cocos2d::CCArray* levels, char const* key) {
    this->handleLevelsLoaded(levels);
}

void LiveyChatOverlay::loadLevelsFailed(char const* key) {
    this->restoreLevelManagerDelegate();
}

void LiveyChatOverlay::loadLevelsFinished(cocos2d::CCArray* levels, char const* key, int type) {
    this->handleLevelsLoaded(levels);
}

void LiveyChatOverlay::loadLevelsFailed(char const* key, int type) {
    this->restoreLevelManagerDelegate();
}

// ---------------------------------------------------------------------------
// Mod entry point
// ---------------------------------------------------------------------------

$on_mod(Loaded) {
    auto* overlay = LiveyChatOverlay::create();
    OverlayManager::get()->addChild(overlay);

    overlay->applyEnabled(Mod::get()->getSettingValue<bool>("enabled"));

    // React to setting changes without restarting the game.
    listenForSettingChanges<bool>("enabled", [](bool v) {
        if (auto* o = LiveyChatOverlay::get()) o->applyEnabled(v);
    });
    listenForSettingChanges<std::string>("position", [](std::string const&) {
        if (auto* o = LiveyChatOverlay::get()) o->applyPosition();
    });
    listenForSettingChanges<double>("opacity", [](double) {
        if (auto* o = LiveyChatOverlay::get()) o->applyOpacity();
    });
    listenForSettingChanges<int64_t>("max-messages", [](int64_t) {
        if (auto* o = LiveyChatOverlay::get()) o->applyPosition();
    });
    listenForSettingChanges<bool>("show-title", [](bool) {
        if (auto* o = LiveyChatOverlay::get()) o->applyVisibilityFlags();
    });
    listenForSettingChanges<bool>("show-status", [](bool) {
        if (auto* o = LiveyChatOverlay::get()) o->applyVisibilityFlags();
    });

    // Network-related settings: restart the polling loop so they take effect.
    listenForSettingChanges<std::string>("video-id", [](std::string const&) {
        if (auto* o = LiveyChatOverlay::get()) o->restartPolling();
    });
    listenForSettingChanges<std::string>("chat-id", [](std::string const&) {
        if (auto* o = LiveyChatOverlay::get()) o->restartPolling();
    });
    listenForSettingChanges<std::string>("api-key", [](std::string const&) {
        if (auto* o = LiveyChatOverlay::get()) o->restartPolling();
    });
}