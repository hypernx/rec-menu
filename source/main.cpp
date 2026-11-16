#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include "recorder.hpp"

class RecGui : public tsl::Gui {
private:
    tsl::elm::ListItem *m_recItem = nullptr;
    tsl::elm::ListItem *m_timeItem = nullptr;
    tsl::elm::ListItem *m_shotItem = nullptr;

public:
    RecGui() = default;

    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("Rec Menu", "v0.2.2");
        auto *list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Recording"));

        m_recItem = new tsl::elm::ListItem("Record (demo)");
        m_recItem->setValue(rec::isRecording() ? "ON" : "OFF");
        m_recItem->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A) || !m_recItem) return false;
            if (rec::isRecording()) rec::stop();
            else rec::start();
            m_recItem->setValue(rec::isRecording() ? "ON" : "OFF");
            return true;
        });
        list->addItem(m_recItem);

        m_timeItem = new tsl::elm::ListItem("Elapsed (demo)");
        m_timeItem->setValue("--:--");
        list->addItem(m_timeItem);

        list->addItem(new tsl::elm::CategoryHeader("Settings"));
        auto *timerToggle = new tsl::elm::ToggleListItem("Show timer", rec::state().showTimer);
        timerToggle->setStateChangedListener([](bool on) {
            rec::state().showTimer = on;
        });
        list->addItem(timerToggle);

        list->addItem(new tsl::elm::CategoryHeader("Capture"));
        m_shotItem = new tsl::elm::ListItem("Screenshot");
        m_shotItem->setValue(rec::shotStatus());
        m_shotItem->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A) || !m_shotItem) return false;
            rec::screenshot();
            m_shotItem->setValue(rec::shotStatus());
            return true;
        });
        list->addItem(m_shotItem);

        frame->setContent(list);
        return frame;
    }

    void update() override {
        if (!m_timeItem) return;
        if (rec::isRecording() && rec::state().showTimer) {
            char elapsed[16] = {};
            rec::elapsedText(elapsed, sizeof(elapsed));
            m_timeItem->setValue(elapsed);
        } else {
            m_timeItem->setValue("--:--");
        }
    }

    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                     HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override {
        (void)keysDown;
        (void)keysHeld;
        (void)touchPos;
        (void)joyStickPosLeft;
        (void)joyStickPosRight;
        return false;
    }
};

class RecOverlay : public tsl::Overlay {
public:
    // No additional services are needed just to open the menu.
    void initServices() override {}
    void exitServices() override {}
    void onShow() override {}
    void onHide() override {}

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<RecGui>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<RecOverlay>(argc, argv);
}
