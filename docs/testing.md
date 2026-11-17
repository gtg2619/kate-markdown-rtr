# Test results

Environment: Fedora 45, Kate 26.08.2, Qt 6.11.2, KDE Frameworks 6.31 and GCC 16.2.
Sanitizer builds use Clang 23.1.5. Recorded on 2026-11-17.

## Automated tests

`scripts/test.sh` runs three Qt Test executables with 43 passing functional cases/data rows,
excluding setup and cleanup. CTest also runs ECM's metadata check, making 4 suites in total.
Tests use isolated XDG directories and Qt's offscreen platform, leaving user Kate sessions
untouched. HTTP image tests use a loopback mock server without external network requests.

Local 0.2.1 results: normal build 4/4 passed in approximately 4.2 seconds;
ASan + UBSan + LeakSanitizer build 4/4 passed in approximately 8.1 seconds,
with no suppressions or skipped cases.

- CommonMark/GFM headings, emphasis, strikethrough, tables, tasks, reference links and code languages.
- Source partitions including invisible definitions and whitespace; 300 seeded random Markdown inputs.
- Minimal UTF-16 edits, CJK/emoji, non-breaking spaces and Unicode line/paragraph separators.
- Input-method preedit stays out of the source; only committed text is inserted.
- Kate undo, cross-block deletion, select-all replacement, read-only changes, external edits
  from another view and synchronous formatter callbacks.
- Native plugin factory loading, saving real documents, toggling, resizing and unloading.
- Font/palette inheritance, the 48-layout cache limit and no full parse on each keystroke.
- Repeated splitter enlargement/shrinking without native child surfaces intercepting input.
- Fourteen math delimiter cases: Unicode, escapes, currency, code, links, lists, quotes,
  tables and placeholder collisions.
- Twenty inline/display formula conversions: fractions, roots, integrals, sums, matrices,
  aligned expressions, cases and math fonts. Validate SVG, nonempty pixels, embedded objects,
  DPI, font size and text color.
- Invalid formulas, recursive macros, macro isolation, oversized input, bounded queues and
  destruction while a background task is pending.
- Dense unclosed delimiters and long backslash runs have bounded scan work and preserve source partitions.
- Pasted image files, relative paths, undo retaining assets, Unicode filenames, scaling,
  missing/corrupt/oversized images.
- Asynchronous image downloads, default-disabled remote access, cancellation/retry and queue cleanup.
- Repeated image resource requests and painting retain the image instead of charging the
  per-block byte limit on every repaint. This regression failed before the cache fix.
- Automatic activation for existing/new Markdown views and Save As, with manual opt-out
  preserved across tab switches and filename changes. Focus restored by Kate is transferred
  to RTR, preventing input from reaching the covered source editor.
- Esc remains available to a host panel-close action in both rendered and block-editing modes;
  Ctrl+Enter finishes a block without inserting a newline.

A local RelWithDebInfo run parsed 82,890 UTF-16 units / 4000 blocks in 9 ms.
Three hundred source-preserving edits took approximately 0.33 seconds, including KTextEditor
updates and assertions. The twenty-formula test took about 1.23 seconds, including engine
cold start and Qt Test polling delays; it is not a pure conversion benchmark.
These are observations on this machine, not cross-machine performance guarantees. Broad
regression ceilings catch severe slowdowns without treating ordinary CI timing variation as failure.

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

Load the built plugin into an isolated D-Bus/Xvfb Kate session, enable it and send actual
keyboard events: automatic live rendering → Enter → edit the heading → Ctrl+S → Ctrl+Enter.
Use [demo.md](demo.md) with a relative SVG image and compare the saved file byte for byte
against the expected edit. Inspect table equations, code highlighting, inline math, display
`aligned` math and the local image. Capture the entire Kate window:
[rendered view](real-kate.png) and [in-place source editing](real-kate-edit.png).
Separate XDG directories keep the user's existing session unchanged.

Repeat loading, editing, saving and exiting using the stripped release plugin installed into
a temporary prefix. The binary release targets the Qt/KF environment listed above;
rebuild from source on other platforms or incompatible versions.

This does not certify every Typora interaction or every distribution. See README for limits
on complete LaTeX, Mermaid, cross-block drag selections and remote image loading.
