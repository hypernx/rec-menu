#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include "recorder.hpp"

#include <atomic>

// Teslaのレイヤー(描画面)のID。libnx/libtesla側で定義されている
extern "C" u64 __nx_vi_layer_id;

// ---------------------------------------------------------------------------
// 録画中タイマー(HUD)
//   メニューを閉じたあと、画面右上に録画時間だけを表示する。
//   その間はゲームを普通に操作できる(入力はゲームに渡す)。
//   Ultrahandを開くボタン(通常 L + ↓ + 右スティック押し込み)でメニューに戻る。
// ---------------------------------------------------------------------------
namespace hud {

inline std::atomic<bool> active{false};
inline u64 savedCombo = 0;

// Teslaのレイヤーを横に動かす(通常は画面左端。HUD中は右端に寄せる)
inline void setLayerX(float x) {
    ViLayer layer{};
    layer.layer_id = __nx_vi_layer_id;
    layer.initialized = true;
    viSetLayerPosition(&layer, x, 0);
}

inline void enter() {
    if (active) return;
    savedCombo = tsl::cfg::launchCombo;
    // HUD中はTesla本体のボタン判定を止め、自分で判定する(勝手に消えないように)
    tsl::cfg::launchCombo = 0;
    setLayerX(float(tsl::cfg::ScreenWidth - tsl::cfg::LayerWidth));
    tsl::hlp::requestForeground(false);  // 入力をゲームに返す
    active = true;
}

// HUDをやめて通常の状態に戻す。HOMEボタン等でTeslaが隠れたときは別スレッドから呼ばれる
inline void leave(bool takeFocus) {
    if (!active) return;
    active = false;
    tsl::cfg::launchCombo = savedCombo;
    setLayerX(0);
    if (takeFocus) tsl::hlp::requestForeground(true);
}

} // namespace hud

class HudElement : public tsl::elm::Element {
public:
    void draw(tsl::gfx::Renderer *r) override {
        r->clearScreen();  // 残りは全部透明にしてゲーム画面を見せる

        const s32 w = 132, h = 36;
        const s32 x = tsl::cfg::FramebufferWidth - w - 8;
        const s32 y = 8;
        r->drawRect(x, y, w, h, tsl::Color(0x0, 0x0, 0x0, 0xA));   // 半透明の黒
        r->drawRect(x + 10, y + 12, 12, 12, tsl::Color(0xF, 0x2, 0x2, 0xF));  // 赤い●の代わり

        const std::string t = rec::elapsedText();
        r->drawString(("REC " + t).c_str(), false, x + 30, y + 26, 20, tsl::Color(0xF, 0xF, 0xF, 0xF));
    }
    void layout(u16, u16, u16, u16) override {}
};

class HudGui : public tsl::Gui {
public:
    HudGui() { hud::enter(); }

    tsl::elm::Element *createUI() override { return new HudElement(); }

    void update() override {
        rec::poll();
        // HOMEボタンなどでHUDが解除された / 録画が止まった → メニューに戻る
        if (!hud::active || !rec::isRecording()) {
            hud::leave(true);
            tsl::goBack();   // この後このオブジェクトは消えるので何も触らない
            return;
        }
        if (m_toMenu) {
            hud::leave(true);
            tsl::goBack();
            return;
        }
    }

    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &, HidAnalogStickState,
                     HidAnalogStickState) override {
        const u64 combo = hud::savedCombo;
        if (combo != 0 && (keysHeld & combo) == combo && (keysDown & combo))
            m_toMenu = true;
        return true;  // ゲーム用の入力なので、Tesla側では何もしない(Bで閉じたりしない)
    }

private:
    bool m_toMenu = false;
};

// ---------------------------------------------------------------------------
// メニュー
// ---------------------------------------------------------------------------
class RecGui : public tsl::Gui {
private:
    tsl::elm::ListItem *m_recItem = nullptr;
    tsl::elm::ListItem *m_timeItem = nullptr;
    tsl::elm::ListItem *m_shotItem = nullptr;

public:
    RecGui() {}

    tsl::elm::Element *createUI() override {
        auto frame = new tsl::elm::OverlayFrame("Rec Menu", "v0.4.1");
        auto list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Recording  (save: /switch/rec-menu)"));

        m_recItem = new tsl::elm::ListItem("Record");
        m_recItem->setValue(rec::recStatus());
        m_recItem->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            if (rec::isRecording()) {
                rec::stop();
            } else if (rec::start()) {
                if (rec::state().showTimer)
                    tsl::changeTo<HudGui>();          // 右上にタイマーを出してゲームに戻る
                else
                    tsl::Overlay::get()->hide();      // 何も出さずに閉じる(録画は続く)
            }
            return true;
        });
        list->addItem(m_recItem);

        m_timeItem = new tsl::elm::ListItem("Elapsed");
        m_timeItem->setValue("--:--");
        list->addItem(m_timeItem);

        list->addItem(new tsl::elm::CategoryHeader("Settings"));

        auto timerToggle = new tsl::elm::ToggleListItem("Show timer (top right)", rec::state().showTimer);
        timerToggle->setStateChangedListener([](bool on) {
            rec::state().showTimer = on;
        });
        list->addItem(timerToggle);

        list->addItem(new tsl::elm::CategoryHeader("Capture"));

        m_shotItem = new tsl::elm::ListItem("Screenshot");
        m_shotItem->setValue(rec::shotStatus());
        m_shotItem->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                rec::screenshot();
                m_shotItem->setValue(rec::shotStatus());
                return true;
            }
            return false;
        });
        list->addItem(m_shotItem);

        frame->setContent(list);
        return frame;
    }

    void update() override {
        rec::poll();
        if (m_recItem)
            m_recItem->setValue(rec::isRecording() ? "REC (A: stop)" : rec::recStatus());
        if (m_timeItem)
            m_timeItem->setValue(rec::isRecording() ? rec::elapsedText() : "--:--");
    }

    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                     HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override {
        // 録画中にBで閉じるとオーバーレイごと終了して録画が止まるので、隠すだけにする
        if ((keysDown & HidNpadButton_B) && rec::isRecording()) {
            if (rec::state().showTimer)
                tsl::changeTo<HudGui>();
            else
                tsl::Overlay::get()->hide();
            return true;
        }
        return false;
    }
};

class RecOverlay : public tsl::Overlay {
public:
    void initServices() override {
        rec::init();  // smセッション内で呼ばれる
    }

    void exitServices() override {
        rec::shutdown();  // オーバーレイ終了時は録画を止めてファイルを閉じる
    }

    void onShow() override {}

    void onHide() override {
        // HOME/電源ボタン等でTeslaが隠れたとき。HUD用の設定を元に戻す
        hud::leave(false);
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<RecGui>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<RecOverlay>(argc, argv);
}
