# JMComic C++ downloader

A C++23 application with an FTXUI terminal interface. Search by keyword or comic
ID, browse account favorites and folders, choose chapters and a destination,
and download decoded pages as PNG. The adjacent `jmcomic-downloader` reference
project is unchanged.

## Build and run

Linux is the validated platform. Requirements: GCC 13+ or Clang with C++23,
CMake 3.25+, Git, Ninja (or Make), OpenSSL 3 development headers, and a system CA
certificate bundle. Python 3 and the `openssl` command are needed only for the
local integration test.

On Debian / Raspberry Pi OS:

```sh
sudo apt-get install build-essential cmake ninja-build git libssl-dev ca-certificates python3 openssl
```

After cloning this repository, initialize the pinned dependencies:

```sh
git submodule update --init --recursive
```

Or clone a published copy with `git clone --recurse-submodules <repository-url>`.
FTXUI, JSON, stb, libwebp, and curl are Git submodules; CMake does not download
additional sources. OpenSSL and platform libraries are discovered by CMake.

The following keeps disposable build files in RAM-backed cache, while installing
the executable into this repository's `bin/` directory:

```sh
mkdir -p "$HOME/.cache/codex-tmp"
jm_build="$HOME/.cache/codex-tmp/jmcomic-build"
TMPDIR="$HOME/.cache/codex-tmp" cmake -S . -B "$jm_build" -G Ninja -DCMAKE_BUILD_TYPE=Release
TMPDIR="$HOME/.cache/codex-tmp" cmake --build "$jm_build" -j 3
TMPDIR="$HOME/.cache/codex-tmp" ctest --test-dir "$jm_build" --output-on-failure
cmake --install "$jm_build" --prefix "$PWD"
./bin/jmcomic
```

Use a writable installation directory. `jmcomic.json`, `jmcomic-session.json`,
and the default `Downloads/` directory live **beside the executable**, independent
of the current directory. An explicit destination may point anywhere writable.
A normal build directory also works on machines without RAM-backed cache.
`--help` is the application's only command-line option.

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

Default limits are 3 parallel workers, a 30-second request timeout, 3 retries,
and at least 250 ms between requests. HTTP 429/Retry-After pauses all new
requests on that client; waits over 60 seconds are reported for manual retry.
TLS certificate and hostname verification stay enabled. Redirects are reported
so the user can update the origin; authentication headers are never forwarded
to another host. Service challenges are reported without attempting to bypass
them. Hosts can change: the working API origin checked on 2026-10-26 was
`https://www.cdnhth.net`; the reference's `www.cdnhth.cc` served a parked page.

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

## Architecture and dependencies

`src/api` contains HTTP transport, rate limiting, protocol signatures, response
decryption, and API model conversion. `src/auth` contains cryptographic helpers;
the API client owns cookie sessions. `src/image` handles decoding, strip
reordering, PNG encoding, and verified page receipts. `src/downloader` owns a
bounded worker pool, cancellation tokens, jobs, and progress snapshots.
`src/storage.cpp` owns config and atomic file writes. `src/tui` owns UI state:
one background command worker sends completion callbacks through a mutex queue
and FTXUI custom events; download workers publish snapshots. Network, decoding,
and output writes run outside the terminal event loop.

| Dependency | Pin |
| --- | --- |
| FTXUI 7.0.3 | `f921fad208912747c17d129a8ef75ec7624b6eec` |
| nlohmann/json 3.12.0 | `55f93686c01528224f448c19128836e7df245f72` |
| stb | `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5` |
| libwebp 1.6.0 | `4fa21912338357f89e4fd51cf2368325b59e9bd9` |
| curl 8.23.0 | `3163ec065643ba20709f190111f42c3e1fe95273` |

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for attribution and licenses.

## Validation and allocator decision

The CTest core suite covers crypto primitives, invalid ciphertext, numeric IDs,
filename handling, strip thresholds and remainder rows, WebP/JPEG/GIF decoding,
PNG pixel round trips, receipts, corruption detection, and configuration.
The local HTTP fixture verifies the real signing/encryption protocol, search
ordering and pagination, comic/chapters, login failure and cookie persistence,
favorites/folders, expired sessions, HTTP 429, corrupt image retries, progress,
cancellation, failed pages, and resume. It does not need a JMComic account. A pseudo-terminal smoke test
checks all five tabs, terminal resizing, and graceful quit.

Optional developer checks (these are test-binary options, not application CLI):

```sh
TMPDIR="$HOME/.cache/codex-tmp" "$jm_build/jmcomic_tests" --live
TMPDIR="$HOME/.cache/codex-tmp" "$jm_build/jmcomic_tests" --benchmark
```

The live check uses the configured default public hosts, requests all three
search orderings, loads a real comic and chapter, decodes and converts a page,
downloads the complete first chapter, and verifies restart/resume. Its scratch
data is deleted afterward. Live login and favorites require a usable account
and are **unverified against the service**; only the local fixture verifies them.

Mimalloc is not included. HTTP/JSON uses small allocations, but image processing
mostly uses a few large worker-owned buffers. Network latency and PNG encoding
are credible bottlenecks; no measured allocator contention currently justifies
another dependency or a global allocator override. The benchmark reports decode,
strip-copy, and PNG encoding times with three workers. It is a synthetic image
pipeline measurement, not a mimalloc A/B test. Reconsider an optional allocator
only if representative profiling demonstrates allocation/lock overhead.

On this aarch64 host (GCC 14.2, Release), six synthetic 1200×1800 pages with
three workers took 909 ms for the measured pipeline. Summed worker phase times
were 370 ms decoding, 64 ms strip copying, and 2286 ms PNG encoding (84% of
measured processing time); process peak RSS was 120544 KiB. This supports
prioritizing encoding and memory limits over changing the allocator. It does
not establish how much mimalloc would improve a different workload.

Validation on 2026-10-26: Release build and focused tests passed. The compiled
client returned 47 results for each search order, fetched comic/chapter 1480638,
converted a 1079×1500 WebP with 12 strips, downloaded all 22 chapter pages as
PNGs, and reused all 22 verified outputs in a new download manager. The single
page PNG decoded back to identical RGBA pixels. Live account workflows remain
unverified because no usable credentials were supplied.
