// Serial drive server side of the FDC+ protocol, used to check an FDC+.
//
// Commands and responses are ten byte messages: four ASCII command bytes
// followed by three little endian 16 bit words (param1, param2, checksum).
// The checksum is the 16 bit sum of the first eight bytes.
//
// With drive type 7 (serial drive) the FDC+ sends STAT about ten times a
// second, whether or not the Altair is using a disk:
//   STAT  param1 LSB = selected drive (0xff = none), MSB = head status;
//         param2 = current track.
// We answer with response code 0 and a mounted-drive bitmap of 0, so no
// disks are offered and the FDC+ has no reason to send READ or WRIT.
//
// At the wrong speed the bytes still arrive but never form a message with a
// good checksum, which is how a speed mismatch shows up. The scan then
// reopens the port at each speed in turn and listens for valid messages.

#include "monitor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

const BaudRate BAUD_RATES[] = {
    {403200, "403.2K"}, {460800, "460.8K"}, {230400, "230.4K"}, {76800, "76.8K"},
    {57600, "57.6K"},   {38400, "38.4K"},   {19200, "19.2K"},   {9600, "9.6K"},
};
const int NUM_BAUD_RATES = sizeof(BAUD_RATES) / sizeof(BAUD_RATES[0]);

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t COMMAND_LENGTH = 8;        // does not include checksum bytes
constexpr int READ_WAIT_MS = 10;
constexpr int GAP_CHARS = 20;               // a gap is also at least this many character times
constexpr size_t JUNK_SAMPLE = 16;          // garbage bytes shown in the log
constexpr auto JUNK_LOG_INTERVAL = std::chrono::seconds(5);
constexpr auto RECEIVE_LIGHT_TIME = std::chrono::seconds(3);
constexpr uint16_t RESPONSE_OK = 0;
constexpr uint16_t RESPONSE_NOT_READY = 1;
constexpr int NO_DRIVE = 0xff;

uint16_t getWord(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

void putWord(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>(v >> 8);
}

uint16_t checksum(const uint8_t* p, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i)
        sum += p[i];
    return static_cast<uint16_t>(sum);
}

bool knownCommand(const uint8_t* p)
{
    return std::memcmp(p, "STAT", 4) == 0 || std::memcmp(p, "READ", 4) == 0
        || std::memcmp(p, "WRIT", 4) == 0;
}

std::string hexBytes(const uint8_t* p, size_t len)
{
    std::string s;
    char buf[4];
    for (size_t i = 0; i < len; ++i) {
        std::snprintf(buf, sizeof(buf), i ? " %02X" : "%02X", p[i]);
        s += buf;
    }
    return s;
}

double seconds(Clock::duration d)
{
    return std::chrono::duration<double>(d).count();
}

} // namespace

std::string describeStat(int drive, bool headLoaded, int track)
{
    if (drive == NO_DRIVE)
        return "No drive selected";
    return "Drive " + std::to_string(drive) + " selected, head "
        + (headLoaded ? "loaded" : "unloaded") + ", track " + std::to_string(track);
}

Monitor::~Monitor()
{
    stop();
}

void Monitor::start(std::unique_ptr<Transport> transport, int baudIndex, Opener opener)
{
    stop();
    transport_ = std::move(transport);
    opener_ = std::move(opener);
    baudIndex_ = baudIndex;

    parser_ = Parser{};
    samples_.clear();
    openedAt_ = Clock::now();
    scanning_ = false;
    scannedThisEpisode_ = false;
    lastLogged_ = Diagnosis::Listening;
    lastDrive_ = lastHead_ = lastTrack_ = -1;
    lastJunkLog_ = Clock::time_point{};
    lastByteAt_ = Clock::time_point{};
    lastDiagnosis_ = Diagnosis::Listening;
    scanRequested_ = false;
    setGap(baudIndex_);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.running = true;
        view_.status = transport_->status();
        view_.baudIndex = baudIndex;
        view_.diagnosis = Diagnosis::Listening;
        view_.scanIndex = -1;
        view_.scan = ScanResult{};
        view_.statPerSec = 0;
        view_.damagedPercent = 0;
        view_.haveStat = false;
        lastRx_ = Clock::time_point{};
        addLog(std::string("Listening at ") + BAUD_RATES[baudIndex].label);
    }
    running_ = true;
    thread_ = std::thread(&Monitor::run, this);
}

void Monitor::stop()
{
    running_ = false;
    if (thread_.joinable())
        thread_.join();
    transport_.reset();

    std::lock_guard<std::mutex> lock(mutex_);
    view_.running = false;
    view_.diagnosis = Diagnosis::Stopped;
    view_.scanIndex = -1;
}

void Monitor::findSpeed()
{
    scanRequested_ = true;
}

void Monitor::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    view_.bytes = view_.goodFrames = view_.badFrames = 0;
    view_.garbageBytes = view_.otherCommands = 0;
    view_.log.clear();
}

MonitorView Monitor::view() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    MonitorView v = view_;
    v.receiving = v.running && lastRx_ != Clock::time_point{}
        && Clock::now() - lastRx_ < RECEIVE_LIGHT_TIME;
    return v;
}

void Monitor::run()
{
    while (running_) {
        if (scanRequested_.exchange(false)) {
            scan();
            continue;
        }
        listen();
        if (running_) {
            flushJunk(false);
            evaluate();
        }
    }
}

void Monitor::setGap(int index)
{
    int chars = GAP_CHARS * 10 * 1000 / BAUD_RATES[index].rate;
    gap_ = std::max(minGap, std::chrono::milliseconds(chars));
}

// Read whatever has arrived and feed it to the parser.
void Monitor::listen()
{
    uint8_t data[256];
    int n = transport_->read(data, sizeof(data), READ_WAIT_MS);
    if (n < 0) {
        running_ = false;
        std::lock_guard<std::mutex> lock(mutex_);
        view_.running = false;
        view_.diagnosis = Diagnosis::Stopped;
        view_.status = transport_->status();
        addLog("Serial port failed: " + view_.status);
        return;
    }

    Parser before = parser_;
    auto now = Clock::now();

    if (n == 0) {
        // A real gap discards any partial message, which keeps us in step
        // with the FDC+. USB adapters deliver bytes in packets, so a short
        // pause in the middle of a message is not a gap.
        if (parser_.idx > 0 && now - parser_.lastRx >= gap_)
            dropBytes(parser_.idx);
    } else {
        parser_.lastRx = now;
        parser_.bytes += static_cast<unsigned>(n);
        for (int i = 0; i < n; ++i) {
            parser_.buf[parser_.idx++] = data[i];
            if (parser_.idx == MSG_SIZE)
                frame();
        }
    }

    if (scanning_)
        return;

    Sample s{now, parser_.good - before.good, parser_.other - before.other, parser_.bad - before.bad,
             parser_.garbage - before.garbage, parser_.bytes - before.bytes};
    if (s.bytes || s.garbage)
        samples_.push_back(s);

    if (n > 0)
        lastByteAt_ = now;

    std::lock_guard<std::mutex> lock(mutex_);
    if (n > 0)
        lastRx_ = now;
    view_.bytes += s.bytes;
    view_.goodFrames += s.good;
    view_.badFrames += s.bad;
    view_.garbageBytes += s.garbage;
    view_.otherCommands += s.other;
}

// parser_.buf holds ten bytes.
void Monitor::frame()
{
    const uint8_t* m = parser_.buf;

    if (!knownCommand(m)) {
        // Slide along one byte, so stray bytes ahead of a message don't hide it.
        dropBytes(1);
        return;
    }

    bool sumOk = checksum(m, COMMAND_LENGTH) == getWord(m + 8);
    parser_.idx = 0;
    if (!scanning_)
        flushJunk(true);

    if (!sumOk) {
        ++parser_.bad;
        if (!scanning_)
            log("Damaged " + std::string(reinterpret_cast<const char*>(m), 4)
                + " (bad checksum): " + hexBytes(m, MSG_SIZE));
        return;
    }

    uint16_t param1 = getWord(m + 4);
    uint16_t param2 = getWord(m + 6);

    if (std::memcmp(m, "STAT", 4) == 0) {
        ++parser_.good;
        // Answer during a scan too: an unanswered FDC+ waits out its one
        // second timeout before asking again, an answered one asks ten
        // times a second.
        reply("STAT", RESPONSE_OK, 0);
        if (scanning_)
            return;

        int drive = param1 & 0xff;
        int head = (param1 >> 8) ? 1 : 0;
        int track = param2;
        std::lock_guard<std::mutex> lock(mutex_);
        view_.haveStat = true;
        view_.drive = drive;
        view_.headLoaded = head != 0;
        view_.track = track;
        if (drive != lastDrive_ || head != lastHead_ || (drive != NO_DRIVE && track != lastTrack_)) {
            addLog("STAT  " + describeStat(drive, head != 0, track));
            lastDrive_ = drive;
            lastHead_ = head;
            lastTrack_ = track;
        }
        return;
    }

    // READ or WRIT. Not expected, since we tell the FDC+ no disks are loaded.
    ++parser_.other;
    if (scanning_)
        return;
    if (std::memcmp(m, "WRIT", 4) == 0)
        reply("WRIT", RESPONSE_NOT_READY, 0);
    log(std::string(reinterpret_cast<const char*>(m), 4) + "  drive " + std::to_string(param1 >> 12)
        + " track " + std::to_string(param1 & 0x0fff) + " (no disk offered, so not answered)");
}

void Monitor::dropBytes(size_t n)
{
    n = std::min(n, parser_.idx);
    for (size_t i = 0; i < n; ++i) {
        if (parser_.junk.size() < JUNK_SAMPLE)
            parser_.junk.push_back(parser_.buf[i]);
        ++parser_.junkCount;
    }
    parser_.garbage += static_cast<unsigned>(n);
    std::memmove(parser_.buf, parser_.buf + n, parser_.idx - n);
    parser_.idx -= n;
}

// Log the run of garbage bytes, at most every five seconds unless forced.
void Monitor::flushJunk(bool force)
{
    if (parser_.junkCount == 0)
        return;
    auto now = Clock::now();
    if (!force && now - lastJunkLog_ < JUNK_LOG_INTERVAL)
        return;
    lastJunkLog_ = now;
    std::string line = std::to_string(parser_.junkCount) + " garbled byte"
        + (parser_.junkCount == 1 ? "" : "s") + ": " + hexBytes(parser_.junk.data(), parser_.junk.size());
    if (parser_.junkCount > parser_.junk.size())
        line += " ...";
    parser_.junk.clear();
    parser_.junkCount = 0;
    log(line);
}

void Monitor::evaluate()
{
    auto now = Clock::now();
    while (!samples_.empty() && now - samples_.front().when > window)
        samples_.pop_front();

    unsigned good = 0, other = 0, bad = 0, garbage = 0, bytes = 0;
    for (const auto& s : samples_) {
        good += s.good;
        other += s.other;
        bad += s.bad;
        garbage += s.garbage;
        bytes += s.bytes;
    }

    bool judged = now - openedAt_ >= window;
    double rate = good / seconds(window);
    unsigned damaged = bad + (garbage + MSG_SIZE - 1) / MSG_SIZE;
    int percent = good + damaged ? static_cast<int>(damaged * 100 / (good + damaged)) : 0;

    // At a speed far below the FDC+'s, a whole message can fit in one
    // character time and often vanishes, and an unanswered FDC+ asks only
    // once a second. So a mismatch can have quiet windows: once garbled
    // bytes have been seen, it takes a longer silence to call it silent.
    bool mismatched = lastDiagnosis_ == Diagnosis::Mismatch;
    bool recentBytes = lastByteAt_ != Clock::time_point{} && now - lastByteAt_ < mismatchSilence;

    Diagnosis d;
    if (!judged)
        d = Diagnosis::Listening;
    else if (bytes == 0 && mismatched && recentBytes)
        d = Diagnosis::Mismatch;
    else if (bytes == 0)
        d = Diagnosis::Silent;
    else if (good + other == 0)
        d = Diagnosis::Mismatch;
    else if (percent > maxDamagedPercent)
        d = Diagnosis::Marginal;
    else if (rate < unansweredStatPerSec)
        d = Diagnosis::NotHearing;
    else if (rate < minStatPerSec)
        d = Diagnosis::Slow;
    else
        d = Diagnosis::Good;

    if (d == Diagnosis::Mismatch && !scannedThisEpisode_) {
        scannedThisEpisode_ = true;
        scanRequested_ = true;
    } else if (d != Diagnosis::Mismatch && d != Diagnosis::Listening && d != Diagnosis::Silent) {
        // Only a working link ends the episode. Silence doesn't: a slow,
        // garbled trickle would otherwise start a new scan every few seconds.
        scannedThisEpisode_ = false;
    }
    if (d != Diagnosis::Listening)
        lastDiagnosis_ = d;

    std::lock_guard<std::mutex> lock(mutex_);
    view_.diagnosis = d;
    view_.statPerSec = judged ? rate : 0;
    view_.damagedPercent = percent;

    if (d == lastLogged_ || d == Diagnosis::Listening)
        return;
    lastLogged_ = d;
    char line[96];
    switch (d) {
    case Diagnosis::Silent:
        addLog("Nothing is arriving from the FDC+");
        break;
    case Diagnosis::Mismatch:
        addLog("Bytes are arriving, but none are valid FDC+ messages");
        break;
    case Diagnosis::Marginal:
        std::snprintf(line, sizeof(line), "Link unreliable: %d%% of messages damaged", percent);
        addLog(line);
        break;
    case Diagnosis::NotHearing:
        std::snprintf(line, sizeof(line), "Only %.1f STAT messages a second: the FDC+ is not getting our answers", rate);
        addLog(line);
        break;
    case Diagnosis::Slow:
        std::snprintf(line, sizeof(line), "Only %.1f STAT messages a second (expected about 10)", rate);
        addLog(line);
        break;
    case Diagnosis::Good:
        std::snprintf(line, sizeof(line), "Link good: %.1f STAT messages a second", rate);
        addLog(line);
        break;
    default:
        break;
    }
}

// Reopen the port at each speed in turn and listen for valid messages.
void Monitor::scan()
{
    scanning_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.diagnosis = Diagnosis::Scanning;
        addLog("Trying every speed to find the one the FDC+ uses...");
    }
    parser_.junk.clear();
    parser_.junkCount = 0;

    ScanResult r;
    r.asked = baudIndex_;
    unsigned bestGood = 0, bestBad = 0;

    transport_.reset();
    for (int i = 0; i < NUM_BAUD_RATES && running_; ++i) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.scanIndex = i;
        }

        std::string error;
        transport_ = opener_(i, error);
        if (!transport_) {
            r.unopenable.push_back(i);
            log(std::string("  ") + BAUD_RATES[i].label + ": this serial port can't use this speed ("
                + error + ")");
            continue;
        }

        setGap(i);
        parser_ = Parser{};

        // Unanswered, the FDC+ asks only once a second, so listen a little
        // longer than that. Once it is heard, give it the same time again to
        // be heard twice.
        auto until = Clock::now() + scanListen;
        bool heard = false;
        while (running_ && Clock::now() < until) {
            listen();
            unsigned valid = parser_.good + parser_.other;
            if (valid >= static_cast<unsigned>(minScanFrames))
                break;
            if (valid > 0 && !heard) {
                heard = true;
                until = Clock::now() + scanListen;
            }
        }
        transport_.reset();
        if (!running_)
            break;

        unsigned good = parser_.good + parser_.other;
        char line[128];
        std::snprintf(line, sizeof(line), "  %s: %u good message%s, %u damaged, %u garbled bytes",
                      BAUD_RATES[i].label, good, good == 1 ? "" : "s", parser_.bad, parser_.garbage);
        log(line);

        if (good >= static_cast<unsigned>(minScanFrames)
            && (good > bestGood || (good == bestGood && parser_.bad < bestBad))) {
            r.found = i;
            bestGood = good;
            bestBad = parser_.bad;
            if (parser_.bad == 0 && parser_.garbage < MSG_SIZE)
                break;      // clean: no need to try the rest
        }
    }
    r.done = true;

    if (!running_) {
        scanning_ = false;
        return;
    }

    std::string error;
    transport_ = opener_(baudIndex_, error);
    setGap(baudIndex_);
    parser_ = Parser{};
    samples_.clear();
    openedAt_ = Clock::now();
    lastByteAt_ = openedAt_;    // bytes were arriving when the scan began
    lastDiagnosis_ = Diagnosis::Mismatch;
    scanning_ = false;

    std::lock_guard<std::mutex> lock(mutex_);
    view_.scan = r;
    view_.scanIndex = -1;
    lastLogged_ = Diagnosis::Listening;
    if (r.found >= 0)
        addLog(std::string("The FDC+ is using ") + BAUD_RATES[r.found].label);
    else
        addLog("No speed gave valid FDC+ messages");

    if (!transport_) {
        running_ = false;
        view_.running = false;
        view_.diagnosis = Diagnosis::Stopped;
        view_.status = error;
        addLog("Could not reopen the serial port: " + error);
        return;
    }
    view_.diagnosis = Diagnosis::Listening;
    view_.status = transport_->status();
    addLog(std::string("Listening at ") + BAUD_RATES[baudIndex_].label);
}

void Monitor::reply(const char* name, uint16_t code, uint16_t data)
{
    uint8_t rsp[MSG_SIZE];
    std::memcpy(rsp, name, 4);
    putWord(rsp + 4, code);
    putWord(rsp + 6, data);
    putWord(rsp + 8, checksum(rsp, COMMAND_LENGTH));
    transport_->write(rsp, sizeof(rsp));   // a failure shows up on the next read
}

void Monitor::log(const std::string& line)
{
    std::lock_guard<std::mutex> lock(mutex_);
    addLog(line);
}

void Monitor::addLog(const std::string& line)
{
    view_.log.push_back(line);
    ++view_.logTotal;
    while (view_.log.size() > MAX_LOG)
        view_.log.pop_front();
}
