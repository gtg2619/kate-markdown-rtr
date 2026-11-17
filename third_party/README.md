# Vendored rendering libraries

## cmark-gfm

Unmodified upstream archive: **0.29.0.gfm.13**.

- Source: https://github.com/github/cmark-gfm/tree/0.29.0.gfm.13
- Download: https://codeload.github.com/github/cmark-gfm/tar.gz/refs/tags/0.29.0.gfm.13
- SHA-256: `5abc61798ebd9de5660bc076443c07abad2b8d15dbc11094a3a79644b8ad243a`
- License: [COPYING.cmark-gfm](COPYING.cmark-gfm), also inside the archive.

CMake verifies the archive and builds the parser plus GFM extensions statically.
No network download, package manager or external executable is used at runtime.
Update this archive, the CMake checksum and tests together when upgrading.

## MathJax

MathJax **3.2.2**, built from the pinned `mathjax-full` npm package using esbuild **0.28.2**.
This is a deliberately pinned v3 renderer, not a claim to track the latest MathJax release.

- Source: https://github.com/mathjax/MathJax-src/tree/3.2.2
- License: [Apache-2.0](mathjax/LICENSE), copied unchanged from the package.
- Bundle: `mathjax/engine.js`, SHA-256 `b55101a92b62955da09703a1594080fd00ab48e6799cf6963316a0c7b0d5e474`.
- Entry point and dependency integrity lock: [`tools/mathjax`](../tools/mathjax).
- Included TeX packages: base, ams, newcommand, boldsymbol, mathtools. SVG glyph paths are embedded;
  no font download, browser DOM, dynamic package loading or runtime Node process is required.

Reproduce the bundle (only maintainers need Node.js/npm and network access):

```sh
cd tools/mathjax
npm ci --ignore-scripts
npm run build
sha256sum ../../third_party/mathjax/engine.js
```

The wrapper is our integration code; upstream mathjax-full is not patched. CMake checks the
committed bundle hash and embeds it as a Qt resource. When upgrading, review the generated
bundle, licenses, package lock, checksum and rendering tests together.
