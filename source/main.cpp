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
        auto frame = new tsl::elm::OverlayFrame("Rec Menu", "v0.2.3");
        auto list = new tsl::elm::List();

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

        list->addItem(new tsl::elm::CategoryHeader("Settings"));

        auto timerToggle = new tsl::elm::ToggleListItem("Show timer", rec::state().showTimer);
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
        tsl::hlp::doWithSmSession([]() {
            rec::init();
        });
    }

    virtual void exitServices() override {
        rec::shutdown();
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
