#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include "recorder.hpp"

class RecGui : public tsl::Gui {
private:
    tsl::elm::ListItem *m_recItem = nullptr;
    tsl::elm::ListItem *m_timeItem = nullptr;
    tsl::elm::ListItem *m_shotItem = nullptr;

public:
    RecGui() {}

    virtual tsl::elm::Element *createUI() override {
        auto frame = new tsl::elm::OverlayFrame("Rec Menu", "v0.1.0");
        auto list = new tsl::elm::List();

        // ---- Recording ----
        list->addItem(new tsl::elm::CategoryHeader("Recording"));

        m_recItem = new tsl::elm::ListItem("Record");
        m_recItem->setValue(rec::isRecording() ? "ON" : "OFF");
        m_recItem->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                if (rec::isRecording()) rec::stop();
                else rec::start();
                m_recItem->setValue(rec::isRecording() ? "ON" : "OFF");
                return true;
            }
            return false;
        });
        list->addItem(m_recItem);

        m_timeItem = new tsl::elm::ListItem("Elapsed");
        m_timeItem->setValue("--:--");
        list->addItem(m_timeItem);

        // ---- Settings ----
        list->addItem(new tsl::elm::CategoryHeader("Settings"));

        auto quality = new tsl::elm::NamedStepTrackBar("\uE132", {"Low", "Mid", "High"});
        quality->setProgress(static_cast<u8>(rec::state().quality));
        quality->setValueChangedListener([](u8 v) {
            rec::state().quality = static_cast<rec::Quality>(v);
        });
        list->addItem(quality);

        auto timerToggle = new tsl::elm::ToggleListItem("Show timer", rec::state().showTimer);
        timerToggle->setStateChangedListener([](bool on) {
            rec::state().showTimer = on;
        });
        list->addItem(timerToggle);

        // ---- Capture ----
        list->addItem(new tsl::elm::CategoryHeader("Capture"));

        m_shotItem = new tsl::elm::ListItem("Screenshot");
        m_shotItem->setValue(std::to_string(rec::state().screenshotCount));
        m_shotItem->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                rec::screenshot();
                m_shotItem->setValue(std::to_string(rec::state().screenshotCount));
                return true;
            }
            return false;
        });
        list->addItem(m_shotItem);

        frame->setContent(list);
        return frame;
    }

    // 毎フレーム呼ばれる。経過時間の表示を更新する。
    virtual void update() override {
        if (m_timeItem) {
            if (rec::isRecording() && rec::state().showTimer)
                m_timeItem->setValue(rec::elapsedText());
            else
                m_timeItem->setValue("--:--");
        }
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                             HidAnalogStickState joyStickPosLeft,
                             HidAnalogStickState joyStickPosRight) override {
        return false;
    }
};

class RecOverlay : public tsl::Overlay {
public:
    virtual void initServices() override {
        // TODO: 録画sysmoduleとのIPC接続や capssc の初期化をここで行う
    }

    virtual void exitServices() override {
        // TODO: 上記の後始末
    }

    virtual void onShow() override {}
    virtual void onHide() override {}

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<RecGui>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<RecOverlay>(argc, argv);
}
