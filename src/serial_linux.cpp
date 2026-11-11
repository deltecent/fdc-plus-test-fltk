// Linux serial specifics: arbitrary baud rates via termios2/BOTHER, and port
// enumeration via sysfs.
//
// <asm/termbits.h> conflicts with <termios.h>, which is why this lives apart
// from serial_posix.cpp.

#include "serial_posix.h"
#include "transport.h"

#include <algorithm>
#include <asm/termbits.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

bool serialSetBaud(int fd, int baud)
{
    termios2 tio{};
    if (ioctl(fd, TCGETS2, &tio) < 0)
        return false;
    tio.c_cflag &= ~static_cast<tcflag_t>(CBAUD);
    tio.c_cflag |= BOTHER;
    tio.c_ispeed = static_cast<speed_t>(baud);
    tio.c_ospeed = static_cast<speed_t>(baud);
    if (ioctl(fd, TCSETS2, &tio) < 0)
        return false;

    // Some drivers accept any rate silently; confirm it took.
    if (ioctl(fd, TCGETS2, &tio) < 0)
        return false;
    long actual = static_cast<long>(tio.c_ospeed);
    return actual > baud * 97L / 100 && actual < baud * 103L / 100;
}

namespace {

// The legacy ttyS devices always exist; only list ones backed by a UART.
bool isRealLegacyPort(const std::string& path)
{
    int fd = open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return false;
    serial_struct info{};
    bool real = ioctl(fd, TIOCGSERIAL, &info) == 0 && info.type != PORT_UNKNOWN;
    close(fd);
    return real;
}

} // namespace

std::vector<std::string> listSerialPorts()
{
    std::vector<std::string> ports;
    DIR* dir = opendir("/sys/class/tty");
    if (!dir)
        return ports;

    while (dirent* e = readdir(dir)) {
        std::string name = e->d_name;
        if (name == "." || name == "..")
            continue;

        struct stat st{};
        std::string device = "/sys/class/tty/" + name + "/device";
        if (stat(device.c_str(), &st) != 0)
            continue;                           // virtual console, pty, ...

        std::string path = "/dev/" + name;
        if (name.rfind("ttyS", 0) == 0 && !isRealLegacyPort(path))
            continue;
        ports.push_back(path);
    }
    closedir(dir);

    std::sort(ports.begin(), ports.end());
    return ports;
}
