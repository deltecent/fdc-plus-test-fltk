// Serial drive server side of the FDC+ protocol, used to check an FDC+.
// Listens to the STAT commands the FDC+ sends about ten times a second,
// answers them, and works out whether the link is good. When the bytes make
// no sense it tries every speed to find the one the FDC+ is using. Runs on
// its own thread; the GUI polls view() to update its display.
#pragma once

#include "transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

constexpr int NUM_DRIVES = 16;

struct BaudRate {
    int rate;
    const char* label;
};

// The FDC+ prefers 403.2K, then 460.8K, then 230.4K, so a speed scan tries
// them in this order.
extern const BaudRate BAUD_RATES[];
extern const int NUM_BAUD_RATES;

enum class Diagnosis {
    Stopped,    // no port open
    Listening,  // just opened; not enough time to judge
    Silent,     // nothing received
    Mismatch,   // bytes received, but no valid messages
    Scanning,   // trying each speed
    Marginal,   // valid messages, but too many damaged ones
    NotHearing, // valid messages, but only about one a second: the FDC+
                // is timing out, so our answers aren't reaching it
    Slow,       // valid messages, but far fewer than ten a second
    Good,
};

struct ScanResult {
    bool done = false;
    int asked = 0;                  // speed index the user chose
    int found = -1;                 // speed index the FDC+ answered at, or -1
    std::vector<int> unopenable;    // speed indexes the port could not be set to
};

struct MonitorView {
    bool running = false;
    bool receiving = false;         // data received within the last 3 seconds
    std::string status;
    int baudIndex = 0;

    Diagnosis diagnosis = Diagnosis::Stopped;
    int scanIndex = -1;             // speed being tried while scanning
    ScanResult scan;                // last completed scan

    // Over the last window.
    double statPerSec = 0;
    int damagedPercent = 0;

    // Totals since start or clear().
    unsigned long bytes = 0;
    unsigned long goodFrames = 0;
    unsigned long badFrames = 0;    // right command, wrong checksum
    unsigned long garbageBytes = 0; // bytes that were not part of any message
    unsigned long otherCommands = 0;// READ and WRIT (not expected: no disks offered)

    // From the last good STAT.
    bool haveStat = false;
    int drive = 0xff;               // selected drive, 0xff = none
    bool headLoaded = false;
    int track = 0;

    // Log of recent events, oldest first. logTotal counts every line ever
    // added, so a viewer can tell which lines are new.
    std::deque<std::string> log;
    unsigned long logTotal = 0;
};

class Monitor {
public:
    // Opens the port at BAUD_RATES[index]; returns null and sets error on failure.
    using Opener = std::function<std::unique_ptr<Transport>(int index, std::string& error)>;

    Monitor() = default;
    ~Monitor();

    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    // transport is already open at BAUD_RATES[baudIndex]. opener reopens the
    // port at other speeds during a scan.
    void start(std::unique_ptr<Transport> transport, int baudIndex, Opener opener);
    void stop();

    // Try every speed now.
    void findSpeed();

    // Zero the counters and empty the log.
    void clear();

    MonitorView view() const;

    // Tuning, adjustable for tests.
    std::chrono::milliseconds window{2000};         // judge the link over this long
    std::chrono::milliseconds scanListen{1200};     // listen this long at each speed: the
                                                    // FDC+ asks once a second when unanswered
    std::chrono::milliseconds minGap{50};           // silence that ends a partial message
    std::chrono::milliseconds mismatchSilence{5000};// during a mismatch, quiet this long is silent
    double minStatPerSec = 3.0;                     // FDC+ sends about 10 a second
    double unansweredStatPerSec = 1.5;              // ...and 1 a second when not answered
    int maxDamagedPercent = 2;
    int minScanFrames = 2;

private:
    static constexpr size_t MSG_SIZE = 10;
    static constexpr size_t MAX_LOG = 200;

    using Clock = std::chrono::steady_clock;

    struct Sample {
        Clock::time_point when;
        unsigned good, other, bad, garbage, bytes;
    };

    // Message parser state, reset on every (re)open.
    struct Parser {
        uint8_t buf[MSG_SIZE];
        size_t idx = 0;
        Clock::time_point lastRx{};
        unsigned good = 0;          // STATs
        unsigned other = 0;         // valid READ and WRIT
        unsigned bad = 0;           // right command, wrong checksum
        unsigned garbage = 0;       // bytes not part of any message
        unsigned bytes = 0;
        std::vector<uint8_t> junk;  // current run of garbage, for the log
        unsigned junkCount = 0;
    };

    void run();
    void listen();
    void frame();
    void dropBytes(size_t n);       // first n bytes of parser_.buf are garbage
    void flushJunk(bool force);
    void evaluate();
    void scan();
    void setGap(int index);
    void reply(const char* name, uint16_t code, uint16_t data);

    void addLog(const std::string& line);   // caller must hold mutex_
    void log(const std::string& line);

    mutable std::mutex mutex_;              // guards view_
    std::condition_variable wake_;
    MonitorView view_;
    Clock::time_point lastRx_{};

    std::unique_ptr<Transport> transport_;
    Opener opener_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> scanRequested_{false};

    // Thread-only state.
    Parser parser_;
    int baudIndex_ = 0;
    bool scanning_ = false;
    std::chrono::milliseconds gap_{50};
    Clock::time_point lastJunkLog_{};
    std::deque<Sample> samples_;
    Clock::time_point openedAt_{};
    bool scannedThisEpisode_ = false;
    Diagnosis lastDiagnosis_ = Diagnosis::Listening;
    Clock::time_point lastByteAt_{};
    Diagnosis lastLogged_ = Diagnosis::Stopped;
    int lastDrive_ = -1, lastHead_ = -1, lastTrack_ = -1;
};

// Plain words for the drive status in a STAT, e.g. "Drive 0, head loaded, track 5".
std::string describeStat(int drive, bool headLoaded, int track);
