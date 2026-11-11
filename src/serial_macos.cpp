// macOS serial specifics: arbitrary baud rates via IOSSIOSPEED, and port
// enumeration of the /dev/cu.* call-out devices.

#include "serial_posix.h"
#include "transport.h"

#include <IOKit/serial/ioss.h>
#include <algorithm>
#include <dirent.h>
#include <sys/ioctl.h>

bool serialSetBaud(int fd, int baud)
{
    speed_t speed = static_cast<speed_t>(baud);
    return ioctl(fd, IOSSIOSPEED, &speed) == 0;
}

std::vector<std::string> listSerialPorts()
{
    std::vector<std::string> ports;
    DIR* dir = opendir("/dev");
    if (!dir)
        return ports;

    while (dirent* e = readdir(dir)) {
        std::string name = e->d_name;
        if (name.rfind("cu.", 0) != 0)
            continue;
        if (name.find("Bluetooth-Incoming-Port") != std::string::npos)
            continue;
        ports.push_back("/dev/" + name);
    }
    closedir(dir);

    std::sort(ports.begin(), ports.end());
    return ports;
}
