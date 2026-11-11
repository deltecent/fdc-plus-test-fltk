// Byte-stream transport the tester talks to the server over.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Transport {
public:
    virtual ~Transport() = default;

    // Read up to len bytes, waiting at most timeoutMs for the first byte.
    // Returns the number of bytes read (0 on timeout) or -1 on a fatal error.
    virtual int read(uint8_t* buf, size_t len, int timeoutMs) = 0;

    // Write all len bytes. Returns false on failure.
    virtual bool write(const uint8_t* buf, size_t len) = 0;

    // Human readable state, e.g. "Serial /dev/ttyUSB0 at 403.2K".
    virtual std::string status() const = 0;
};

// Serial port (implemented per platform in serial_*.cpp).
std::vector<std::string> listSerialPorts();
std::unique_ptr<Transport> openSerial(const std::string& name, int baud,
                                      const std::string& baudLabel, std::string& error);
