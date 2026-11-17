# Test results

Environment: Fedora 45, Kate 26.08.2, Qt 6.11.2, KDE Frameworks 6.31 and GCC 16.2.
Sanitizer builds use Clang 23.1.5. Recorded on 2026-11-17 for version 0.3.0.

## Automated tests

Three Qt Test executables cover 101 functional cases/data rows, excluding setup and cleanup.
Together with ECM's metadata check, CTest runs four suites. Tests use isolated XDG directories
and Qt's offscreen platform. HTTP tests use a loopback server, without external requests.

The release and ASan + UBSan + LeakSanitizer builds pass all four suites, without suppressions
or skipped cases. The coverage includes:

- Continuous editing of rendered Markdown, incomplete delimiters, nested emphasis, links,
  escaped characters, HTML entities, Unicode and inline/fenced/indented code.
- Exact source edits through formatting boundaries, repeated visible words, GFM table escapes,
  empty and automatically padded table cells, headings, lists and paragraph breaks.
- Preserving formatting during selection replacement and repairing partially selected wrappers.
- Native click, Shift+click and select-all behavior without source expansion. Forty scroll/click
  cycles across a long document retain document geometry, scroll range and parse count.
- Repeated clicks, double-clicks, Shift selections, arrow/Home/End navigation and programmatic
  caret positioning beside tall images retain the exact vertical scroll offset. Explicit wheel
  scrolling remains functional.
- Incremental word edits: 600 insertions/deletions in a 1000-block document without a full parse.
- IME preedit and commit, non-breaking spaces, emoji, read-only mode, external changes and
  synchronous formatter callbacks.
- Real KTextEditor undo/redo, saving, automatic activation, manual opt-out, focus redirection,
  plugin loading/unloading and editor split shrinking.
- Host Escape shortcuts remain available while RTR is focused.
- Theme/font inheritance and rendered code layout after applying Kate syntax highlighting.
- Image paste with atomic asset saving, relative and Unicode paths, undo retaining assets,
  repeated painting, missing/corrupt/oversized files and bounded decoding/cache use.
- Optional remote images, cancellation/retry, bounded queues and asynchronous shutdown.
- Twenty inline/display MathJax conversions, SVG validity, rendered objects, font sizing and
  text color. Invalid/recursive formulas, macro isolation, queue limits and background teardown.
- Markdown parser partition checks, seeded malformed input and bounded math scanning.

The final release run passed in 5.68 seconds; the sanitizer run passed in 13.45 seconds.
The 600-edit test completed in about 0.5 seconds in Release and 2.4 seconds with sanitizers.
Detailed results are included with the release artifacts. They are measurements on this
machine, not cross-machine guarantees. Broad latency ceilings catch regressions; source
preservation and parse-count assertions verify the incremental path independently of timing.

AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer configuration:

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_MODULE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-asan --parallel 4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure
```

## Real Kate smoke check

Load the stripped release plugin into an isolated D-Bus/Xvfb Kate session. Open
[demo.md](demo.md), let RTR activate automatically, position the caret in its rendered heading,
type, save with Ctrl+S and compare the file byte for byte with the expected Markdown edit.
Select the heading with Shift+End and verify that its layout remains rendered.

Inspect highlighted Python code, table equations, inline and display math, and the relative SVG
image. Capture the complete Kate window: [editing surface](real-kate.png) and
[native text selection](real-kate-edit.png). Exit Kate cleanly. Separate XDG directories leave
existing user sessions unchanged.

The binary release targets the environment listed above; rebuild from source on incompatible
Qt/KF versions. README documents supported syntax, atomic image/math editing and resource limits.
