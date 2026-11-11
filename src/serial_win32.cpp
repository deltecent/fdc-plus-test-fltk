// Windows serial port.

#include "transport.h"

#include <windows.h>

#include <algorithm>

namespace {

constexpr DWORD WRITE_TIMEOUT_MS = 2000;

class SerialPort : public Transport {
public:
    SerialPort(HANDLE h, std::string description) : h_(h), description_(std::move(description)) {}

    ~SerialPort() override { CloseHandle(h_); }

    int read(uint8_t* buf, size_t len, int timeoutMs) override
    {
        if (failed_)
            return -1;

        // Return as soon as any bytes are available, or after timeoutMs.
        DWORD timeout = static_cast<DWORD>(std::max(timeoutMs, 1));
        if (timeout != readTimeout_) {
            COMMTIMEOUTS t{};
            t.ReadIntervalTimeout = MAXDWORD;
            t.ReadTotalTimeoutMultiplier = MAXDWORD;
            t.ReadTotalTimeoutConstant = timeout;
            t.WriteTotalTimeoutConstant = WRITE_TIMEOUT_MS;
            if (!SetCommTimeouts(h_, &t))
                return fail("SetCommTimeouts");
            readTimeout_ = timeout;
        }

        DWORD n = 0;
        if (!ReadFile(h_, buf, static_cast<DWORD>(len), &n, nullptr))
            return fail("device disconnected");
        return static_cast<int>(n);
    }

    bool write(const uint8_t* buf, size_t len) override
    {
        if (failed_)
            return false;
        DWORD n = 0;
        if (!WriteFile(h_, buf, static_cast<DWORD>(len), &n, nullptr)) {
            fail("write");
            return false;
        }
        return n == len;
    }

    std::string status() const override { return description_; }

private:
    int fail(const char* what)
    {
        failed_ = true;
        description_ += std::string(" - error: ") + what;
        return -1;
    }

    HANDLE h_;
    std::string description_;
    DWORD readTimeout_ = 0;
    bool failed_ = false;
};

} // namespace

std::vector<std::string> listSerialPorts()
{
    std::vector<std::string> ports;
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0,
                      KEY_READ, &key) != ERROR_SUCCESS)
        return ports;

    for (DWORD i = 0;; ++i) {
        char valueName[256];
        DWORD valueNameLen = sizeof(valueName);
        char data[256];
        DWORD dataLen = sizeof(data) - 1;
        DWORD type = 0;
        LONG rc = RegEnumValueA(key, i, valueName, &valueNameLen, nullptr, &type,
                                reinterpret_cast<BYTE*>(data), &dataLen);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc != ERROR_SUCCESS || type != REG_SZ)
            continue;
        data[dataLen] = '\0';
        ports.emplace_back(data);
    }
    RegCloseKey(key);

    // Natural sort so COM10 follows COM9.
    std::sort(ports.begin(), ports.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return ports;
}

std::unique_ptr<Transport> openSerial(const std::string& name, int baud,
                                      const std::string& baudLabel, std::string& error)
{
    std::string path = "\\\\.\\" + name;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = "Cannot open " + name;
        return nullptr;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        error = name + " is not a serial port";
        CloseHandle(h);
        return nullptr;
    }

    // Binary 8N1, no flow control, DTR and RTS off.
    DCB fresh{};
    fresh.DCBlength = sizeof(fresh);
    fresh.fBinary = TRUE;
    fresh.fDtrControl = DTR_CONTROL_DISABLE;
    fresh.fRtsControl = RTS_CONTROL_DISABLE;
    fresh.BaudRate = static_cast<DWORD>(baud);
    fresh.ByteSize = 8;
    fresh.Parity = NOPARITY;
    fresh.StopBits = ONESTOPBIT;
    fresh.XonChar = dcb.XonChar;
    fresh.XoffChar = dcb.XoffChar;
    fresh.XonLim = dcb.XonLim;
    fresh.XoffLim = dcb.XoffLim;
    if (!SetCommState(h, &fresh)) {
        error = "Baud rate " + baudLabel + " is not supported by " + name;
        CloseHandle(h);
        return nullptr;
    }

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return std::make_unique<SerialPort>(h, "Serial " + name + " at " + baudLabel);
}
