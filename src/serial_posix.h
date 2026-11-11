// Hooks serial_posix.cpp needs from the platform file (serial_linux.cpp or
// serial_macos.cpp).
#pragma once

// Set an arbitrary baud rate on an open, already configured serial port.
bool serialSetBaud(int fd, int baud);
