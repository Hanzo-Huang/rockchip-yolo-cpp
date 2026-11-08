# Project guidance

This is a Linux C++17 YOLO application for RK3576 and RK3588. The pipeline uses
FFmpeg for input, MPP for decoding, RGA for preprocessing, and RKNN for inference.

## Layout and build

- `src/engine/`: hardware pipeline shared by the CLI and channel worker.
- `src/worker/`: per-channel processing, playback control, and preview transport.
- `src/server/`: HTTP/WebSocket API, asset management, and persistent state.
- `web/`: React dashboard. `tools/`: setup, build, and packaging scripts.
- Build on a compatible board with `./nvr build`; use `./nvr build --cli-only`
  to skip the server and frontend. Use `./nvr cross-build` for the Docker build.
- Follow `.clang-format`: four spaces and Allman braces for C++.

## Code style and documentation

- Follow the user's formatting rules and the surrounding code. Keep code easy
  to read, with clear names and straightforward control flow.
- Separate logical sections and modules with blank lines. Add short comments
  where they help explain a section's purpose or a non-obvious decision.
- Do not add documentation files unless the user explicitly asks for them.
  Update this existing file when asked to remember project preferences.
- Edit the README only when needed. Use plain, natural language and concrete
  instructions. Avoid promotional language, filler, repetitive summaries, and
  generic AI-style wording.
- If unsure how to apply these writing preferences, consult relevant skills
  or search online for guidance before making speculative changes.

## Architecture and reuse

- Organize code around clear responsibilities. Keep reusable processing modules
  separate from channel orchestration, server APIs, storage, and the dashboard.
- Make useful modules easy to use from another application or project through
  small, explicit interfaces. Avoid dependencies on application configuration,
  global state, or server-specific types in reusable processing code.
- Keep buffer ownership, resource lifetime, threading, and error handling clear
  at module boundaries. Callers should not need to understand internal details
  to use a module correctly.
- Extract shared behavior when it has a clear purpose; avoid duplicate logic,
  unnecessary abstraction layers, and speculative extension points.
- Prefer a structure that is easy to read and modify. Hardware acceleration
  choices should follow measured needs; RGA-based postprocessing is an example
  of a possible reusable module, not a required feature.

## Hardware and concurrency

- Keep model exports compatible with the target chip and metadata.
- Automatic inference uses two independent contexts on RK3576 and three on
  RK3588. `inference_workers: 1` uses one context with the all-core mask.
- Each inference worker owns its RKNN context, input DMA buffer, preprocessor,
  and output storage. Do not share mutable inference buffers across threads.
- Preserve frame-order publication, bounded live queues, and offline lossless
  back-pressure. Playback must remain independent of inference.
- Join consumers and release imported buffers before destroying the MPP decoder.
- Keep performance-CPU affinity within the inherited allowed CPU set.
- RGA engine selection belongs to librga and the driver. Do not assume a fixed
  engine from the chip name; validate job and buffer compatibility on hardware.

## Validation and local state

- Validate hardware changes on the affected boards; a successful build alone
  does not establish performance or detection correctness.
- Compare the same source, model, and settings. Measure wall-clock inference
  throughput, queue drops, and latency; stream timestamps alone can mislead.
- For concurrency changes, check detection equivalence, result ordering,
  pause/resume, seek, error propagation, and shutdown.
- Track only what users need to build, configure, and run the application.
  Keep temporary model conversion, test, and benchmark scripts in the ignored
  `tmp/` folder. Do not commit those scripts or their outputs.
- `.local/state/` contains user settings, models, uploads, and recordings. Do not
  delete it when cleaning builds. Keep validation state and binaries separate
  from running installations, and do not replace drivers for routine builds.
