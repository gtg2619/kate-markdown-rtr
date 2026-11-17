# Changelog

## 0.2.1 — 2026-11-17

- Enable live rendering automatically for Markdown views; retain manual opt-out while a view is open.
- Redirect restored editor focus to the live surface so typing cannot modify the covered source view.
- Cache image resources in each rendered document so scrolling and repainting do not consume
  the image budget repeatedly and replace valid images with an unavailable placeholder.
- Use Ctrl+Enter to finish a block and Ctrl+Shift+M to toggle RTR. Leave Esc to Kate panels and dialogs.
- Add regressions for repeated image painting, automatic activation and host Escape shortcuts.

## 0.2.0 — 2026-11-17

Initial public release of Kate Markdown RTR.

- Edit Markdown blocks in Kate's existing document view, preserving source, save and undo behavior.
- CommonMark/GFM rendering with bundled cmark-gfm; reuse Kate syntax highlighting and editor theme.
- Local and optional asynchronous HTTP(S) images; paste screenshots into adjacent asset files.
- Inline and display LaTeX through bundled MathJax and native Qt SVG rendering.
- Bounded caches and queues, malformed-input fallback, and splitter shrink regression coverage.
- Forty functional test cases/data rows, sanitizer validation and a real Kate GUI smoke check.

See README for supported interactions, build dependencies and limits.
