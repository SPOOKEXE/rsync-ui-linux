# rsync-ui-linux

Linux rsync user interface. A single Dear ImGui window that queues rsync copies
one after another and shows live progress.

## What it does

- **Sources** — a list of files or directories, each with its own `rec` checkbox.
  Off means "copy this folder's own files, skip its subdirectories".
- **Destinations** — a list of target directories. Every source is copied into
  every destination, so N sources and M destinations produce N x M jobs.
- **Drop folder** — open a directory, tick `drops copy straight here`, and
  anything dragged onto the window is queued into that folder immediately.
  With the tick off, drops are added to the source list instead.
- **Queue** — jobs run one at a time with a progress bar, speed and ETA.
  A failed job goes red and the queue moves on. Hover a source cell to see the
  exact rsync command, and the error if it failed.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/rsync-ui
```

CMake fetches Dear ImGui and GLFW at configure time, so the first build needs
network access. Building GLFW from source needs the X11 development headers
(`sudo apt install xorg-dev` on Debian/Ubuntu). `rsync` must be on `PATH` at
runtime.

## Notes

- rsync is launched with `fork` + `execvp`, never through a shell, so paths with
  spaces, quotes or `$` need no escaping.
- Jobs run with `--info=progress2 --no-inc-recursive`. The second flag makes the
  percentage monotonic; without it rsync keeps discovering files mid-transfer and
  the bar slides backwards.
- Source paths are passed without a trailing slash, so copying `/a/photos` into
  `/b` lands at `/b/photos` rather than dumping the contents into `/b`.
- `--delete` removes files from the destination and is behind a confirmation.
  Try a dry run first.
