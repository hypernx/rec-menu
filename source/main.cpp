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
        auto frame = new tsl::elm::OverlayFrame("Rec Menu", "v0.2.0");
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

        auto quality = new tsl::elm::NamedStepTrackBar("\uE132",
