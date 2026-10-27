# JMComic C++ downloader

A C++23 application with an FTXUI terminal interface. Search by keyword or comic ID, browse account favorites and folders, choose chapters and a destination,
and download decoded pages as PNG.

## Build and run

Linux is the validated platform. Requirements: C++23 Compiler, CMake, Git, Ninja/Make, OpenSSL 3 dev, and CA certificate.

```sh
sudo apt install build-essential cmake ninja-build git libssl-dev ca-certificates
git clone https://github.com/void-512/jmcomic-cpp.git
cd jmcomic-cpp
git submodule update --init --recursive
mkdir build && cd build
```

## Controls and workflow

- **Tab / Shift-Tab** move focus, arrow keys navigate, **Enter** activates a button or opens a result, and **Space** toggles chapter checkboxes.
- Use the top tabs to switch between **Search**, **Chapters**, **Account**, **Queue**, and **Settings**. **Ctrl-Q** or **Quit** cancels work and closes.
- **Search:** enter a keyword or comic ID and press Enter or Search. Choose Latest, Most clicks, or Most favorites. Previous/Next browse pages.
  Results show titles, IDs, and authors.
- **Account:** enter credentials to log in; the password field is masked and cleared on submission. Passwords are not saved. A saved cookie session is
  checked at startup; Log out removes it. Favorites on Search loads the list; choose a folder and Load folder to filter it. Open a favorite like any result.
- **Chapters:** read comic metadata, select individual/multiple/all chapters, edit the destination, and Queue selected chapters.
- **Queue:** Pause selected / Pause all stop new page work while in-flight pages finish. Resume selected / Resume all continue paused tasks and retry failed
  ones. Cancel selected / Cancel all stop tasks and remove them from the queue, keeping downloaded pages. Completed tasks show a green ✓ Finished label
  on the right. After restarting, find the comic and queue the same chapters and destination to resume them.
- **Settings:** edit concurrency (1–8), timeout, retry count, request spacing, API and image origins, optional HTTP(S)/SOCKS proxy, and destination. Apply
  after active downloads stop. Retries use the current applied network settings. Empty proxy means a direct connection; no proxy environment setup is needed.

Defaults are 4 download workers, a 30-second request timeout, 3 retries, and at least 250 ms between requests. Saved settings override these defaults.

## Output, correctness, and resource use

```text
Destination/
  Comic title [comic ID]/
    Chapter name [chapter ID]/
      001.png
      002.png
```

## Project structure and dependencies

```text
CMakeLists.txt           Main executable, compiler settings, and install rule
cmake/Dependencies.cmake Third-party build configuration
include/jmcomic/         Application headers
src/main.cpp            Application entry point
src/models.cpp          API models and settings validation
src/storage.cpp         Configuration and atomic file writes
src/api/                API client and HTTP transport
src/auth/               Cryptographic helpers
src/downloader/         Download queue and workers
src/image/              Image decoding and PNG output
src/tui/                Terminal interface
external/               Pinned dependency submodules
```

| Dependency |
| --- |
| FTXUI 7.0.3 |
| nlohmann/json 3.12.0 |
| stb (JPEG/PNG/GIF decoding) |
| libwebp 1.6.0 |
| curl 8.23.0 |
| zlib-ng 2.3.3 (PNG compression) |

