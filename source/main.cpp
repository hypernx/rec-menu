#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include "recorder.hpp"
#include "lang.hpp"

// ---------------------------------------------------------------------------
// メニュー
//   録画中のタイマー(右上)と通知(左下)は osd.hpp が別レイヤーに描くので、
//   メニューを閉じたままでも表示され、ゲームは普通に操作できる。
// ---------------------------------------------------------------------------
class RecGui : public tsl::Gui {
private:
    tsl::elm::ListItem *m_recItem = nullptr;
    tsl::elm::ListItem *m_timeItem = nullptr;
    tsl::elm::ListItem *m_shotItem = nullptr;
    tsl::elm::ListItem *m_osdItem = nullptr;

public:
    RecGui() {}

    tsl::elm::Element *createUI() override {
        auto frame = new tsl::elm::OverlayFrame("Rec Menu", "v0.7.1");
        auto list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader(TR("Recording  (saved to /switch/rec-menu)", "録画（保存先: /switch/rec-menu）")));

        m_recItem = new tsl::elm::ListItem(TR("Record", "録画"));
        m_recItem->setValue(rec::recStatus());
        m_recItem->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            if (rec::isRecording()) {
                rec::stop();
            } else if (rec::start()) {
                tsl::Overlay::get()->hide();   // メニューを閉じてゲームに戻る(録画は続く)
            }
            return true;
        });
        list->addItem(m_recItem);

        m_timeItem = new tsl::elm::ListItem(TR("Elapsed", "経過時間"));
        m_timeItem->setValue("--:--");
        list->addItem(m_timeItem);

        list->addItem(new tsl::elm::CategoryHeader(TR("Settings", "設定")));

        auto timerToggle = new tsl::elm::ToggleListItem(TR("Show timer (top right)", "右上にタイマーを表示"), rec::state().showTimer);
        timerToggle->setStateChangedListener([](bool on) {
            rec::state().showTimer = on;
        });
        list->addItem(timerToggle);

        // タイマー/通知の表示が使えるか(うまく出ないときの確認用)
        m_osdItem = new tsl::elm::ListItem(TR("Notification display", "通知の表示"));
        m_osdItem->setValue(osd::g_status);
        list->addItem(m_osdItem);

        list->addItem(new tsl::elm::CategoryHeader(TR("Capture", "撮影")));

        m_shotItem = new tsl::elm::ListItem(TR("Screenshot", "スクリーンショット"));
        m_shotItem->setValue(rec::shotStatus());
        m_shotItem->setClickListener([](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            // メニューを閉じてから撮る(メニューが写らないように)。結果は左下に通知
            rec::requestScreenshot();
            tsl::Overlay::get()->hide();
            return true;
        });
        list->addItem(m_shotItem);

        frame->setContent(list);
        return frame;
    }

    void update() override {
        if (m_recItem)
            m_recItem->setValue(rec::isRecording() ? TR("REC (A: stop)", "録画中（Aで停止）") : rec::recStatus());
        if (m_timeItem)
            m_timeItem->setValue(rec::isRecording() ? rec::elapsedText() : "--:--");
        if (m_shotItem)
            m_shotItem->setValue(rec::shotStatus());
    }

    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                     HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override {
        // 録画中にBで閉じるとオーバーレイごと終了して録画が止まるので、隠すだけにする
        if ((keysDown & HidNpadButton_B) && rec::isRecording()) {
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
    void onHide() override {}

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        // Teslaの画面の準備ができたので、タイマー/通知の表示と監視スレッドを始める
        osd::init();
        rec::startMonitor();
        return initially<RecGui>();
    }
};

static void onBeforeExitScreen() {
    // Teslaが画面を片付ける前に、監視スレッドと自分のレイヤーを片付ける
    rec::stopMonitor();
    if (rec::isRecording()) rec::stop(rec::StopReason::Exit);
    osd::exit();
}

int main(int argc, char **argv) {
    // libtesla に追加したフック(ワークフローでパッチを当てている)
    tsl::impl::onHomeButton = rec::onHomeButton;
    tsl::impl::onBeforeExitScreen = onBeforeExitScreen;
    return tsl::loop<RecOverlay>(argc, argv);
}
