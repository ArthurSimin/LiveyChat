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
#include <cstdint>
#include <regex>
#include <string>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Overlay geometry ("points"). Width shrinks to fit content; height is dynamic.
constexpr float kMaxPanelWidth = 400.0f;   // hard cap so very long messages wrap
constexpr float kMinPanelWidth = 150.0f;
constexpr float kTextWrapWidth = 360.0f;   // wrap width for message text
constexpr float kPad           = 10.0f;    // outer padding
constexpr float kRowGap        = 4.0f;     // vertical gap between message rows
constexpr float kLineHeight    = 15.0f;    // flow-layout line height
constexpr float kWordGap       = 4.0f;     // horizontal gap between words/IDs

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
    std::string author;
    std::string text;
};

struct FetchResult {
    std::vector<ChatMessage> messages;
    std::string status;     // short human-readable state ("Connected", errors…)
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

struct Token {
    bool isId = false;
    std::string text;
};

static void appendWords(std::vector<Token>& out, std::string const& chunk) {
    std::string cur;
    for (char c : chunk) {
        if (c == ' ') {
            if (!cur.empty()) {
                out.push_back(Token{false, cur});
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        out.push_back(Token{false, cur});
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
                out.push_back(Token{true, stripped});
            }
            catch (std::exception const&) {
                out.push_back(Token{false, raw});
            }
        } else {
            out.push_back(Token{false, raw});
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

static arc::Future<Result<std::string, std::string>> resolveChatId(Config const& cfg) {
    auto url = fmt::format(
        "https://www.googleapis.com/youtube/v3/videos?part=liveStreamingDetails&id={}&key={}",
        cfg.videoId, cfg.apiKey);

    web::WebRequest req;
    auto res = co_await req.get(url);
    if (!res.ok())
        co_return Err(fmt::format("YouTube request failed (HTTP {})", res.code()));

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
    if (!details.contains("activeLiveChatId"))
        co_return Err("Live chat is not active for that stream.");

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
    if (!res.ok())
        co_return Err(fmt::format("Chat request failed (HTTP {})", res.code()));

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

    // Advance (or clear) the pagination cursor.
    if (root.contains("nextPageToken")) {
        if (auto tok = root["nextPageToken"].asString())
            pageToken = tok.unwrap();
    } else {
        pageToken.clear();
    }

    if (root.contains("items") && root["items"].isArray()) {
        auto items = root["items"].asArray();
        if (items) {
            for (auto const& it : items.unwrap()) {
                ChatMessage msg;
                if (it.contains("authorDetails") && it["authorDetails"].contains("displayName"))
                    msg.author = it["authorDetails"]["displayName"].asString().unwrapOr("Viewer");
                else
                    msg.author = "Viewer";
                if (it.contains("snippet") && it["snippet"].contains("displayMessage"))
                    msg.text = it["snippet"]["displayMessage"].asString().unwrapOr("");
                if (msg.text.empty())
                    continue;
                out.messages.push_back(std::move(msg));
            }
        }
    }

    out.status = out.messages.empty() ? "Connected - waiting for messages..." : "Connected";
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
    void addMessages(std::vector<ChatMessage> const& messages);
    CCNode* buildMessageRow(ChatMessage const& message);
    CCMenuItemSpriteExtra* buildIdItem(int levelID);
    void setStatus(std::string const& status);

    // Callbacks ------------------------------------------------------------
    void onLevelIDClicked(CCObject* sender);
    void openLevel(int levelID);
    void handleLevelsLoaded(cocos2d::CCArray* levels);
    void restoreLevelManagerDelegate();

    // Members ---------------------------------------------------------------
    CCLayerColor* m_bg      = nullptr;
    CCLabelBMFont* m_title  = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCNode* m_content       = nullptr;

    std::vector<CCNode*> m_rows;   // newest at front

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
    m_content->setAnchorPoint(ccp(0.0f, 1.0f));
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

            co_await async::waitForMainThread<void>([this, f = std::move(fetch)]() mutable {
                this->setStatus(f.status);
                if (!f.messages.empty())
                    this->addMessages(f.messages);
            });

            delay = cfg->pollInterval;
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

void LiveyChatOverlay::addMessages(std::vector<ChatMessage> const& messages) {
    int64_t maxRaw = Mod::get()->getSettingValue<int64_t>("max-messages");
    int maxMsgs = static_cast<int>(std::clamp<int64_t>(maxRaw, 1, 20));

    for (auto const& message : messages) {
        auto row = this->buildMessageRow(message);
        m_content->addChild(row);
        m_rows.insert(m_rows.begin(), row);
    }

    while (static_cast<int>(m_rows.size()) > maxMsgs) {
        auto* last = m_rows.back();
        m_rows.pop_back();
        last->removeFromParentAndCleanup(true);
    }

    this->applyPosition();
}

CCNode* LiveyChatOverlay::buildMessageRow(ChatMessage const& message) {
    auto* row = CCNode::create();
    row->setAnchorPoint(ccp(0.0f, 1.0f));

    // Author line.
    auto* author = CCLabelBMFont::create((message.author + ":").c_str(), "chatFont.fnt");
    author->setScale(0.45f);
    author->setAnchorPoint(ccp(0.0f, 1.0f));
    author->setColor(ccc3(150, 150, 150));
    row->addChild(author);

    float maxExtent = author->getContentSize().width * author->getScaleX();
    float y = -(author->getContentSize().height * author->getScaleY() + 2.0f);

    auto* menu = CCMenu::create();
    menu->setAnchorPoint(ccp(0.0f, 1.0f));
    menu->setPosition(ccp(0.0f, 0.0f));
    row->addChild(menu);

    // Flow layout: white words + green clickable level IDs, with word wrap.
    float x = 0.0f;
    for (auto const& tok : tokenizeMessage(message.text)) {
        if (tok.isId) {
            auto* item = this->buildIdItem(std::stoi(tok.text));
            float w = item->getContentSize().width;
            if (x > 0.0f && x + w > kTextWrapWidth) {
                x = 0.0f;
                y -= kLineHeight;
            }
            item->setPosition(ccp(x + w / 2.0f, y - kLineHeight / 2.0f));
            menu->addChild(item);
            maxExtent = std::max(maxExtent, x + w);
            x += w + kWordGap;
        } else {
            auto* lbl = CCLabelBMFont::create(tok.text.c_str(), "chatFont.fnt");
            lbl->setScale(0.5f);
            lbl->setAnchorPoint(ccp(0.0f, 1.0f));
            lbl->setColor(ccc3(255, 255, 255));
            float w = lbl->getContentSize().width * lbl->getScaleX();
            if (x > 0.0f && x + w > kTextWrapWidth) {
                x = 0.0f;
                y -= kLineHeight;
            }
            lbl->setPosition(ccp(x, y));
            row->addChild(lbl);
            maxExtent = std::max(maxExtent, x + w);
            x += w + kWordGap;
        }
    }

    float rowH = (-y) + kLineHeight;
    row->setContentSize(CCSize(std::min(maxExtent, kTextWrapWidth), rowH));
    return row;
}

CCMenuItemSpriteExtra* LiveyChatOverlay::buildIdItem(int levelID) {
    auto* label = CCLabelBMFont::create(std::to_string(levelID).c_str(), "chatFont.fnt");
    label->setScale(0.5f);
    label->setColor(ccc3(90, 230, 90));   // green => clickable

    float w = label->getContentSize().width * label->getScaleX();
    float h = label->getContentSize().height * label->getScaleY();

    auto* item = CCMenuItemSpriteExtra::create(
        label, this, menu_selector(LiveyChatOverlay::onLevelIDClicked));
    item->setTag(levelID);
    item->setContentSize(CCSize(w, h));
    return item;
}

void LiveyChatOverlay::relayout() {
    // Stack rows top-down and measure the widest one.
    float y = 0.0f;
    float maxRowW = 0.0f;
    for (auto* row : m_rows) {
        row->setPosition(ccp(kPad, y));
        y -= row->getContentSize().height + kRowGap;
        maxRowW = std::max(maxRowW, row->getContentSize().width);
    }
    float contentH = m_rows.empty() ? 0.0f : (-y - kRowGap);

    bool titleVisible  = m_title  && m_title->isVisible();
    bool statusVisible = m_status && m_status->isVisible();

    float titleH  = titleVisible  ? m_title->getContentSize().height  * m_title->getScaleY()  : 0.0f;
    float titleW  = titleVisible  ? m_title->getContentSize().width   * m_title->getScaleX()  : 0.0f;
    float statusH = statusVisible ? m_status->getContentSize().height * m_status->getScaleY() : 0.0f;
    float statusW = statusVisible ? m_status->getContentSize().width  * m_status->getScaleX() : 0.0f;

    // Shrink the panel to fit its content (with a max cap for wrapping).
    float panelW = std::clamp(
        std::max({ maxRowW, titleW, statusW }) + 2.0f * kPad,
        kMinPanelWidth, kMaxPanelWidth);

    // Header height (only counts visible elements).
    float headerH = 0.0f;
    if (titleVisible)  headerH += titleH  + 2.0f;
    if (statusVisible) headerH += statusH + 4.0f;

    float totalH = kPad + headerH + contentH + kPad;

    this->setContentSize(CCSize(panelW, totalH));
    if (m_bg)
        m_bg->setContentSize(CCSize(panelW, totalH));

    // Position header elements from the top down.
    float top = totalH - kPad;
    if (titleVisible) {
        m_title->setPosition(ccp(kPad, top));
        top -= titleH + 2.0f;
    }
    if (statusVisible) {
        m_status->setPosition(ccp(kPad, top));
        top -= statusH + 4.0f;
    }
    if (m_content)
        m_content->setPosition(ccp(0.0f, top));
}

void LiveyChatOverlay::setStatus(std::string const& status) {
    if (m_status)
        m_status->setString(status.c_str());
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