# Development

This document covers build-environment setup, compilation, testing, local execution, and packaging.
See [ARCHITECTURE.md](ARCHITECTURE.md) for component ownership and runtime interactions, and
[DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) for the current engineering roadmap.

## Supported development environment

Javelin is currently developed and tested for KDE Plasma on Linux. The installed runtime expects a
systemd user session for automatic daemon startup and uses Plasma's StatusNotifierItem host, desktop
notification service, icon theme, and KDE resource lookup conventions. Other Linux desktops are a
best-effort compatibility target rather than the environment that drives design decisions.

The codebase targets:

- C++23
- CMake 3.25 or newer for the supplied presets
- Ninja
- Qt 6.6 or newer
- KDE Frameworks 6

Both GCC and Clang should work, but the normal repository workflow uses the compiler selected by the
host CMake environment.

## Dependencies

Required Qt components are Core, DBus, Network, SQL, Widgets, Concurrent, LinguistTools, WebEngine,
SVG, and WebSockets. Required KDE components are ConfigWidgets, XmlGui, CoreAddons, TextEditor,
Extra CMake Modules, and KPim6Mime. These are product dependencies, not merely build conveniences:
KXMLGUI owns the main-window action layout, KConfigWidgets owns preferences presentation,
KTextEditor powers compose/source and Sieve editing, and the daemon publishes a KDE
StatusNotifierItem directly over D-Bus.

CMake uses installed QCoro, Glaze, and Catch2 packages when available. Otherwise it fetches the
versions pinned in `cmake/Dependencies.cmake`. fastText is fetched when local language detection is
enabled; its compact language model is already included in the repository.

### Arch Linux

A suitable base environment can be installed with:

```sh
sudo pacman -S --needed \
  base-devel ccache cmake extra-cmake-modules git ninja \
  qt6-base qt6-svg qt6-tools qt6-webengine qt6-websockets \
  kconfigwidgets kcoreaddons ktexteditor kxmlgui kmime
```

Installed `qcoro`, `glaze`, and `catch2` packages are optional because CMake can fetch them. A fresh
configuration therefore needs network access unless all fallback dependencies are already available.

For formatting and optional analysis, also install the appropriate tools:

```sh
sudo pacman -S --needed clang
```

Install `clazy` when using the clazy targets.

## CMake presets

The repository provides these primary configure presets:

| Preset | Purpose |
| --- | --- |
| `debug` | Normal development and test build |
| `asan` | Debug build with AddressSanitizer and UndefinedBehaviorSanitizer |
| `release` | Optimized build suitable for installation or packaging |

Build trees are created under `out/build/<preset>`. Local development data is installed under
`out/install/<preset>` by default so KXMLGUI resources, icons, and the language model are available
without a system install.

The `debug` and `asan` presets use `ccache` as the C++ compiler launcher. They store cached objects
in `/tmp/javelin-mail-ccache-$USER`, which is writable from sandboxed development agents and shared
by their separate worktrees. Configure a suitable per-user limit once, for example with
`CCACHE_DIR="/tmp/javelin-mail-ccache-$USER" ccache --max-size 20G`, and inspect effectiveness with
`CCACHE_DIR="/tmp/javelin-mail-ccache-$USER" ccache --show-stats`. The operating system may clear
this cache on reboot. Debug, sanitizer, and compiler-option differences intentionally produce
distinct cache entries.

Never run multiple CMake or Ninja processes against the same binary directory. For work performed
in the shared repository workspace, use `scripts/check-debug.sh`; it serializes configuration,
compilation, and tests with a host-wide lock. Truly concurrent agents should instead use isolated
worktrees with their own build directories. Their shared `ccache` makes those isolated builds
inexpensive after the cache has warmed.

## Debug build

```sh
cmake --preset debug
cmake --build --preset debug
```

For a focused build and test run in the shared workspace, use:

```sh
scripts/check-debug.sh --target javelin_jmap_tests --tests 'SessionClient'
```

Repeat `--target` to build more than one target. Run the complete configure, build, test, and format
workflow only for final verification:

```sh
scripts/check-debug.sh --full
```

The convenience target builds both processes and launches the GUI:

```sh
make run
```

`make run` sources the generated build-tree prefix before launching `javelin`. When no daemon is
running, the GUI recovery window can start the matching build-tree `javelind` process. This target
also sets `JAVELIN_FORWARD_DAEMON_STDIO=1`, so a daemon started by the recovery window keeps the
terminal's standard output and error streams. Normal desktop and installed launches continue to
detach the daemon from terminal output.

Additional GUI arguments can be passed with:

```sh
make run RUN_ARGS="--help"
```

## Running isolated development processes

An installed daemon and a development daemon normally use the same runtime socket. For an isolated
session, provide a private runtime directory to both executables:

```sh
runtime_dir="$(mktemp -d)"
chmod 700 "$runtime_dir"

. out/build/debug/prefix.sh
out/build/debug/bin/javelind --runtime-directory "$runtime_dir" &
out/build/debug/bin/javelin --runtime-directory "$runtime_dir"
```

Both executables also accept `--socket <path>`. The socket path must remain inside the selected
private runtime directory.

The GUI does not silently enter a daemon-free mode. If the daemon is absent or incompatible, it
shows the recovery surface instead of opening a partially functional application.

## Release build and system installation

```sh
cmake --preset release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DJAVELIN_ENABLE_LOCAL_DATA_INSTALL=OFF
cmake --build --preset release
sudo cmake --install out/build/release
```

Then load and enable the installed user service:

```sh
systemctl --user daemon-reload
systemctl --user enable --now javelind.service
```

The installation includes:

- `javelin`, the Qt Widgets/WebEngine GUI
- `javelind`, the background daemon
- `javelind.service`, a systemd user unit
- the desktop file, icon, KXMLGUI resource, fastText model, and third-party notices

## Arch package build

The repository contains a development `PKGBUILD` that builds the current checkout:

```sh
makepkg -si
```

The package version is derived from the CMake project version and current Git revision. After
installation, reload and enable the user service as shown above.

## Continuous integration and package artifacts

GitHub Actions runs two test configurations for every pull request and every push to `master`:

- the complete Debug build, Catch2 suite, and repository formatting check; and
- the same test suite under AddressSanitizer and UndefinedBehaviorSanitizer.

A separate packaging workflow produces downloadable CI artifacts:

- an Arch Linux `javelin-mail-git` package built through the repository `PKGBUILD`;
- a Flatpak bundle based on `org.kde.Platform//6.9`; and
- an x86-64 AppImage containing both `javelin` and `javelind`.

The Flatpak and AppImage deliberately omit the host systemd unit. The GUI launches the adjacent
daemon in those portable environments. The AppImage launcher remains alive while the daemon is
running so its mounted KDE resources, QtWebEngine helper, icons, and language model remain available
after the GUI window closes.

The Flatpak manifest is
[`packaging/flatpak/app.javelin.JavelinMail.yml`](../packaging/flatpak/app.javelin.JavelinMail.yml).
A local bundle can be built with:

```sh
flatpak-builder --force-clean --install-deps-from=flathub \
  --repo=flatpak-repo flatpak-build \
  packaging/flatpak/app.javelin.JavelinMail.yml
flatpak build-bundle flatpak-repo Javelin-Mail.flatpak app.javelin.JavelinMail
```

## Tests and formatting

Qt test discovery starts Qt and needs a valid private runtime directory. This matters especially in
remote, headless, or tool-managed sessions:

```sh
install -d -m 700 /tmp/javelin-mail-xdg-runtime
export XDG_RUNTIME_DIR=/tmp/javelin-mail-xdg-runtime
```

The standard repository check configures, builds, runs all tests, and checks formatting:

```sh
cmake --workflow --preset debug-check
```

Individual steps are:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --test-dir out/build/debug --output-on-failure
cmake --build --preset debug-format-check
```

Sanitizer validation uses:

```sh
cmake --workflow --preset asan-check
```

The project uses Catch2 for deterministic unit and integration tests. Network-dependent behavior
should normally be exercised through scripted transports and fixtures rather than live accounts.

## Static analysis

Enable clang-tidy during compilation with:

```sh
cmake --preset debug -DJAVELIN_ENABLE_CLANG_TIDY=ON
cmake --build --preset debug
```

Enable per-target clazy targets with:

```sh
cmake --preset debug -DJAVELIN_ENABLE_CLAZY=ON
cmake --build --preset debug --target <target>_clazy
```

The repository-wide C++ formatter targets are `format` and `format-check`. Do not run clang-format
on CMake, Markdown, vendored files, or other non-C++ sources.

### Qt widget stylesheet subcontrols

Qt complex widgets draw their subcontrols on top of the widget rule. When a stylesheet adds a
rounded border to a `QComboBox`, it must also style `QComboBox::drop-down` with the same right-hand
corner radii and an intentional background/border, plus provide an explicit
`QComboBox::down-arrow` image. Otherwise the native drop-down subcontrol can paint a square or
gradient block over the widget’s rounded right edge, or the arrow can disappear entirely. Before
adding or changing a widget stylesheet, search for its `::` subcontrols and keep their geometry
consistent with the parent widget. See Qt’s [QComboBox stylesheet example](https://doc.qt.io/qt-6/stylesheet-examples.html#customizing-qcombobox).

## Useful build options

| Option | Default | Purpose |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` in normal preset use | Build the test suite |
| `JAVELIN_ENABLE_LOCAL_DATA_INSTALL` | `ON` | Copy application resources into the active build install prefix |
| `JAVELIN_ENABLE_FASTTEXT_LANGUAGE_DETECTION` | `ON` | Build local fastText language detection |
| `JAVELIN_ENABLE_CLANG_TIDY` | `OFF` | Run clang-tidy as part of compilation |
| `JAVELIN_ENABLE_CLAZY` | `OFF` | Generate clazy targets when available |
| `JAVELIN_INSTALL_SYSTEMD_USER_SERVICE` | `ON` | Install `javelind.service`; portable bundles disable this |

The sanitizer options are set by the `asan` preset.

## Diagnostics

Set `JAVELIN_UI_PROFILING=1` before starting both processes to enable split-process performance
metrics:

```sh
JAVELIN_UI_PROFILING=1 make run 2>javelin-performance.log
```

See [DIAGNOSTICS.md](DIAGNOSTICS.md) for the metric format and measurement workflow.

The repository also builds these diagnostic tools:

- `jmap-query` — send configured diagnostic JMAP requests
- `jmap-transport-benchmark` — inspect method-transport behavior
- `javelin-undo-live-check` — exercise live Undo/Redo workflows

See [JMAP_QUERY_TOOL.md](JMAP_QUERY_TOOL.md) and
[LIVE_UNDO_REDO_CHECK.md](LIVE_UNDO_REDO_CHECK.md).

## Source and contribution workflow

Read the repository `AGENTS.md` before changing code. In particular:

- use the Debug preset for normal development validation;
- keep protocol, daemon coordination, cache, and GUI responsibilities within their documented
  boundaries;
- add deterministic tests for new state transitions and regressions;
- update documentation when a workflow or architectural invariant changes; and
- commit meaningful changes in focused logical commits.

The high-level source map and representative data flows are in [ARCHITECTURE.md](ARCHITECTURE.md).
The detailed implementation priorities are maintained separately in
[DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md).
