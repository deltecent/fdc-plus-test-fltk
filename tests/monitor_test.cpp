// Monitor tests. A fake transport plays an FDC+ set to one speed: opened at
// that speed it sends STAT messages, opened at any other it sends garbage,
// the way a real serial port does when the speeds don't match.

#include "../src/monitor.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

uint16_t sum(const uint8_t* p, size_t len)
{
    uint32_t s = 0;
    for (size_t i = 0; i < len; ++i)
        s += p[i];
    return static_cast<uint16_t>(s);
}

struct FakeFdc {
    std::mutex mutex;

    // Behaviour
    int speed = 0;                  // index into BAUD_RATES
    bool unplugged = false;
    bool garbageEverywhere = false;
    int corruptEvery = 0;           // damage every Nth STAT
    milliseconds interval{10};      // between STATs when answered
    milliseconds timeout{0};        // if set: between STATs when not answered,
                                    // as the real FDC+ waits out its timeout
    bool deaf = false;              // never hears the answers (broken wire)
    int sparse = 0;                 // at the wrong speed, only every Nth message
                                    // shows up (the rest vanish, as at 9.6K)
    std::set<int> unopenable;
    int drive = 0xff, head = 0, track = 0;
    bool sendRead = false;

    // Observed
    std::vector<int> opens;
    std::vector<std::vector<uint8_t>> replies;

    std::vector<uint8_t> message()
    {
        std::vector<uint8_t> m = {'S', 'T', 'A', 'T',
                                  static_cast<uint8_t>(drive), static_cast<uint8_t>(head),
                                  static_cast<uint8_t>(track & 0xff), static_cast<uint8_t>(track >> 8)};
        if (sendRead) {
            std::memcpy(m.data(), "READ", 4);
            m[4] = 0;
            m[5] = 0x10;    // drive 1, track 0
            m[6] = 0x20;
            m[7] = 0x11;
        }
        uint16_t s = sum(m.data(), 8);
        m.push_back(static_cast<uint8_t>(s & 0xff));
        m.push_back(static_cast<uint8_t>(s >> 8));
        return m;
    }
};

class FakePort : public Transport {
public:
    FakePort(FakeFdc& fdc, int speed) : fdc_(fdc), speed_(speed), next_(Clock::now()) {}

    int read(uint8_t* buf, size_t len, int timeoutMs) override
    {
        auto deadline = Clock::now() + milliseconds(timeoutMs);
        for (;;) {
            generate();
            if (!pending_.empty()) {
                size_t n = std::min(len, pending_.size());
                for (size_t i = 0; i < n; ++i) {
                    buf[i] = pending_.front();
                    pending_.pop_front();
                }
                return static_cast<int>(n);
            }
            if (Clock::now() >= deadline)
                return 0;
            std::this_thread::sleep_for(milliseconds(1));
        }
    }

    bool write(const uint8_t* buf, size_t len) override
    {
        std::lock_guard<std::mutex> lock(fdc_.mutex);
        fdc_.replies.emplace_back(buf, buf + len);
        // Heard only at the FDC+'s own speed.
        if (fdc_.timeout.count() && !fdc_.deaf && speed_ == fdc_.speed)
            next_ = std::min(next_, Clock::now() + fdc_.interval);
        return true;
    }

    std::string status() const override { return std::string("Fake at ") + BAUD_RATES[speed_].label; }

private:
    void generate()
    {
        std::lock_guard<std::mutex> lock(fdc_.mutex);
        while (Clock::now() >= next_) {
            next_ = Clock::now() + (fdc_.timeout.count() ? fdc_.timeout : fdc_.interval);
            if (fdc_.unplugged)
                continue;
            std::vector<uint8_t> m = fdc_.message();
            if ((fdc_.garbageEverywhere || speed_ != fdc_.speed) && fdc_.sparse
                && ++dropped_ % fdc_.sparse != 0)
                continue;
            if (fdc_.garbageEverywhere || speed_ != fdc_.speed) {
                // What a mismatched UART makes of the bits: never a valid message.
                for (auto& b : m)
                    b = static_cast<uint8_t>(0x80 | ((b * 7 + count_) & 0x7f));
            } else if (fdc_.corruptEvery && ++count_ % fdc_.corruptEvery == 0) {
                m[9] ^= 0x55;
            }
            pending_.insert(pending_.end(), m.begin(), m.end());
        }
    }

    FakeFdc& fdc_;
    int speed_;
    Clock::time_point next_;
    std::deque<uint8_t> pending_;
    int count_ = 0;
    int dropped_ = 0;
};

Monitor::Opener opener(FakeFdc& fdc)
{
    return [&fdc](int index, std::string& error) -> std::unique_ptr<Transport> {
        std::lock_guard<std::mutex> lock(fdc.mutex);
        if (fdc.unopenable.count(index)) {
            error = "speed not supported";
            return nullptr;
        }
        fdc.opens.push_back(index);
        return std::make_unique<FakePort>(fdc, index);
    };
}

void fast(Monitor& m)
{
    m.window = milliseconds(300);
    m.scanListen = milliseconds(150);
    m.minGap = milliseconds(5);
}

void begin(Monitor& m, FakeFdc& fdc, int speed)
{
    auto open = opener(fdc);
    std::string error;
    m.start(open(speed, error), speed, open);
}

bool waitFor(const Monitor& m, const std::function<bool(const MonitorView&)>& pred,
             milliseconds timeout = milliseconds(5000))
{
    auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (pred(m.view()))
            return true;
        std::this_thread::sleep_for(milliseconds(5));
    }
    return pred(m.view());
}

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok)
        ++failures;
}

bool is(const MonitorView& v, Diagnosis d)
{
    return v.diagnosis == d;
}

void testGood()
{
    FakeFdc fdc;
    Monitor m;
    fast(m);
    begin(m, fdc, 0);
    check(waitFor(m, [](const MonitorView& v) { return is(v, Diagnosis::Good); }), "matching speed is good");
    MonitorView v = m.view();
    check(v.statPerSec > 50, "message rate measured");
    check(v.badFrames == 0, "no damaged messages");
    m.stop();

    std::lock_guard<std::mutex> lock(fdc.mutex);
    bool allValid = !fdc.replies.empty();
    for (const auto& r : fdc.replies) {
        allValid = allValid && r.size() == 10 && std::memcmp(r.data(), "STAT", 4) == 0
            && r[4] == 0 && r[5] == 0 && r[6] == 0 && r[7] == 0
            && sum(r.data(), 8) == static_cast<uint16_t>(r[8] | (r[9] << 8));
    }
    check(allValid, "STAT answered with code 0, no disks, good checksum");
    check(fdc.opens.size() == 1, "no speed scan");
}

void testSilent()
{
    FakeFdc fdc;
    fdc.unplugged = true;
    Monitor m;
    fast(m);
    begin(m, fdc, 0);
    check(waitFor(m, [](const MonitorView& v) { return is(v, Diagnosis::Silent); }), "nothing received is silent");
    std::this_thread::sleep_for(milliseconds(200));
    m.stop();
    std::lock_guard<std::mutex> lock(fdc.mutex);
    check(fdc.opens.size() == 1 && fdc.replies.empty(), "silence does not start a scan");
}

void testMismatch()
{
    FakeFdc fdc;
    fdc.speed = 2;  // 230.4K
    Monitor m;
    fast(m);
    begin(m, fdc, 0);
    check(waitFor(m, [](const MonitorView& v) { return v.scan.done; }), "mismatch starts a scan");
    MonitorView v = m.view();
    check(v.scan.found == 2 && v.scan.asked == 0, "scan finds the FDC+ speed");
    check(waitFor(m, [](const MonitorView& v) { return is(v, Diagnosis::Mismatch); }),
          "back at the chosen speed, still a mismatch");
    std::this_thread::sleep_for(milliseconds(500));
    m.stop();
    std::lock_guard<std::mutex> lock(fdc.mutex);
    check(fdc.opens == std::vector<int>({0, 0, 1, 2, 0}), "tries speeds in order, stops when found, reopens");
}

// The FDC+ asks only once per timeout until it is answered, so the scan has
// to listen longer than that and answer what it hears.
void testMismatchUnanswered()
{
    FakeFdc fdc;
    fdc.speed = 2;
    fdc.timeout = milliseconds(120);
    Monitor m;
    fast(m);
    begin(m, fdc, 0);
    check(waitFor(m, [](const MonitorView& v) { return v.scan.done; }), "unanswered FDC+: scan runs");
    check(m.view().scan.found == 2, "unanswered FDC+: scan finds its speed");
}

void testNotHearing()
{
    FakeFdc fdc;
    fdc.timeout = milliseconds(250);
    fdc.deaf = true;
    Monitor m;
    fast(m);
    m.window = milliseconds(1000);
    m.minStatPerSec = 8;
    m.unansweredStatPerSec = 6;
    begin(m, fdc, 0);
    check(waitFor(m, [](const MonitorView& v) { return is(v, Diagnosis::NotHearing); }),
          "FDC+ that never hears answers is detected");

    FakeFdc hearing;
    hearing.timeout = milliseconds(250);
    Monitor m2;
    fast(m2);
    m2.window = milliseconds(1000);
    m2.minStatPerSec = 8;
    m2.unansweredStatPerSec = 6;
    begin(m2, hearing, 0);
    check(waitFor(m2, [](const MonitorView& v) { return is(v, Diagnosis::Good); }),
          "FDC+ that hears answers speeds up and is good");
}

// Far below the FDC+'s speed most messages vanish, leaving quiet windows.
// That must stay a mismatch, and must not start a scan every few seconds.
void testSparseMismatch()
{
    FakeFdc fdc;
    fdc.speed = 0;
    fdc.sparse = 50;    // one garbled burst every 500 ms
    Monitor m;
    fast(m);
    begin(m, fdc, 7);
    check(waitFor(m, [](const MonitorView& v) { return v.scan.done; }), "sparse mismatch: scan runs");
    check(m.view().scan.found == 0, "sparse mismatch: finds the speed");
    bool silent = false;
    auto until = Clock::now() + milliseconds(3000);
    while (Clock::now() < until) {
        silent = silent || is(m.view(), Diagnosis::Silent);
        std::this_thread::sleep_for(milliseconds(5));
    }
    check(!silent, "sparse mismatch: quiet windows are not called silent");
    check(is(m.view(), Diagnosis::Mismatch), "sparse mismatch: still a mismatch");
    m.stop();
    std::lock_guard<std::mutex> lock(fdc.mutex);
    check(fdc.opens.size() == 3, "sparse mismatch: scanned only once");
}

void testNoSpeed()
{
    FakeFdc fdc;
    fdc.garbageEverywhere = true;
    Monitor m;
    fast(m);
    begin(m, fdc, 0);
    check(waitFor(m, [](const MonitorView& v) { return v.scan.done; }), "garbage starts a scan");
    check(m.view().scan.found == -1, "no speed found");
    m.stop();
    std::lock_guard<std::mutex> lock(fdc.mutex);
    check(fdc.opens.size() == 1 + static_cast<size_t>(NUM_BAUD_RATES) + 1, "every speed tried");
}

void testFindSpeedButton()
{
    FakeFdc fdc;
    Monitor m;
    fast(m);
    begin(m, fdc, 0);
    waitFor(m, [](const MonitorView& v) { return is(v, Diagnosis::Good); });
    m.findSpeed();
    check(waitFor(m, [](const MonitorView& v) { return v.scan.done; }), "find speed runs a scan");
    check(m.view().scan.found == 0, "scan confirms the chosen speed");
    check(waitFor(m, [](const MonitorView& v) { return is(v, Diagnosis::Good); }), "good again after scan");
}

void testMarginal()
{
    FakeFdc fdc;
    fdc.corruptEvery = 5;
    fdc.speed = 1;
    Monitor m;
    fast(m);
    begin(m, fdc, 1);
    check(waitFor(m, [](const MonitorView& v) { return is(v, Diagnosis::Marginal); }),
          "damaged messages make the link marginal");
    check(m.view().damagedPercent >= 10, "damaged percentage reported");
}

void testUnopenable()
{
    FakeFdc fdc;
    fdc.speed = 1;
    fdc.unopenable = {0};
    Monitor m;
    fast(m);
    begin(m, fdc, 2);
    check(waitFor(m, [](const MonitorView& v) { return v.scan.done; }), "scan with an unusable speed");
    MonitorView v = m.view();
    check(v.scan.found == 1, "still finds the FDC+ speed");
    check(v.scan.unopenable == std::vector<int>({0}), "unusable speed reported");
}

void testStatParams()
{
    FakeFdc fdc;
    fdc.drive = 3;
    fdc.head = 1;
    fdc.track = 42;
    Monitor m;
    fast(m);
    begin(m, fdc, 0);
    check(waitFor(m, [](const MonitorView& v) { return v.haveStat; }), "STAT decoded");
    MonitorView v = m.view();
    check(v.drive == 3 && v.headLoaded && v.track == 42, "drive, head, and track");
    check(describeStat(3, true, 42) == "Drive 3 selected, head loaded, track 42", "describeStat");
    check(describeStat(0xff, false, 0) == "No drive selected", "describeStat no drive");
}

void testSlow()
{
    FakeFdc fdc;
    fdc.interval = milliseconds(200);   // 5 a second
    Monitor m;
    fast(m);
    m.window = milliseconds(1000);
    m.minStatPerSec = 8;
    begin(m, fdc, 0);
    check(waitFor(m, [](const MonitorView& v) { return is(v, Diagnosis::Slow); }), "few messages is slow");
}

void testOtherCommands()
{
    FakeFdc fdc;
    fdc.sendRead = true;
    Monitor m;
    fast(m);
    begin(m, fdc, 0);
    check(waitFor(m, [](const MonitorView& v) { return v.otherCommands > 0; }), "READ counted");
    std::this_thread::sleep_for(milliseconds(400));
    check(m.view().scan.done == false, "READ is a valid message, not a mismatch");
    m.stop();
    std::lock_guard<std::mutex> lock(fdc.mutex);
    check(fdc.replies.empty(), "READ not answered");
}

void testTransportFailure()
{
    struct Broken : Transport {
        int read(uint8_t*, size_t, int) override { return -1; }
        bool write(const uint8_t*, size_t) override { return false; }
        std::string status() const override { return "Unplugged"; }
    };
    Monitor m;
    m.start(std::make_unique<Broken>(), 0, nullptr);
    check(waitFor(m, [](const MonitorView& v) { return !v.running; }), "port failure stops the monitor");
    check(m.view().status == "Unplugged", "failure shown");
}

} // namespace

int main()
{
    testGood();
    testSilent();
    testMismatch();
    testMismatchUnanswered();
    testNotHearing();
    testSparseMismatch();
    testNoSpeed();
    testFindSpeedButton();
    testMarginal();
    testUnopenable();
    testStatParams();
    testSlow();
    testOtherCommands();
    testTransportFailure();

    std::printf("%s\n", failures ? "FAILED" : "All tests passed");
    return failures ? 1 : 0;
}
