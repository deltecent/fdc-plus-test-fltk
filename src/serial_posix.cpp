// POSIX serial port (Linux, macOS). Setting a non-standard baud rate such as
// 403.2K and enumerating ports are platform specific; see serial_linux.cpp
// and serial_macos.cpp.

#include "serial_posix.h"
#include "transport.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr int WRITE_TIMEOUT_MS = 2000;

class SerialPort : public Transport {
public:
    SerialPort(int fd, std::string description) : fd_(fd), description_(std::move(description)) {}

    ~SerialPort() override { close(fd_); }

    int read(uint8_t* buf, size_t len, int timeoutMs) override
    {
        if (failed_)
            return -1;

        pollfd p{fd_, POLLIN, 0};
        int r = poll(&p, 1, timeoutMs);
        if (r < 0)
            return errno == EINTR ? 0 : fail("poll");
        if (r == 0)
            return 0;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
            return fail("device disconnected");

        ssize_t n = ::read(fd_, buf, len);
        if (n < 0)
            return (errno == EAGAIN || errno == EINTR) ? 0 : fail("read");
        return static_cast<int>(n);
    }

    bool write(const uint8_t* buf, size_t len) override
    {
        while (len > 0 && !failed_) {
            ssize_t n = ::write(fd_, buf, len);
            if (n > 0) {
                buf += n;
                len -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno != EAGAIN && errno != EINTR) {
                fail("write");
                return false;
            }
            pollfd p{fd_, POLLOUT, 0};
            if (poll(&p, 1, WRITE_TIMEOUT_MS) <= 0)
                return false;
        }
        return !failed_;
    }

    std::string status() const override { return description_; }

private:
    int fail(const char* what)
    {
        failed_ = true;
        description_ += std::string(" - error: ") + what;
        return -1;
    }

    int fd_;
    std::string description_;
    bool failed_ = false;
};

} // namespace

std::unique_ptr<Transport> openSerial(const std::string& name, int baud,
                                      const std::string& baudLabel, std::string& error)
{
    int fd = open(name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        error = "Cannot open " + name + ": " + std::strerror(errno);
        return nullptr;
    }

    if (ioctl(fd, TIOCEXCL) < 0) {
        error = "Cannot get exclusive access to " + name + ": " + std::strerror(errno);
        close(fd);
        return nullptr;
    }

    // Raw 8N1, no flow control, no modem control lines.
    termios tio{};
    if (tcgetattr(fd, &tio) < 0) {
        error = name + " is not a serial port";
        close(fd);
        return nullptr;
    }
    cfmakeraw(&tio);
    tio.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB | CSTOPB | CRTSCTS | HUPCL);
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    tio.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        error = "Cannot configure " + name + ": " + std::strerror(errno);
        close(fd);
        return nullptr;
    }

    if (!serialSetBaud(fd, baud)) {
        error = "Baud rate " + baudLabel + " is not supported by " + name;
        close(fd);
        return nullptr;
    }

    int lines = TIOCM_DTR | TIOCM_RTS;
    ioctl(fd, TIOCMBIC, &lines);
    tcflush(fd, TCIOFLUSH);

    return std::make_unique<SerialPort>(fd, "Serial " + name + " at " + baudLabel);
}
