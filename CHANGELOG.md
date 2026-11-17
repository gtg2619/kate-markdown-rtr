# Changelog

## 0.2.0 — 2026-11-17

Initial public release of Kate Markdown RTR.

- Edit Markdown blocks in Kate's existing document view, preserving source, save and undo behavior.
- CommonMark/GFM rendering with bundled cmark-gfm; reuse Kate syntax highlighting and editor theme.
- Local and optional asynchronous HTTP(S) images; paste screenshots into adjacent asset files.
- Inline and display LaTeX through bundled MathJax and native Qt SVG rendering.
- Bounded caches and queues, malformed-input fallback, and splitter shrink regression coverage.
- Forty functional test cases/data rows, sanitizer validation and a real Kate GUI smoke check.

See README for supported interactions, build dependencies and limits.
