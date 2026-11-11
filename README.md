# FDC+ Tester

Checks that an FDC+ Enhanced Floppy Disk Controller set for the serial drive
is talking to the serial port, and finds its speed when it doesn't match. The
tester pretends to be an FDC+ Serial Drive Server. In drive type 7 (or 6) the
FDC+ sends the server a status message about ten times a second, whether or
not the Altair is using a disk. The tester listens to those messages, answers
them, and tells you in plain words what it found.

The most common problem is a **speed mismatch**: the FDC+ and the PC use
different baud rates. When that happens, the tester tries every speed and tells
you which one the FDC+ is using.

The tester never offers the FDC+ a disk, so nothing can be read or written.

Runs on Windows, macOS, and Linux as a single program with nothing to install.
To test a drive server instead, use the
[FDC+ Server Tester](https://github.com/deltecent/fdc-sds-test-fltk).

Version 1.0. Copyright © 2026 Deltec Enterprises LLC. MIT License.

## Download

| Platform | Download | Notes |
|---|---|---|
| Windows | [fdcplustest-windows-x64.zip](https://github.com/deltecent/fdc-plus-test-fltk/releases/latest/download/fdcplustest-windows-x64.zip) | Windows 10 or later. |
| macOS | [fdcplustest-macos-universal.tar.gz](https://github.com/deltecent/fdc-plus-test-fltk/releases/latest/download/fdcplustest-macos-universal.tar.gz) | macOS 11 or later, Apple Silicon and Intel. |
| Linux | [fdcplustest-linux-x86_64.tar.gz](https://github.com/deltecent/fdc-plus-test-fltk/releases/latest/download/fdcplustest-linux-x86_64.tar.gz) | Needs a desktop with GTK 3. |

**Windows:** open the zip file, drag `fdcplustest.exe` to your desktop (or any
folder), and double-click it. If Windows shows "Windows protected your PC",
click **More info**, then **Run anyway**.

**macOS:** the program is not signed, so the first time, right-click it in
Finder and choose **Open**.

**Linux:** your account must be in the `dialout` group to use serial ports
(`sudo usermod -aG dialout $USER`, then log out and back in).

## What you need

- The Altair with the FDC+, its Drive Type switches set to **7** (serial
  drive as an 8" drive) or **6** (serial drive as a Minidisk). The FDC+ reads
  the switches only at power-on.
- A computer with a serial port, and the cable you use (or will use) between
  the FDC+ and the drive server.
- The drive server must **not** be running on that port. Only one program can
  use a serial port at a time.

## How to use it

1. Connect the cable and turn the Altair on.
2. Start the tester.
3. In **Serial port**, choose the port the cable is plugged into. On Windows
   these are named COM1, COM2, COM3, and so on. If you plugged in a USB
   serial adapter after starting the tester, click **Find Ports** first.
4. In **Speed**, choose the speed you expect the FDC+ to use. 403.2K is the
   usual FDC+ speed.

The tester starts listening right away. After two seconds the big message
says what it found, and the light next to it turns green or red:

- **Yes! The FDC+ is working at 403.2K.** Everything is connected correctly.
  Use the same speed in your drive server.
- **Nothing from the FDC+.** Nothing at all is arriving. Check that the
  Altair is on, that the FDC+ Drive Type switches are at 7 (or 6), that the
  cable is plugged in at both ends, and that you chose the right serial port.
- **Speed mismatch! The FDC+ is set to 230.4K, but you chose 403.2K.** Bytes
  are arriving, but they only make sense at another speed. Click **Use
  230.4K**, or change the FDC+ to the speed you want. The tester tries
  every speed by itself the first time it sees this. Click **Find Speed** to
  try again, which takes up to ten seconds.
- **Data arrives, but makes no sense at any speed.** Usually a bad cable or
  noise. Some USB serial adapters can't use 403.2K at all. If so, the message
  says so, because the FDC+ may be set to a speed the adapter can't use.
- **The FDC+ is talking, but N% of its messages are damaged.** The speeds
  match, but the link is unreliable. 460.8K is borderline for the FDC+: use
  403.2K on both sides if your serial port can.
- **The FDC+ is talking, but isn't hearing the answers.** Messages from the
  FDC+ arrive, but only about once a second. The FDC+ slows down like this
  when nothing comes back to it. The wire that carries data from the computer
  to the FDC+ is probably broken or not connected.

**What the FDC+ reports** shows the drive the Altair has selected, whether the
head is loaded, and the track. No drive selected is normal while the Altair is
not using a disk. Because the tester tells the FDC+ that no disks are loaded,
the Altair will report that the drive is not ready if it tries to use one.

**What happened** lists everything the tester saw, including the bytes it
could not make sense of and the result at each speed it tried. Click
**Clear** to set the counts back to zero and empty the list.

### Saved settings

The serial port, speed, and window position are remembered for next time.

## Building from source

Requires CMake 3.20+, a C++17 compiler, and git (to fetch FLTK, which is built
and linked statically).

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release

The program is `build/fdcplustest` (`build/Release/fdcplustest.exe` with Visual Studio).

To run the tests:

    ctest --test-dir build -C Release --output-on-failure

For a universal macOS binary, add
`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0`
when configuring.

Linux needs the X11/Wayland development packages FLTK uses, e.g. on Debian/Ubuntu:

    sudo apt install build-essential cmake git libx11-dev libxext-dev libxft-dev \
        libxinerama-dev libxcursor-dev libxrender-dev libxfixes-dev libpango1.0-dev \
        libcairo2-dev libwayland-dev wayland-protocols libxkbcommon-dev \
        libdbus-1-dev libgtk-3-dev

### Testing without an FDC+

[AltairSim](https://github.com/deltecent/altairsim)'s `fdcplus` board emulates
the FDC+ serial drive, link timing included. Connect two serial ports with a
null-modem cable, put the simulated FDC+ on one, and run the tester on the
other:

    altairsim --set "fdc0 baud=230400" \
        -x "CONNECT fdc0:line serial:/dev/cu.usbserial-A" -x "RUN 0" \
        -i tools/altairsim-fdc7.toml

`tools/altairsim-fdc7.toml` is the `default` machine with its 88-DCDD swapped
for an FDC+ at drive type 7. Change `baud` to see the mismatch messages.

### Releases

GitHub Actions builds Windows, macOS, and Linux on every push to `master` and
every pull request, and runs the tests. To publish a release with downloadable
programs, push a version tag:

    git tag v1.0
    git push origin v1.0

The release appears on the
[Releases](https://github.com/deltecent/fdc-plus-test-fltk/releases) page
with the three download files attached.

### Source layout

| File | Purpose |
|---|---|
| `src/main.cpp` | FLTK user interface and the wording of each diagnosis |
| `src/monitor.*` | Server side of the FDC+ protocol, the diagnosis, and the speed scan (runs on its own thread) |
| `src/transport.h` | Serial port interface |
| `src/serial_posix.cpp` | Serial port for Linux and macOS |
| `src/serial_linux.cpp` / `serial_macos.cpp` | Custom baud rates, port list |
| `src/serial_win32.cpp` | Serial port for Windows |
| `tests/monitor_test.cpp` | Monitor tests against a fake FDC+ |
| `tools/altairsim-fdc7.toml` | AltairSim machine with a simulated FDC+, for testing without one |
| `tools/make_icon.py` | Draws `resources/fdcplustest.ico` (run from the repository root) |

The sources contain no conditional compilation; `CMakeLists.txt` picks the
platform files.
