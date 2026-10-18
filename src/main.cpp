// Mobile FPS Bypass (Android32 + Android64)
//
// How it works (same idea as the PC IntegratedFPSBypass.dll, adapted for Android):
//   * PC: sets the cocos2d animation interval to 1/fps and turns V-Sync off.
//   * Android: CCDirector::setAnimationInterval() feeds the Java Cocos2dxRenderer
//     frame limiter (that is the real 60 FPS cap), and eglSwapInterval(0) is the
//     Android equivalent of "V-Sync off". Both are applied here, and re-applied
//     every frame if the game (or another mod) changes them back.
//
// UI: a draggable launcher bubble + a draggable panel. Both follow your finger
// with frame-rate independent smoothing. Tap the bubble to open/close the panel.

#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/OverlayManager.hpp>
#if __has_include(<Geode/ui/SceneManager.hpp>)
#include <Geode/ui/SceneManager.hpp>
#define FPSB_SCENEMANAGER 1
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#ifdef GEODE_IS_ANDROID
#include <dlfcn.h>
#endif

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Config + FPS logic
// ---------------------------------------------------------------------------
namespace fpsb {

    struct Config {
        bool enabled = false;
        double fps = 120.0;
        bool killVsync = true;
        bool hideInLevel = false;
        bool showButton = true;
        double btnX = 44.0;
        double btnY = 200.0;
        double panelX = -1.0;   // -1 = centre of screen
        double panelY = -1.0;
    };

    inline Config cfg;

    inline void sanitize() {
        cfg.fps = std::clamp(std::round(cfg.fps), 10.0, 1000.0);
    }

    // Settings (the gear in the mod list) are the single source of truth for
    // the main values; the panel writes to them and they are polled back.
    inline void pullSettings() {
        auto m = Mod::get();
        cfg.enabled     = m->getSettingValue<bool>("enabled");
        cfg.killVsync   = m->getSettingValue<bool>("disable-vsync");
        cfg.hideInLevel = m->getSettingValue<bool>("hide-in-level");
        cfg.showButton  = m->getSettingValue<bool>("show-button");
        cfg.fps         = static_cast<double>(m->getSettingValue<int64_t>("fps"));
        sanitize();
    }

    inline void pushSettings() {
        auto m = Mod::get();
        m->setSettingValue<bool>("enabled", cfg.enabled);
        m->setSettingValue<bool>("disable-vsync", cfg.killVsync);
        m->setSettingValue<bool>("hide-in-level", cfg.hideInLevel);
        m->setSettingValue<bool>("show-button", cfg.showButton);
        m->setSettingValue<int64_t>("fps", static_cast<int64_t>(cfg.fps));
    }

    inline void loadCfg() {
        auto m = Mod::get();
        cfg.btnX   = m->getSavedValue<double>("btnX", cfg.btnX);
        cfg.btnY   = m->getSavedValue<double>("btnY", cfg.btnY);
        cfg.panelX = m->getSavedValue<double>("panelX", cfg.panelX);
        cfg.panelY = m->getSavedValue<double>("panelY", cfg.panelY);
        pullSettings();
    }

    inline void saveCfg() {
        auto m = Mod::get();
        m->setSavedValue<double>("btnX", cfg.btnX);
        m->setSavedValue<double>("btnY", cfg.btnY);
        m->setSavedValue<double>("panelX", cfg.panelX);
        m->setSavedValue<double>("panelY", cfg.panelY);
        pushSettings();
    }

    inline double nowSec() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }

    inline double measuredFps = 0.0;
    inline double origInterval = 0.0;
    inline bool applied = false;
    inline bool vsyncOff = false;
    inline double lastVsyncApply = 0.0;

    // Android's "V-Sync off": eglSwapInterval(display, 0). Must run on the GL
    // thread (it does: this is called from a scheduled update inside the
    // cocos main loop), and only when a context is current.
    inline bool setSwapInterval(int interval) {
#ifdef GEODE_IS_ANDROID
        using GetFn = void* (*)();
        using SwapFn = unsigned int (*)(void*, int);
        static void* lib = dlopen("libEGL.so", RTLD_NOW);
        if (!lib) return false;
        static auto getCtx = reinterpret_cast<GetFn>(dlsym(lib, "eglGetCurrentContext"));
        static auto getDisp = reinterpret_cast<GetFn>(dlsym(lib, "eglGetCurrentDisplay"));
        static auto swapInt = reinterpret_cast<SwapFn>(dlsym(lib, "eglSwapInterval"));
        if (!getCtx || !getDisp || !swapInt) return false;
        if (!getCtx()) return false;
        return swapInt(getDisp(), interval) != 0;
#else
        (void)interval;
        return false;
#endif
    }

    // Called every frame. Cheap: only touches the director when the value differs.
    inline void enforce(double now, bool force = false) {
        auto dir = CCDirector::sharedDirector();
        if (!dir) return;

        if (cfg.enabled) {
            if (!applied) {
                double cur = dir->getAnimationInterval();
                origInterval = cur > 0.0 ? cur : 1.0 / 60.0;
                applied = true;
            }

            double want = 1.0 / cfg.fps;
            if (std::fabs(dir->getAnimationInterval() - want) > 1e-7) {
                dir->setAnimationInterval(want);
            }

            if (cfg.killVsync) {
                if (force || !vsyncOff || now - lastVsyncApply > 1.0) {
                    lastVsyncApply = now;
                    if (setSwapInterval(0)) vsyncOff = true;
                }
            } else if (vsyncOff) {
                setSwapInterval(1);
                vsyncOff = false;
            }
        } else if (applied) {
            dir->setAnimationInterval(origInterval > 0.0 ? origInterval : 1.0 / 60.0);
            if (vsyncOff) {
                setSwapInterval(1);
                vsyncOff = false;
            }
            applied = false;
        }
    }

    inline void changed() {
        sanitize();
        saveCfg();
        enforce(nowSec(), true);
    }

    void togglePanel();
    void ensureUI();
}

// ---------------------------------------------------------------------------
// Per-frame enforcer + FPS measurement (lives in the OverlayManager so it
// keeps running across every scene, including levels)
// ---------------------------------------------------------------------------
class FPSEnforcer : public CCNode {
protected:
    int m_frames = 0;
    double m_windowStart = 0.0;
    float m_poll = 0.f;

public:
    static FPSEnforcer* create() {
        auto ret = new FPSEnforcer();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init() override {
        if (!CCNode::init()) return false;
        this->setID("fps-enforcer"_spr);
        m_windowStart = fpsb::nowSec();
        this->scheduleUpdate();
        return true;
    }

    void update(float dt) override {
        double now = fpsb::nowSec();
        m_poll -= dt;
        if (m_poll <= 0.f) {
            m_poll = 0.25f;
            fpsb::pullSettings();
        }
        ++m_frames;
        double span = now - m_windowStart;
        if (span >= 0.5) {
            fpsb::measuredFps = m_frames / span;
            m_frames = 0;
            m_windowStart = now;
        }
        fpsb::enforce(now);
    }
};

// ---------------------------------------------------------------------------
// Draggable launcher bubble (tap = open/close panel, drag = move, smoothed)
// ---------------------------------------------------------------------------
class FPSLauncher : public CCNode, public CCTouchDelegate {
protected:
    static constexpr float kSize = 46.f;

    CCNode* m_body = nullptr;
    CCLabelBMFont* m_lbl = nullptr;
    bool m_registered = false;
    bool m_down = false;
    bool m_dragging = false;
    CCPoint m_startTouch = {0.f, 0.f};
    CCPoint m_grab = {0.f, 0.f};
    CCPoint m_target = {0.f, 0.f};
    float m_press = 1.f;
    double m_lblTimer = 0.0;

    CCPoint clampPos(CCPoint p) {
        auto ws = CCDirector::sharedDirector()->getWinSize();
        float h = kSize * 0.5f + 2.f;
        return {
            std::clamp(p.x, h, std::max(h, ws.width - h)),
            std::clamp(p.y, h, std::max(h, ws.height - h))
        };
    }

    void registerTouch() {
        if (m_registered) return;
        m_registered = true;
        CCDirector::sharedDirector()->getTouchDispatcher()
            ->addTargetedDelegate(this, -600, true);
    }

    void unregisterTouch() {
        if (!m_registered) return;
        m_registered = false;
        CCDirector::sharedDirector()->getTouchDispatcher()->removeDelegate(this);
    }

    bool hiddenNow() {
        if (!fpsb::cfg.hideInLevel) return false;
        auto pl = PlayLayer::get();
        return pl && !pl->m_isPaused;
    }

    void finish(bool allowTap) {
        if (!m_down) return;
        m_down = false;
        if (m_dragging) {
            m_dragging = false;
            fpsb::cfg.btnX = m_target.x;
            fpsb::cfg.btnY = m_target.y;
            fpsb::saveCfg();
        } else if (allowTap) {
            fpsb::togglePanel();
        }
    }

public:
    static FPSLauncher* create() {
        auto ret = new FPSLauncher();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init() override {
        if (!CCNode::init()) return false;
        this->setID("fps-launcher"_spr);
        this->setContentSize({kSize, kSize});
        this->ignoreAnchorPointForPosition(false);
        this->setAnchorPoint({0.5f, 0.5f});

        m_body = CCNode::create();
        m_body->setContentSize({kSize, kSize});
        m_body->ignoreAnchorPointForPosition(false);
        m_body->setAnchorPoint({0.5f, 0.5f});
        m_body->setPosition({kSize / 2.f, kSize / 2.f});
        this->addChild(m_body);

        auto bg = CCScale9Sprite::create("square02_001.png", {0.f, 0.f, 80.f, 80.f});
        bg->setContentSize({kSize, kSize});
        bg->setPosition({kSize / 2.f, kSize / 2.f});
        bg->setOpacity(215);
        m_body->addChild(bg);

        auto tag = CCLabelBMFont::create("FPS", "bigFont.fnt");
        tag->setScale(0.3f);
        tag->setColor({150, 150, 150});
        tag->setPosition({kSize / 2.f, kSize - 11.f});
        m_body->addChild(tag, 1);

        m_lbl = CCLabelBMFont::create("--", "bigFont.fnt");
        m_lbl->setScale(0.5f);
        m_lbl->setPosition({kSize / 2.f, 16.f});
        m_body->addChild(m_lbl, 1);

        auto ws = CCDirector::sharedDirector()->getWinSize();
        m_target = clampPos({
            static_cast<float>(fpsb::cfg.btnX),
            std::min(static_cast<float>(fpsb::cfg.btnY), ws.height - 30.f)
        });
        this->setPosition(m_target);
        this->scheduleUpdate();
        return true;
    }

    void onEnter() override {
        CCNode::onEnter();
        this->registerTouch();
    }

    void onExit() override {
        this->unregisterTouch();
        CCNode::onExit();
    }

    void update(float dt) override {
        dt = std::min(dt, 0.1f);
        if (!m_registered) this->registerTouch();

        this->setVisible((fpsb::cfg.showButton && !this->hiddenNow()) || m_down);

        // Smooth follow (frame-rate independent exponential smoothing).
        float k = 1.f - std::exp(-24.f * dt);
        auto p = this->getPosition();
        p = p + (m_target - p) * k;
        if (std::fabs(m_target.x - p.x) < 0.05f && std::fabs(m_target.y - p.y) < 0.05f) p = m_target;
        this->setPosition(p);

        // Press / lift feedback.
        float want = m_down ? (m_dragging ? 1.12f : 0.9f) : 1.f;
        m_press += (want - m_press) * (1.f - std::exp(-20.f * dt));
        m_body->setScale(m_press);

        m_lblTimer -= dt;
        if (m_lblTimer <= 0.0) {
            m_lblTimer = 0.4;
            if (fpsb::cfg.enabled && fpsb::measuredFps > 0.0) {
                m_lbl->setString(fmt::format("{}", static_cast<int>(std::round(fpsb::measuredFps))).c_str());
                m_lbl->setColor({120, 255, 140});
            } else {
                m_lbl->setString("OFF");
                m_lbl->setColor({190, 190, 190});
            }
        }
    }

    bool ccTouchBegan(CCTouch* t, CCEvent*) override {
        if (!this->isVisible() || m_down || !this->getParent()) return false;
        auto p = this->getParent()->convertTouchToNodeSpace(t);
        auto pos = this->getPosition();
        if (std::hypot(p.x - pos.x, p.y - pos.y) > kSize * 0.5f + 6.f) return false;

        m_down = true;
        m_dragging = false;
        m_startTouch = p;
        m_grab = pos - p;
        m_target = pos;
        return true;
    }

    void ccTouchMoved(CCTouch* t, CCEvent*) override {
        if (!m_down) return;
        auto p = this->getParent()->convertTouchToNodeSpace(t);
        if (!m_dragging) {
            if (std::hypot(p.x - m_startTouch.x, p.y - m_startTouch.y) < 10.f) return;
            m_dragging = true;
        }
        m_target = clampPos(p + m_grab);
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override { this->finish(true); }
    void ccTouchCancelled(CCTouch*, CCEvent*) override { this->finish(false); }
};

// ---------------------------------------------------------------------------
// Draggable panel
// ---------------------------------------------------------------------------
class FPSPanel : public CCNode, public CCTouchDelegate {
public:
    static constexpr float W = 330.f;
    static constexpr float H = 312.f;
    static constexpr float SX0 = 34.f;          // slider track start
    static constexpr float SX1 = W - 34.f;      // slider track end

protected:
    enum class Kind { Toggle, Button, Stepper };

    struct Control {
        Kind kind = Kind::Button;
        CCNode* vis = nullptr;
        CCRect hit = {0.f, 0.f, 0.f, 0.f};
        float baseScale = 1.f;
        std::function<void()> fire;
        std::function<bool()> state;    // toggles: on/off
        std::function<bool()> active;   // buttons: highlighted?
        CCSprite* on = nullptr;
        CCSprite* off = nullptr;
        CCLayerColor* body = nullptr;
        ccColor3B color = {255, 255, 255};
    };

    std::vector<Control> m_controls;
    int m_pressed = -1;
    bool m_pressedInside = false;
    bool m_dragging = false;
    bool m_registered = false;
    bool m_open = false;
    CCPoint m_grab = {0.f, 0.f};
    CCPoint m_target = {0.f, 0.f};
    float m_scale = 0.f;
    float m_scaleTarget = 0.f;
    float m_repeat = 0.f;
    int m_repeatCount = 0;
    double m_liveTimer = 0.0;
    CCLabelBMFont* m_value = nullptr;
    CCLabelBMFont* m_live = nullptr;
    CCLabelBMFont* m_note = nullptr;
    CCLayerColor* m_fill = nullptr;
    CCLayerColor* m_knob = nullptr;
    float m_sliderY = 0.f;
    bool m_sliding = false;
    double m_lastChange = 0.0;

    CCPoint clampPos(CCPoint p) {
        auto ws = CCDirector::sharedDirector()->getWinSize();
        float hw = W * 0.5f, hh = H * 0.5f;
        return {
            std::clamp(p.x, hw, std::max(hw, ws.width - hw)),
            std::clamp(p.y, hh, std::max(hh, ws.height - hh))
        };
    }

    void registerTouch() {
        if (m_registered) return;
        m_registered = true;
        CCDirector::sharedDirector()->getTouchDispatcher()
            ->addTargetedDelegate(this, -620, true);
    }

    void unregisterTouch() {
        if (!m_registered) return;
        m_registered = false;
        CCDirector::sharedDirector()->getTouchDispatcher()->removeDelegate(this);
    }

    // ---- building helpers -------------------------------------------------
    CCLabelBMFont* addLabel(char const* text, float x, float y, float scale, bool left) {
        auto l = CCLabelBMFont::create(text, "bigFont.fnt");
        l->setScale(scale);
        if (left) l->setAnchorPoint({0.f, 0.5f});
        l->setPosition({x, y});
        this->addChild(l, 2);
        return l;
    }

    void addToggleRow(char const* text, float y, std::function<bool()> get, std::function<void()> flip) {
        this->addLabel(text, 20.f, y, 0.42f, true);

        auto wrap = CCNode::create();
        wrap->setContentSize({30.f, 30.f});
        wrap->ignoreAnchorPointForPosition(false);
        wrap->setAnchorPoint({0.5f, 0.5f});
        wrap->setPosition({W - 38.f, y});

        auto on = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto off = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        on->setPosition({15.f, 15.f});
        off->setPosition({15.f, 15.f});
        on->setScale(0.75f);
        off->setScale(0.75f);
        wrap->addChild(on);
        wrap->addChild(off);
        this->addChild(wrap, 2);

        Control c;
        c.kind = Kind::Toggle;
        c.vis = wrap;
        c.hit = CCRect(10.f, y - 13.f, W - 20.f, 26.f);
        c.fire = std::move(flip);
        c.state = std::move(get);
        c.on = on;
        c.off = off;
        m_controls.push_back(std::move(c));
    }

    void addButton(
        char const* text, float cx, float cy, float w, float h,
        ccColor3B color, float textScale, Kind kind,
        std::function<void()> fire, std::function<bool()> active = nullptr
    ) {
        auto node = CCNode::create();
        node->setContentSize({w, h});
        node->ignoreAnchorPointForPosition(false);
        node->setAnchorPoint({0.5f, 0.5f});
        node->setPosition({cx, cy});

        auto body = CCLayerColor::create(ccc4(color.r, color.g, color.b, 255), w, h);
        body->setPosition({0.f, 0.f});
        node->addChild(body);

        auto lbl = CCLabelBMFont::create(text, "bigFont.fnt");
        lbl->setScale(textScale);
        lbl->setPosition({w / 2.f, h / 2.f});
        node->addChild(lbl, 1);

        this->addChild(node, 2);

        Control c;
        c.kind = kind;
        c.vis = node;
        c.hit = CCRect(cx - w / 2.f - 3.f, cy - h / 2.f - 3.f, w + 6.f, h + 6.f);
        c.fire = std::move(fire);
        c.active = std::move(active);
        c.body = body;
        c.color = color;
        m_controls.push_back(std::move(c));
    }

    void adjust(int dir) {
        int step = m_repeatCount < 6 ? 1 : (m_repeatCount < 20 ? 5 : (m_repeatCount < 45 ? 10 : 25));
        fpsb::cfg.fps = std::round(fpsb::cfg.fps) + dir * step;
        this->commit();
    }

    void setFps(double v) {
        fpsb::cfg.fps = v;
        this->commit();
    }

    // Slider uses a curve (finer control at low FPS so 10-30 are easy to hit).
    void setFromSlider(float localX) {
        float t = std::clamp((localX - SX0) / (SX1 - SX0), 0.f, 1.f);
        double v = std::round(10.0 + 990.0 * t * t);
        if (v != std::round(fpsb::cfg.fps)) {
            fpsb::cfg.fps = v;
            this->commit();
        }
    }

    void commit() {
        m_lastChange = fpsb::nowSec();
        fpsb::changed();
        this->refreshAll();
    }

    void refreshAll() {
        for (auto& c : m_controls) {
            if (c.on && c.state) {
                bool s = c.state();
                c.on->setVisible(s);
                c.off->setVisible(!s);
            }
            if (c.body && c.active) {
                auto col = c.active() ? ccc3(255, 175, 30) : c.color;
                c.body->setColor(col);
            }
        }
        m_value->setString(fmt::format("{}", static_cast<int>(std::round(fpsb::cfg.fps))).c_str());
        m_value->setColor(fpsb::cfg.enabled ? ccc3(120, 255, 140) : ccc3(170, 170, 170));

        // Slider visuals
        float t = std::sqrt(std::clamp((static_cast<float>(fpsb::cfg.fps) - 10.f) / 990.f, 0.f, 1.f));
        float x = SX0 + t * (SX1 - SX0);
        m_fill->setContentSize({std::max(t * (SX1 - SX0), 0.5f), 6.f});
        m_knob->setPosition({x - 6.f, m_sliderY - 12.f});

        // "Device limit" note
        bool settled = fpsb::nowSec() - m_lastChange > 1.5;
        double got = fpsb::measuredFps;
        if (settled && fpsb::cfg.enabled && got > 1.0 && got < fpsb::cfg.fps * 0.88) {
            int g = static_cast<int>(std::round(got));
            m_note->setString(
                fpsb::cfg.killVsync
                    ? fmt::format("Device limit reached (~{} FPS)", g).c_str()
                    : fmt::format("Capped at ~{} FPS - try turning V-Sync off", g).c_str()
            );
            m_note->setVisible(true);
        } else {
            m_note->setVisible(false);
        }
    }

    void build() {
        // Background
        auto bg = CCScale9Sprite::create("square02_001.png", {0.f, 0.f, 80.f, 80.f});
        bg->setContentSize({W, H});
        bg->setAnchorPoint({0.f, 0.f});
        bg->setPosition({0.f, 0.f});
        bg->setOpacity(235);
        this->addChild(bg, 0);

        // Title bar + divider
        this->addLabel("FPS Bypass", W / 2.f, H - 20.f, 0.58f, false);
        auto div = CCLayerColor::create(ccc4(255, 255, 255, 50), W - 30.f, 2.f);
        div->setPosition({15.f, H - 38.f});
        this->addChild(div, 1);

        auto hint = this->addLabel("drag empty space to move", W / 2.f, H - 47.f, 0.26f, false);
        hint->setColor({130, 130, 130});

        // Close
        this->addButton("X", W - 22.f, H - 20.f, 26.f, 26.f, {190, 55, 55}, 0.5f, Kind::Button,
            [this]() { this->setOpen(false); });

        // Enable
        this->addToggleRow("Enable FPS Bypass", H - 68.f,
            []() { return fpsb::cfg.enabled; },
            [this]() { fpsb::cfg.enabled = !fpsb::cfg.enabled; this->commit(); });

        // Big value + steppers
        float vy = H - 104.f;
        m_value = this->addLabel("120", W / 2.f, vy, 0.9f, false);
        this->addLabel("target FPS", W / 2.f, vy - 22.f, 0.28f, false)->setColor({150, 150, 150});

        this->addButton("-", W / 2.f - 92.f, vy, 54.f, 34.f, {55, 55, 62}, 0.8f, Kind::Stepper,
            [this]() { this->adjust(-1); });
        this->addButton("+", W / 2.f + 92.f, vy, 54.f, 34.f, {55, 55, 62}, 0.8f, Kind::Stepper,
            [this]() { this->adjust(+1); });

        // Slider (10 - 1000)
        m_sliderY = H - 146.f;
        auto track = CCLayerColor::create(ccc4(60, 60, 68, 255), SX1 - SX0, 6.f);
        track->setPosition({SX0, m_sliderY - 3.f});
        this->addChild(track, 1);
        m_fill = CCLayerColor::create(ccc4(120, 255, 140, 255), SX1 - SX0, 6.f);
        m_fill->setPosition({SX0, m_sliderY - 3.f});
        this->addChild(m_fill, 2);
        m_knob = CCLayerColor::create(ccc4(240, 240, 240, 255), 12.f, 24.f);
        m_knob->setPosition({SX0 - 6.f, m_sliderY - 12.f});
        this->addChild(m_knob, 3);

        // Presets (two rows)
        static const int presets[13] = {30, 60, 90, 120, 144, 240, 360, 480, 560, 640, 720, 890, 1000};
        for (int i = 0; i < 13; ++i) {
            int row = i < 7 ? 0 : 1;
            int idx = i < 7 ? i : i - 7;
            int n = i < 7 ? 7 : 6;
            float py = H - 176.f - row * 30.f;
            float cx = 37.f + (W - 74.f) * idx / static_cast<float>(n - 1);
            int v = presets[i];
            this->addButton(
                std::to_string(v).c_str(), cx, py, 46.f, 24.f, {55, 55, 62}, 0.32f, Kind::Button,
                [this, v]() { this->setFps(v); },
                [v]() { return static_cast<int>(std::round(fpsb::cfg.fps)) == v; }
            );
        }

        // Toggles
        this->addToggleRow("Disable V-Sync", H - 240.f,
            []() { return fpsb::cfg.killVsync; },
            [this]() { fpsb::cfg.killVsync = !fpsb::cfg.killVsync; this->commit(); });
        this->addToggleRow("Hide button in level", H - 266.f,
            []() { return fpsb::cfg.hideInLevel; },
            [this]() { fpsb::cfg.hideInLevel = !fpsb::cfg.hideInLevel; this->commit(); });

        // Device-limit note + live readout
        m_note = this->addLabel("", W / 2.f, 24.f, 0.3f, false);
        m_note->setColor({255, 170, 60});
        m_note->setVisible(false);
        m_live = this->addLabel("Measured: -- FPS", W / 2.f, 9.f, 0.32f, false);
        m_live->setColor({200, 200, 120});

        for (auto& c : m_controls) c.baseScale = c.vis->getScale();
        this->refreshAll();
    }

    void finish(bool fire) {
        if (m_pressed >= 0) {
            auto& c = m_controls[m_pressed];
            c.vis->setScale(c.baseScale);
            bool doFire = fire && m_pressedInside && c.kind != Kind::Stepper;
            auto cb = c.fire;
            m_pressed = -1;
            if (doFire && cb) cb();
        }
        m_sliding = false;
        if (m_dragging) {
            m_dragging = false;
            fpsb::cfg.panelX = m_target.x;
            fpsb::cfg.panelY = m_target.y;
            fpsb::saveCfg();
        }
    }

public:
    static FPSPanel* create() {
        auto ret = new FPSPanel();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init() override {
        if (!CCNode::init()) return false;
        this->setID("fps-panel"_spr);
        this->setContentSize({W, H});
        this->ignoreAnchorPointForPosition(false);
        this->setAnchorPoint({0.5f, 0.5f});

        this->build();

        auto ws = CCDirector::sharedDirector()->getWinSize();
        CCPoint start = {ws.width / 2.f, ws.height / 2.f};
        if (fpsb::cfg.panelX >= 0.0 && fpsb::cfg.panelY >= 0.0) {
            start = CCPoint(static_cast<float>(fpsb::cfg.panelX), static_cast<float>(fpsb::cfg.panelY));
        }
        m_target = this->clampPos(start);
        this->setPosition(m_target);
        this->setScale(0.001f);
        this->setVisible(false);
        this->scheduleUpdate();
        return true;
    }

    void setOpen(bool open) {
        if (open == m_open) return;
        m_open = open;
        if (open) {
            m_target = this->clampPos(m_target);
            this->setVisible(true);
            if (m_scale < 0.05f) m_scale = 0.05f;
            m_scaleTarget = 1.f;
            this->refreshAll();
        } else {
            this->finish(false);
            m_scaleTarget = 0.f;
        }
    }

    void toggle() { this->setOpen(!m_open); }

    void onEnter() override {
        CCNode::onEnter();
        this->registerTouch();
    }

    void onExit() override {
        this->unregisterTouch();
        CCNode::onExit();
    }

    void update(float dt) override {
        dt = std::min(dt, 0.1f);
        if (!m_registered) this->registerTouch();
        if (!this->isVisible()) return;

        // Open/close + drag "lift" animation.
        float st = m_scaleTarget * (m_dragging ? 1.03f : 1.f);
        m_scale += (st - m_scale) * (1.f - std::exp(-16.f * dt));
        if (!m_open && m_scale < 0.03f) {
            m_scale = 0.f;
            this->setVisible(false);
            return;
        }
        this->setScale(std::max(m_scale, 0.001f));

        // Smooth follow.
        float k = 1.f - std::exp(-24.f * dt);
        auto p = this->getPosition();
        p = p + (m_target - p) * k;
        if (std::fabs(m_target.x - p.x) < 0.05f && std::fabs(m_target.y - p.y) < 0.05f) p = m_target;
        this->setPosition(p);

        // Hold-to-repeat on the +/- buttons (accelerates).
        if (m_pressed >= 0 && m_pressedInside && m_controls[m_pressed].kind == Kind::Stepper) {
            m_repeat -= dt;
            if (m_repeat <= 0.f) {
                ++m_repeatCount;
                m_repeat = 0.05f;
                auto cb = m_controls[m_pressed].fire;
                if (cb) cb();
            }
        }

        m_liveTimer -= dt;
        if (m_liveTimer <= 0.0) {
            m_liveTimer = 0.25;
            this->refreshAll();
            m_live->setString(fmt::format(
                "Measured: {} FPS", static_cast<int>(std::round(fpsb::measuredFps))
            ).c_str());
        }
    }

    bool ccTouchBegan(CCTouch* t, CCEvent*) override {
        if (!m_open || !this->isVisible() || !this->getParent()) return false;
        auto local = this->convertTouchToNodeSpace(t);
        if (!CCRect(0.f, 0.f, W, H).containsPoint(local)) return false;

        if (CCRect(SX0 - 14.f, m_sliderY - 14.f, SX1 - SX0 + 28.f, 28.f).containsPoint(local)) {
            m_pressed = -1;
            m_sliding = true;
            this->setFromSlider(local.x);
            return true;
        }

        m_pressed = -1;
        for (size_t i = 0; i < m_controls.size(); ++i) {
            if (m_controls[i].hit.containsPoint(local)) {
                m_pressed = static_cast<int>(i);
                break;
            }
        }

        if (m_pressed >= 0) {
            auto& c = m_controls[m_pressed];
            m_pressedInside = true;
            c.vis->setScale(c.baseScale * 0.9f);
            if (c.kind == Kind::Stepper) {
                m_repeatCount = 0;
                m_repeat = 0.4f;
                auto cb = c.fire;
                if (cb) cb();
            }
            return true;
        }

        // Empty panel area: start dragging.
        auto p = this->getParent()->convertTouchToNodeSpace(t);
        m_dragging = true;
        m_grab = this->getPosition() - p;
        m_target = this->getPosition();
        return true;
    }

    void ccTouchMoved(CCTouch* t, CCEvent*) override {
        if (m_sliding) {
            this->setFromSlider(this->convertTouchToNodeSpace(t).x);
            return;
        }
        if (m_dragging) {
            auto p = this->getParent()->convertTouchToNodeSpace(t);
            m_target = this->clampPos(p + m_grab);
        } else if (m_pressed >= 0) {
            auto local = this->convertTouchToNodeSpace(t);
            auto& c = m_controls[m_pressed];
            bool inside = c.hit.containsPoint(local);
            if (inside != m_pressedInside) {
                m_pressedInside = inside;
                c.vis->setScale(inside ? c.baseScale * 0.9f : c.baseScale);
            }
        }
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override { this->finish(true); }
    void ccTouchCancelled(CCTouch*, CCEvent*) override { this->finish(false); }
};

// ---------------------------------------------------------------------------
// UI glue
// ---------------------------------------------------------------------------
namespace fpsb {
    inline CCNode* uiEnforcer = nullptr;
    inline CCNode* uiLauncher = nullptr;
    inline CCNode* uiPanel = nullptr;
    inline bool uiBuilt = false;

    // Keeps a node alive on top of every scene (menus AND levels).
    inline void attachPersistent(CCNode* n, int z) {
#ifdef FPSB_SCENEMANAGER
        SceneManager::get()->keepAcrossScenes(n);
        if (!n->getParent()) {
            if (auto sc = CCDirector::sharedDirector()->getRunningScene()) sc->addChild(n, z);
        }
#else
        if (auto ov = OverlayManager::get()) ov->addChild(n, z);
#endif
    }
}

void fpsb::togglePanel() {
    if (uiPanel) static_cast<FPSPanel*>(uiPanel)->toggle();
}

void fpsb::ensureUI() {
    if (uiBuilt) return;
    uiBuilt = true;
#ifdef FPSB_SCENEMANAGER
    log::info("FPS Bypass: building UI with SceneManager");
#else
    log::info("FPS Bypass: building UI with OverlayManager");
#endif
    uiEnforcer = FPSEnforcer::create();
    uiLauncher = FPSLauncher::create();
    uiPanel = FPSPanel::create();
    if (uiEnforcer) attachPersistent(uiEnforcer, 1);
    if (uiLauncher) attachPersistent(uiLauncher, 9000);
    if (uiPanel) attachPersistent(uiPanel, 9001);
    log::info("FPS Bypass: UI built");
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
class $modify(FPSBypassMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        fpsb::ensureUI();
        return true;
    }
};

$on_mod(Loaded) {
    fpsb::loadCfg();
    log::info("Mobile FPS Bypass loaded (enabled={}, fps={})", fpsb::cfg.enabled, fpsb::cfg.fps);
}
