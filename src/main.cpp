// FDC+ Tester - acts as an FDC+ Serial Drive Server so an FDC+ Enhanced
// Floppy Disk Controller can be checked, and finds the FDC+'s speed when it
// doesn't match.

#include "monitor.h"
#include "transport.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Preferences.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int WIN_W = 640;
constexpr int MARGIN = 8;
constexpr int BIG_BUTTON_H = 40;
constexpr int LINK_PANEL_Y = 84;
constexpr int LINK_PANEL_H = 196;
constexpr int DRIVE_PANEL_Y = LINK_PANEL_Y + LINK_PANEL_H + MARGIN;
constexpr int DRIVE_PANEL_H = 112;
constexpr int LOG_PANEL_Y = DRIVE_PANEL_Y + DRIVE_PANEL_H + MARGIN;
constexpr int LOG_PANEL_H = 220;
constexpr int WIN_H = LOG_PANEL_Y + LOG_PANEL_H + 32;
constexpr int MAX_LOG_LINES = 200;
constexpr double REFRESH_SECONDS = 0.05;
constexpr int SPEED_460K = 1;   // index of 460.8K in BAUD_RATES

// Approximation of the MITS Altair 8800 case blue.
const Fl_Color ALTAIR_BLUE = fl_rgb_color(0x4A, 0x7A, 0xB0);
const Fl_Color LED_OFF = fl_rgb_color(128, 128, 128);
const Fl_Color LED_GREEN = fl_rgb_color(0, 255, 0);
const Fl_Color LED_RED = fl_rgb_color(255, 60, 40);
const Fl_Color TEXT_GOOD = fl_rgb_color(140, 255, 140);
const Fl_Color TEXT_BAD = fl_rgb_color(255, 225, 90);
const Fl_Color BUTTON_GO = fl_rgb_color(60, 170, 60);

// Round indicator lamp.
class Led : public Fl_Widget {
public:
    Led(int x, int y, int d) : Fl_Widget(x, y, d, d) {}

    void set(Fl_Color c)
    {
        if (c != color()) {
            color(c);
            redraw();
        }
    }

protected:
    void draw() override
    {
        fl_color(color());
        fl_pie(x(), y(), w(), h(), 0, 360);
        fl_color(FL_DARK3);
        fl_arc(x(), y(), w(), h(), 0, 360);
    }
};

Fl_Box* whiteLabel(int x, int y, int w, int h, const char* text, int size = 14)
{
    auto* b = new Fl_Box(x, y, w, h, text);
    b->labelcolor(FL_WHITE);
    b->labelsize(size);
    b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    return b;
}

Fl_Box* panelFrame(int y, int h, const char* title)
{
    auto* frame = new Fl_Box(FL_ENGRAVED_FRAME, MARGIN, y, WIN_W - 2 * MARGIN, h, nullptr);
    frame->color(ALTAIR_BLUE);
    auto* t = whiteLabel(MARGIN + 10, y + 6, WIN_W - 40, 24, title, 16);
    t->labelfont(FL_HELVETICA_BOLD);
    return t;
}

// Set a widget's label only if it changed, to avoid needless redraws.
void setLabel(Fl_Widget* w, const std::string& text)
{
    if (!w->label() || text != w->label())
        w->copy_label(text.c_str());
}

void setLabelColor(Fl_Widget* w, Fl_Color c)
{
    if (w->labelcolor() != c) {
        w->labelcolor(c);
        w->redraw_label();
    }
}

void setVisible(Fl_Widget* w, bool on)
{
    if (on && !w->visible())
        w->show();
    else if (!on && w->visible())
        w->hide();
}

// Escape characters Fl_Menu_::add() treats specially (e.g. '/' makes submenus).
std::string menuEscape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '/' || c == '\\' || c == '&' || c == '_')
            out += '\\';
        out += c;
    }
    return out;
}

// A counter: small caption above a large number.
struct Counter {
    Fl_Box* value = nullptr;

    Counter(int x, int y, int w, const char* caption)
    {
        whiteLabel(x, y, w, 18, caption, 12);
        value = whiteLabel(x, y + 18, w, 26, "0", 22);
        value->labelfont(FL_HELVETICA_BOLD);
    }

    void set(const std::string& text) { setLabel(value, text); }
};

std::string speedName(int index)
{
    return index >= 0 && index < NUM_BAUD_RATES ? BAUD_RATES[index].label : "?";
}

class App {
public:
    App();
    int run(int argc, char** argv);

private:
    void buildWindow();
    void buildLinkPanel();
    void buildDrivePanel();
    void buildLogPanel();
    void loadPreferences();
    void savePreferences();

    void scanSerialPorts();
    void connect(bool reportErrors);
    void refresh();
    void refreshLink(const MonitorView& v);
    void refreshDrive(const MonitorView& v);
    void refreshLog(const MonitorView& v);
    std::string diagnosis(const MonitorView& v, Fl_Color& color, int& useSpeed) const;
    void close();

    static void onConnectionChange(Fl_Widget*, void* app) { static_cast<App*>(app)->connect(true); }
    static void onRescan(Fl_Widget*, void* app);
    static void onFindSpeed(Fl_Widget*, void* app);
    static void onUseSpeed(Fl_Widget*, void* app);
    static void onClear(Fl_Widget*, void* app);
    static void onAbout(Fl_Widget*, void*);
    static void onClose(Fl_Widget*, void* app) { static_cast<App*>(app)->close(); }
    static void onTimer(void* app);

    Monitor monitor_;
    Fl_Preferences prefs_;

    Fl_Double_Window* window_ = nullptr;
    Fl_Choice* serialPort_ = nullptr;
    Fl_Choice* baud_ = nullptr;
    Led* rxLed_ = nullptr;
    Fl_Box* status_ = nullptr;

    Led* linkLed_ = nullptr;
    Fl_Box* message_ = nullptr;
    Fl_Button* useSpeed_ = nullptr;
    Fl_Button* findSpeed_ = nullptr;
    Counter* rate_ = nullptr;
    Counter* good_ = nullptr;
    Counter* damaged_ = nullptr;
    Counter* garbled_ = nullptr;
    Counter* bytes_ = nullptr;
    int useSpeedIndex_ = -1;

    Led* driveLeds_[NUM_DRIVES] = {};
    Led* headLed_ = nullptr;
    Fl_Box* track_ = nullptr;
    Fl_Box* driveText_ = nullptr;

    Fl_Browser* log_ = nullptr;
    unsigned long logShown_ = 0;

    std::vector<std::string> portNames_;
    std::string serialName_;
    std::string errorStatus_;
    int winX_ = -1, winY_ = -1;
};

App::App() : prefs_(Fl_Preferences::USER_L, "deltecent.com", "fdcplustest") {}

int App::run(int argc, char** argv)
{
    Fl::scheme("gtk+");
    FL_NORMAL_SIZE = 14;
    buildWindow();
    loadPreferences();
    scanSerialPorts();

    if (winX_ >= 0 && winY_ >= 0) {
        int sx, sy, sw, sh;
        Fl::screen_work_area(sx, sy, sw, sh, winX_, winY_);
        if (winX_ >= sx && winY_ >= sy && winX_ < sx + sw - 50 && winY_ < sy + sh - 50)
            window_->position(winX_, winY_);
    }
    window_->show(argc, argv);

    connect(false);
    refresh();
    Fl::add_timeout(REFRESH_SECONDS, onTimer, this);
    return Fl::run();
}

void App::buildWindow()
{
    window_ = new Fl_Double_Window(WIN_W, WIN_H, "FDC+ Tester");
    window_->color(ALTAIR_BLUE);
    window_->callback(onClose, this);

    static const std::string title = std::string("FDC+ Tester v") + FDCPLUSTEST_VERSION;
    whiteLabel(MARGIN + 2, 8, 300, 24, title.c_str(), 18)->labelfont(FL_HELVETICA_BOLD);
    auto* about = new Fl_Button(WIN_W - 200, 8, 70, 26, "About");
    about->callback(onAbout, this);

    whiteLabel(WIN_W - 112, 8, 70, 26, "Receive");
    rxLed_ = new Led(WIN_W - 34, 13, 16);
    rxLed_->color(LED_OFF);

    whiteLabel(MARGIN + 2, 44, 80, 30, "Serial port");
    serialPort_ = new Fl_Choice(92, 44, 250, 30);
    serialPort_->tooltip("The serial port the FDC+ is connected to");
    serialPort_->callback(onConnectionChange, this);

    whiteLabel(354, 44, 50, 30, "Speed");
    baud_ = new Fl_Choice(404, 44, 100, 30);
    baud_->tooltip("The speed (baud rate) you expect the FDC+ to use");
    for (int i = 0; i < NUM_BAUD_RATES; ++i)
        baud_->add(BAUD_RATES[i].label);
    baud_->value(0);
    baud_->callback(onConnectionChange, this);

    auto* rescan = new Fl_Button(WIN_W - MARGIN - 112, 44, 112, 30, "Find Ports");
    rescan->tooltip("Look again for serial ports, e.g. after plugging in a USB adapter");
    rescan->callback(onRescan, this);

    buildLinkPanel();
    buildDrivePanel();
    buildLogPanel();

    status_ = whiteLabel(MARGIN + 2, WIN_H - 26, WIN_W - 220, 22, "", 13);
    whiteLabel(WIN_W - 210, WIN_H - 26, 200, 22, "\xC2\xA9" "2026 Deltec Enterprises LLC", 12)
        ->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

    window_->end();
}

void App::buildLinkPanel()
{
    const int y = LINK_PANEL_Y;
    const int left = MARGIN + 12;
    panelFrame(y, LINK_PANEL_H, "Is the FDC+ talking?");

    linkLed_ = new Led(left, y + 40, 28);
    linkLed_->color(LED_OFF);

    const int buttonW = 130;
    const int msgX = left + 40;
    const int msgW = WIN_W - msgX - left - buttonW - 10;
    message_ = whiteLabel(msgX, y + 32, msgW, 92, "", 15);
    message_->labelfont(FL_HELVETICA_BOLD);
    message_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

    const int bx = WIN_W - left - buttonW;
    useSpeed_ = new Fl_Button(bx, y + 34, buttonW, BIG_BUTTON_H, "Use");
    useSpeed_->labelsize(16);
    useSpeed_->labelfont(FL_HELVETICA_BOLD);
    useSpeed_->labelcolor(FL_WHITE);
    useSpeed_->color(BUTTON_GO);
    useSpeed_->tooltip("Switch to the speed the FDC+ is using");
    useSpeed_->callback(onUseSpeed, this);
    useSpeed_->hide();

    findSpeed_ = new Fl_Button(bx, y + 82, buttonW, 34, "Find Speed");
    findSpeed_->tooltip("Try every speed to find the one the FDC+ is using");
    findSpeed_->callback(onFindSpeed, this);

    const int cy = y + 132;
    const int cw = (WIN_W - 2 * left) / 5;
    rate_ = new Counter(left, cy, cw, "Messages/sec");
    good_ = new Counter(left + cw, cy, cw, "Good");
    damaged_ = new Counter(left + 2 * cw, cy, cw, "Damaged");
    garbled_ = new Counter(left + 3 * cw, cy, cw, "Garbled bytes");
    bytes_ = new Counter(left + 4 * cw, cy, cw, "Bytes received");
}

void App::buildDrivePanel()
{
    const int y = DRIVE_PANEL_Y;
    const int left = MARGIN + 12;
    panelFrame(y, DRIVE_PANEL_H, "What the FDC+ reports");

    const int ly = y + 36;
    whiteLabel(left, ly, 110, 20, "Selected drive:");
    for (int i = 0; i < NUM_DRIVES; ++i) {
        int x = left + 112 + i * 30;
        driveLeds_[i] = new Led(x + 4, ly + 2, 16);
        driveLeds_[i]->color(LED_OFF);
        driveLeds_[i]->tooltip("Lit when the Altair has selected this drive");
        auto* n = whiteLabel(x, ly + 20, 24, 16, nullptr, 11);
        n->copy_label(std::to_string(i).c_str());
        n->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    }

    const int ry = y + 78;
    whiteLabel(left, ry, 100, 24, "Head loaded:");
    headLed_ = new Led(left + 104, ry + 4, 16);
    headLed_->color(LED_OFF);
    whiteLabel(left + 150, ry, 50, 24, "Track:");
    track_ = whiteLabel(left + 200, ry, 60, 24, "-", 16);
    track_->labelfont(FL_HELVETICA_BOLD);
    driveText_ = whiteLabel(left + 270, ry, WIN_W - 2 * left - 270, 24, "", 13);
}

void App::buildLogPanel()
{
    const int y = LOG_PANEL_Y;
    const int left = MARGIN + 12;
    panelFrame(y, LOG_PANEL_H, "What happened");

    auto* clear = new Fl_Button(WIN_W - left - 80, y + 6, 80, 26, "Clear");
    clear->tooltip("Set the counts back to zero and empty the list");
    clear->callback(onClear, this);

    log_ = new Fl_Browser(left, y + 38, WIN_W - 2 * left, LOG_PANEL_H - 38 - 10);
    log_->textfont(FL_COURIER);
    log_->textsize(13);
    log_->color(fl_rgb_color(250, 250, 250));
}

void App::loadPreferences()
{
    char buf[1024];
    int value;

    prefs_.get("SerialPort", buf, "", sizeof(buf));
    serialName_ = buf;

    prefs_.get("BaudIndex", value, 0);
    baud_->value(value >= 0 && value < NUM_BAUD_RATES ? value : 0);

    prefs_.get("WindowX", winX_, -1);
    prefs_.get("WindowY", winY_, -1);
}

void App::savePreferences()
{
    if (!serialName_.empty())
        prefs_.set("SerialPort", serialName_.c_str());
    prefs_.set("BaudIndex", baud_->value());
    prefs_.set("WindowX", window_->x());
    prefs_.set("WindowY", window_->y());
    prefs_.flush();
}

void App::scanSerialPorts()
{
    portNames_ = listSerialPorts();
    serialPort_->clear();
    for (const auto& name : portNames_)
        serialPort_->add(menuEscape(name).c_str());

    serialPort_->value(-1);
    for (size_t i = 0; i < portNames_.size(); ++i)
        if (portNames_[i] == serialName_)
            serialPort_->value(static_cast<int>(i));
    serialPort_->redraw();
}

void App::connect(bool reportErrors)
{
    monitor_.stop();
    monitor_.clear();
    log_->clear();
    logShown_ = monitor_.view().logTotal;

    int idx = serialPort_->value();
    if (idx < 0 || idx >= static_cast<int>(portNames_.size())) {
        errorStatus_ = portNames_.empty() ? "No serial ports found. Plug in the cable, then click Find Ports."
                                          : "Choose the serial port the FDC+ is connected to.";
        return;
    }
    serialName_ = portNames_[idx];
    int speed = baud_->value() < 0 ? 0 : baud_->value();

    std::string name = serialName_;
    Monitor::Opener opener = [name](int index, std::string& error) {
        return openSerial(name, BAUD_RATES[index].rate, BAUD_RATES[index].label, error);
    };

    std::string error;
    auto transport = opener(speed, error);
    if (!transport) {
        errorStatus_ = error;
        if (reportErrors)
            fl_alert("%s\n\nIs another program (such as the drive server) using this port?\n"
                     "Some serial adapters also can't use every speed; try another speed.",
                     error.c_str());
        return;
    }

    errorStatus_.clear();
    monitor_.start(std::move(transport), speed, opener);
}

void App::onRescan(Fl_Widget*, void* app)
{
    auto* self = static_cast<App*>(app);
    self->scanSerialPorts();
    self->connect(false);
}

void App::onFindSpeed(Fl_Widget*, void* app)
{
    static_cast<App*>(app)->monitor_.findSpeed();
}

void App::onUseSpeed(Fl_Widget*, void* app)
{
    auto* self = static_cast<App*>(app);
    if (self->useSpeedIndex_ < 0)
        return;
    self->baud_->value(self->useSpeedIndex_);
    self->connect(true);
    self->refresh();
}

void App::onClear(Fl_Widget*, void* app)
{
    auto* self = static_cast<App*>(app);
    self->monitor_.clear();
    self->log_->clear();
    self->refresh();
}

void App::onAbout(Fl_Widget*, void*)
{
    fl_message_title("About FDC+ Tester");
    fl_message("FDC+ Tester\n"
               "Version %s\n"
               "\xC2\xA9 2026 Deltec Enterprises LLC\n"
               "\n"
               "Acts as an FDC+ Serial Drive Server to check\n"
               "that an FDC+ set to drive type 7 is talking,\n"
               "and finds its speed when it doesn't match.",
               FDCPLUSTEST_VERSION);
}

// Copy the monitor's state onto the widgets.
void App::refresh()
{
    MonitorView v = monitor_.view();

    rxLed_->set(v.receiving ? LED_GREEN : LED_OFF);

    std::string status = v.running ? v.status : errorStatus_;
    if (status.empty())
        status = v.status;
    if (!v.running && status.empty())
        status = "Not connected";
    setLabel(status_, status);

    refreshLink(v);
    refreshDrive(v);
    refreshLog(v);
}

// The words for the current state. Sets useSpeed to a speed index to offer.
std::string App::diagnosis(const MonitorView& v, Fl_Color& color, int& useSpeed) const
{
    color = TEXT_BAD;
    useSpeed = -1;
    const std::string chosen = speedName(v.baudIndex);
    char pct[16];

    switch (v.diagnosis) {
    case Diagnosis::Stopped:
        color = FL_WHITE;
        return errorStatus_.empty() ? "Choose the serial port and speed above." : errorStatus_;

    case Diagnosis::Listening:
        color = FL_WHITE;
        return "Listening to the FDC+...";

    case Diagnosis::Scanning:
        color = FL_WHITE;
        return "The speeds may not match. Trying " + speedName(v.scanIndex) + "...";

    case Diagnosis::Silent:
        return "Nothing from the FDC+. Check that the Altair is on, the FDC+ drive type is 7 "
               "(serial drive), the cable is plugged in at both ends, and the serial port is right.";

    case Diagnosis::Mismatch: {
        const ScanResult& s = v.scan;
        if (!s.done)
            return "Bytes are arriving, but they make no sense. The speeds probably don't match.";
        if (s.found >= 0 && s.found != v.baudIndex) {
            useSpeed = s.found;
            return "Speed mismatch! The FDC+ is set to " + speedName(s.found) + ", but you chose "
                + chosen + ". Click Use " + speedName(s.found) + ", or set the FDC+ to " + chosen + ".";
        }
        if (s.found >= 0)
            return "Garbled data at " + chosen + ", although the FDC+ was heard at this speed a "
                   "moment ago. Check the cable.";
        std::string msg = "Data arrives, but makes no sense at any speed. Check the cable, and that "
                          "the FDC+ drive type is 7 (serial drive).";
        if (!s.unopenable.empty()) {
            msg += " This serial port can't use";
            for (size_t i = 0; i < s.unopenable.size(); ++i)
                msg += (i ? ", " : " ") + speedName(s.unopenable[i]);
            msg += ", so the FDC+ may be set to a speed this port can't use.";
        }
        return msg;
    }

    case Diagnosis::Marginal:
        std::snprintf(pct, sizeof(pct), "%d%%", v.damagedPercent);
        if (v.baudIndex == SPEED_460K)
            return std::string("The FDC+ is talking, but ") + pct + " of its messages are damaged. "
                   "460.8K is borderline for the FDC+: use 403.2K on both sides if this port can.";
        return std::string("The FDC+ is talking, but ") + pct + " of its messages are damaged. "
               "Check the cable, or try a slower speed on both sides.";

    case Diagnosis::NotHearing:
        return "The FDC+ is talking at " + chosen + ", but isn't hearing the answers: it asks only "
               "once a second, as it does when nothing comes back. Check the wire that carries "
               "data from this computer to the FDC+ (a broken or one-way cable).";

    case Diagnosis::Slow: {
        char rate[16];
        std::snprintf(rate, sizeof(rate), "%.1f", v.statPerSec);
        return std::string("The FDC+ is talking at ") + chosen + ", but sends only " + rate
            + " messages a second instead of about 10.";
    }

    case Diagnosis::Good:
        color = TEXT_GOOD;
        return "Yes! The FDC+ is working at " + chosen + ".";
    }
    return "";
}

void App::refreshLink(const MonitorView& v)
{
    Fl_Color color;
    int useSpeed;
    std::string message = diagnosis(v, color, useSpeed);
    setLabel(message_, message);
    setLabelColor(message_, color);

    Fl_Color led = LED_OFF;
    if (v.diagnosis == Diagnosis::Good)
        led = LED_GREEN;
    else if (v.diagnosis == Diagnosis::Silent || v.diagnosis == Diagnosis::Mismatch
             || v.diagnosis == Diagnosis::Marginal || v.diagnosis == Diagnosis::Slow
             || v.diagnosis == Diagnosis::NotHearing)
        led = LED_RED;
    linkLed_->set(led);

    useSpeedIndex_ = useSpeed;
    if (useSpeed >= 0)
        setLabel(useSpeed_, "Use " + speedName(useSpeed));
    setVisible(useSpeed_, useSpeed >= 0);

    if (v.running && v.diagnosis != Diagnosis::Scanning)
        findSpeed_->activate();
    else
        findSpeed_->deactivate();

    char rate[16];
    std::snprintf(rate, sizeof(rate), "%.1f", v.statPerSec);
    rate_->set(v.running && v.diagnosis != Diagnosis::Listening && v.diagnosis != Diagnosis::Scanning
                   ? rate : "-");
    good_->set(std::to_string(v.goodFrames));
    damaged_->set(std::to_string(v.badFrames));
    garbled_->set(std::to_string(v.garbageBytes));
    bytes_->set(std::to_string(v.bytes));
}

void App::refreshDrive(const MonitorView& v)
{
    // Only trust the drive status while the link is up.
    bool live = v.running && v.haveStat
        && (v.diagnosis == Diagnosis::Good || v.diagnosis == Diagnosis::Slow
            || v.diagnosis == Diagnosis::NotHearing || v.diagnosis == Diagnosis::Marginal || v.diagnosis == Diagnosis::Listening);

    for (int i = 0; i < NUM_DRIVES; ++i)
        driveLeds_[i]->set(live && v.drive == i ? LED_GREEN : LED_OFF);
    headLed_->set(live && v.headLoaded ? LED_GREEN : LED_OFF);
    setLabel(track_, live && v.drive != 0xff ? std::to_string(v.track) : "-");
    setLabel(driveText_, live ? (v.drive == 0xff ? "No drive selected. That's normal when idle." : "")
                              : "");
}

void App::refreshLog(const MonitorView& v)
{
    if (v.logTotal == logShown_)
        return;
    unsigned long fresh = v.logTotal - logShown_;
    size_t first = fresh >= v.log.size() ? 0 : v.log.size() - fresh;
    for (size_t i = first; i < v.log.size(); ++i)
        log_->add(v.log[i].c_str());
    while (log_->size() > MAX_LOG_LINES)
        log_->remove(1);
    log_->bottomline(log_->size());
    logShown_ = v.logTotal;
}

void App::onTimer(void* app)
{
    static_cast<App*>(app)->refresh();
    Fl::repeat_timeout(REFRESH_SECONDS, onTimer, app);
}

void App::close()
{
    // Escape closes FLTK windows by default; only close on a real close request.
    if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape)
        return;

    savePreferences();
    monitor_.stop();
    window_->hide();
}

} // namespace

int main(int argc, char** argv)
{
    App app;
    return app.run(argc, argv);
}
