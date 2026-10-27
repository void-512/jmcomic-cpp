# JMComic C++ downloader

A C++23 application with an FTXUI terminal interface. Search by keyword or comic
ID, browse account favorites and folders, choose chapters and a destination,
and download decoded pages as PNG.

## Build and run

Linux is the validated platform. Requirements: GCC 13+ or Clang with C++23,
CMake 3.25+, Git, Ninja/Make, OpenSSL 3 development headers, and a system CA
certificate bundle.

On Debian / Raspberry Pi OS:

```sh
sudo apt install build-essential cmake ninja-build git libssl-dev ca-certificates
git clone https://github.com/void-512/jmcomic-cpp.git
cd jmcomic-cpp
git submodule update --init --recursive
```

Build in RAM-backed temporary storage and install the executable into `bin/`:

```sh
mkdir -p "$HOME/.cache/codex-tmp"
jm_build="$HOME/.cache/codex-tmp/jmcomic-build"
TMPDIR="$HOME/.cache/codex-tmp" cmake -S . -B "$jm_build" -G Ninja -DCMAKE_BUILD_TYPE=Release
TMPDIR="$HOME/.cache/codex-tmp" cmake --build "$jm_build" --target jmcomic -j 3
cmake --install "$jm_build" --prefix "$PWD"
./bin/jmcomic
```

`jmcomic` is the only application build target. Its third-party libraries build
automatically as dependencies. CMake also provides its standard build and install
utility targets. The executable's only command-line option is `--help`.

Use a writable installation directory. `jmcomic.json`, `jmcomic-session.json`,
and the default `Downloads/` directory live beside the executable. Settings and
downloads persist independently of the disposable build directory.

## Controls and workflow

- **Tab / Shift-Tab** move focus, arrow keys navigate, **Enter** activates a
  button or opens a result, and **Space** toggles chapter checkboxes.
- Use the top tabs to switch between **Search**, **Chapters**, **Account**,
  **Queue**, and **Settings**. **Ctrl-Q** or **Quit** cancels work and closes.
- **Search:** enter a keyword and press Enter or Search. Choose Latest, Most
  clicks, or Most favorites. Previous/Next browse pages. Get ID opens an ID
  directly. Results show titles, IDs, and authors.
- **Account:** enter credentials to log in; the password field is masked and
  cleared on submission. Passwords are not saved. A saved cookie session is
  checked at startup; Log out removes it. Favorites on Search loads the list;
  choose a folder and Load folder to filter it. Open a favorite like any result.
- **Chapters:** read comic metadata, select individual/multiple/all chapters,
  edit the destination, and Queue selected chapters.
- **Queue:** monitor chapter and overall page counts and progress bars. Cancel
  selected / Cancel all stop requests. Retry / resume verifies saved pages and
  downloads missing or damaged ones. After restarting, find the comic and queue
  the same chapters and destination to resume them.
- **Settings:** edit concurrency (1–8), timeout, retry count, request spacing,
  API and image origins, optional HTTP(S)/SOCKS proxy, and destination. Apply
  after active downloads stop. Retries use the current applied network settings.
  Empty proxy means a direct connection; no proxy environment setup is needed.

Defaults are 4 download workers, a 30-second request timeout, 3 retries, and
at least 250 ms between requests. Saved settings override these defaults.

## Output, correctness, and resource use

```text
Destination/
  Comic title [comic ID]/
    Chapter name [chapter ID]/
      chapter.json
      001.png
      001.png.json
      002.png
      002.png.json
```

Names are sanitized for common filesystems; IDs disambiguate duplicate titles.
Each PNG is encoded losslessly from decoded RGBA pixels, with JMComic's vertical
strip reordering applied. PNG sidecars record the source filename, chapter,
decoder version, and SHA-256. Resume skips only matching, verified output.
A PNG and its receipt are committed using sibling temporary files and atomic
renames. These small sibling files are necessary for atomic commits across
filesystems. Ordinary cancellation removes temporary writes; an abrupt kill
can leave harmless `.part` files, which are ignored on resume.

WebP, JPEG, PNG, and GIF input are recognized by content rather than filename.
Animated GIF/WebP pages become a PNG of the first composited frame. Output is
8-bit RGBA; animation, metadata/color profiles, and 16-bit source precision are
not retained. No JPEG recompression is performed. Network images are limited
to 64 MiB and decoded images to 24 megapixels / 32768 pixels on either side.
Reduce concurrency when memory is scarce: each worker owns compressed input,
pixel buffers, and PNG encoder working memory.

Session and configuration files use mode 0600 on Linux. Session cookies are
stored in plaintext and are scoped to the selected API origin. Remote response
bodies, credentials, and tokens are excluded from error messages. Windows file
permissions and overwrite semantics are not validated; Linux is recommended.
Run one application instance per output directory. The queue itself is held in
memory; page receipts provide restart/resume support.

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
THIRD_PARTY_NOTICES.md   Attribution and licenses
```

`src/api` contains HTTP transport, rate limiting, protocol signatures, response
decryption, and API model conversion. `src/auth` contains cryptographic helpers;
the API client owns cookie sessions. `src/image` handles decoding, strip
reordering, PNG encoding, and verified page receipts. `src/downloader` owns a
bounded worker pool, cancellation tokens, jobs, and progress snapshots.
`src/storage.cpp` owns config and atomic file writes. `src/tui` owns UI state:
one background command worker sends completion callbacks through a mutex queue
and FTXUI custom events; download workers publish snapshots. Network, decoding,
and output writes run outside the terminal event loop.

| Dependency |
| --- |
| FTXUI 7.0.3 |
| nlohmann/json 3.12.0 |
| stb |
| libwebp 1.6.0 |
| curl 8.23.0 |

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for attribution and licenses.
