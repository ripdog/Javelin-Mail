<p align="center">
  <img src="res/icon.svg" width="128" height="128" alt="Javelin Mail icon">
</p>

<h1 align="center">Javelin Mail</h1>

<p align="center">
  A modern, native desktop client for JMAP mail, contacts, calendars, submission, and Sieve.
</p>

Javelin Mail is a Qt 6/C++23 desktop email client built around JMAP rather than IMAP and SMTP. It
uses a conventional three-pane mail interface, but its runtime is split into a lightweight background
daemon and an on-demand GUI: synchronization, push, notifications, delayed send, and long-running
work continue after the window closes, while Widgets and WebEngine leave memory when the GUI exits.

> **Project status:** active pre-1.0 development. The main mail client is usable and the daemon/GUI
> split is implemented, but release hardening, broader crash/reconnect testing, calendar work, and
> packaging polish are still in progress. Linux with a systemd user session and a
> StatusNotifierItem-compatible desktop is the current first-class environment.

## Highlights

### Mail

- Multiple JMAP account configurations and account-qualified mailbox tabs
- Threaded mailbox and search views backed by authoritative cached query windows
- Archive, move, copy, delete, permanent delete, star, read, and unread operations
- Optimistic UI updates with server acceptance, rejection, and ambiguous-result handling
- Advanced search combining cached/local indexes with server JMAP search
- HTML and plain-text message rendering, source viewing, attachments, and inline MIME content
- Remote-content controls with remembered sender/domain permissions
- Stable selection and navigation across refreshes, new mail, pagination, and mutations

### Compose and submission

- Rich HTML and plain-text composition
- HTML source, preview, and plain-text views
- Reply, Reply All, Forward, and edit-draft workflows
- Multiple sender identities, Cc/Bcc, file attachments, and embedded images
- Server-backed drafts with revision fencing
- Delayed send and daemon-owned Undo Send
- General Undo/Redo history for supported mail, draft, contact, calendar, address-book, and Sieve
  operations

### Contacts and calendars

- RFC 9610 JSContact address books, contacts, organizations, and groups
- Contact photos, starred contacts, group membership, sharing controls, and writable-rights checks
- vCard 4.0 import/export and high-confidence duplicate detection/merge
- JMAP Calendars support pinned to `draft-ietf-jmap-calendars-26`
- JSCalendar month view, recurring events, calendar creation/management, colours, and reminders

Calendar support is still being hardened. See
[`docs/CALENDAR_IMPLEMENTATION_PLAN.md`](docs/CALENDAR_IMPLEMENTATION_PLAN.md) before relying on it
for complex recurrence editing or interoperability-critical workflows.

### Background and offline operation

- `javelind` continues synchronization, push, notifications, delayed send, and maintenance without a
  GUI process
- RFC 8887 JMAP-over-WebSocket method transport and push when advertised by the server
- Safe HTTP and EventSource fallback without replaying uncertain mutations
- Daemon-owned desktop notifications and StatusNotifierItem tray controls
- Explicit complete-offline mailbox mirrors, including raw RFC 5322/MIME sources and attachments
- Resumable background jobs with Task Center progress, pause, resume, and retry
- Rebuildable per-account FTS5 search indexes

### Translation and appearance

- Local fastText language detection
- Optional Google Translate integration with configurable target language and API-key override
- Per-sender and per-domain automatic translation choices
- HTML message colours that follow the application, remain original, or are transformed for dark
  display

Translation is opt-in. Message text selected for translation is sent to Google Translate.

## Requirements

### Server

Javelin requires a JMAP server. Core and Mail capabilities are required for mail access. Additional
features depend on the corresponding advertised capabilities:

| Feature | JMAP capability/specification |
| --- | --- |
| Mail | RFC 8620 and RFC 8621 |
| Submission | `urn:ietf:params:jmap:submission` |
| Contacts | RFC 9610 |
| Calendars | `draft-ietf-jmap-calendars-26` and RFC 8984 |
| Sieve | JMAP Sieve capability |
| WebSocket transport/push | RFC 8887 |

The account UI currently accepts a static bearer API key/access token. Interactive OAuth login and
automatic product-level token refresh are not yet exposed.

### Build environment

- A C++23 compiler
- CMake 3.25 or newer for the supplied presets
- Ninja
- Qt 6.6 or newer with Core, DBus, Network, SQL, Widgets, Concurrent, LinguistTools, WebEngine,
  SVG, and WebSockets
- KDE Frameworks 6: ConfigWidgets, XmlGui, CoreAddons, and TextEditor
- KPim6Mime
- Extra CMake Modules
- SQLite with FTS5 support

CMake uses installed QCoro, Glaze, and Catch2 packages when available and otherwise fetches the
pinned versions. fastText is fetched when local language detection is enabled; the compact language
model is included under `res/models/fasttext/`.

## Building

### Arch Linux package

The repository contains a development `PKGBUILD`:

```sh
makepkg -si
systemctl --user daemon-reload
systemctl --user enable --now javelind.service
javelin
```

### Generic CMake install

For a conventional system installation:

```sh
cmake --preset release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DJAVELIN_ENABLE_LOCAL_DATA_INSTALL=OFF
cmake --build --preset release
sudo cmake --install out/build/release

systemctl --user daemon-reload
systemctl --user enable --now javelind.service
javelin
```

The install provides:

- `javelin` — the Qt Widgets/WebEngine GUI
- `javelind` — the background service
- `javelind.service` — a systemd user unit
- the desktop file, application icon, KXMLGUI definition, fastText model, and third-party notices

### Development build

```sh
cmake --preset debug
cmake --build --preset debug
make run
```

`make run` builds both executables and launches the GUI with the build-tree data prefix. When no
daemon is running, the recovery window offers **Start Javelin now**. An installed systemd unit also
enables **Enable background sync**, which starts the daemon now and on future graphical sessions.

To run the two processes manually:

```sh
. out/build/debug/prefix.sh
out/build/debug/bin/javelind &
out/build/debug/bin/javelin
```

Both executables accept `--runtime-directory <directory>` and `--socket <path>` for isolated test or
development sessions. The default command socket is `$XDG_RUNTIME_DIR/javelind.sock`; the activation
socket is adjacent to it.

## First-run setup

1. Start `javelind`, then launch `javelin`.
2. Open **Settings → Configure…**.
3. On **Accounts**, select **Add** and enter:
   - **Display Name** — a local label for the connection;
   - **Login Email** — the account login/address;
   - **API Key** — the server-issued bearer token or API key; and
   - **Server** — an optional JMAP Session URL or server base URL.
4. Leave **Server** empty to discover `/.well-known/jmap` from the login email domain.
5. Apply the settings. The daemon discovers the Session resource, capabilities, accounts,
   mailboxes, and identities, then refreshes the cache.
6. Open **Mailbox Sync** to choose:
   - mailboxes that should have a complete offline copy; and
   - mailboxes that should produce desktop notifications.

The account needs Mail capability to browse messages. Sending additionally needs Submission and at
least one server identity. Contacts, calendars, and Sieve appear only when the server exposes usable
capabilities and rights.

## Everyday use

### Process lifecycle

Closing the final Javelin window exits the GUI process. `javelind` remains running, keeps the cache
current, publishes notifications, executes delayed sends, and retains the tray menu.

Launching `javelin` again reconnects to the existing daemon and restores the saved workspace. A
second GUI launch raises the already-running GUI instead of creating another process. **Quit** from
the daemon tray intentionally shuts down both the background service and any connected GUI.

Useful service commands:

```sh
systemctl --user status javelind.service
systemctl --user restart javelind.service
journalctl --user -u javelind.service -f
```

### Mail and search

Select a mailbox in the left pane to open or reuse its tab. The list is paginated through cached
JMAP query windows rather than loading an entire mailbox into memory. Search tabs retain their query,
sort order, page, and selection in the saved workspace.

The toolbar and **Message** menu expose reply, forward, source, archive, unread, delete, move, and
copy actions. `Ctrl+Z` and `Ctrl+Y` route Undo and Redo to the daemon, which rejects stale history
heads rather than executing a different operation than the one shown by the GUI.

### Offline mail

In **Preferences → Mailbox Sync**, check **Keep complete offline copy** for mailboxes that should be
fully mirrored. Javelin downloads all metadata, raw messages, and attachments into its filesystem
vault. Large jobs continue in the background and appear in Task Center; foreground navigation and
commands take priority.

Unchecking a mailbox stops treating it as a required mirror. Existing downloaded content becomes
ordinary removable cache rather than being immediately erased.

### Notifications and remote content

The notification mailbox list is independent of complete-offline selection. Notifications are
owned by the daemon and can arrive while the GUI is closed. Activating a mail notification starts or
raises the GUI and routes to the stable account, mailbox, thread, and message identity.

Remote HTML resources are controlled separately. Permissions granted for a sender or domain can be
reviewed and removed under **Preferences → Remote Content**.

### Compose and Undo Send

The compose tab can switch between HTML and plain text. HTML mode provides rich editing plus source,
preview, and generated plain-text tabs. Attachments are copied into daemon-owned immutable staging
before a save or send is accepted, preventing later source-file replacement from changing the
submitted attachment.

The **Composing** preferences page controls the Undo Send window from 1 to 120 seconds. During that
window the daemon retains the deferred submission even when the GUI closes; once submission begins,
the history entry expires.

### Contacts, calendars, Sieve, and tasks

Use the **Account** menu or toolbar to open Contacts, Calendar, the Sieve editor, advanced search, or
a server refresh. The status-bar task summary opens Task Center, which shows resumable offline-sync,
indexing, prefetch, and maintenance jobs.

## Preferences

The current preferences pages include:

- **Accounts** — connection labels, login email, Session URL discovery, and API key
- **Mailbox Sync** — complete-offline and notification mailbox selections
- **Remote Content** — remembered sender/domain permissions
- **Appearance** — HTML message colour behavior
- **Translation** — enablement, target language, API-key override, and auto-translate entries
- **Attachments** — prompt for a destination or save to a fixed directory
- **Composing** — Undo Send delay

Settings are owned and persisted by `javelind`. The GUI receives a revisioned snapshot and submits a
revision-checked aggregate update, so an old Preferences window cannot silently overwrite newer
settings.

## Data and recovery model

Javelin uses Qt standard per-user locations:

- daemon-owned `QSettings` for accounts, credentials, preferences, and workspace state;
- `QStandardPaths::AppLocalDataLocation` for `cache.sqlite3`, `mail-vault/v1`, per-account `indexes/`,
  and the cache-instance identity; and
- `$XDG_RUNTIME_DIR` for private local sockets and the single-GUI lock.

The server remains authoritative for mail, drafts, contacts, and calendars. The SQLite cache, MIME
vault, indexes, mutation projections, notification state, task checkpoints, and history improve
speed and ordinary crash recovery, but the cache is designed to be replaceable and rebuildable.
Destroying it can lose recent local-only operational state such as Undo history or a delayed-send
schedule even though server data remains recoverable.

Account API keys are currently stored in the daemon's native `QSettings` store rather than a desktop
secret service. Protect the user profile accordingly; secret-service integration is planned before
broad end-user release.

## Architecture

```text
                                 private local JVIP socket
┌────────────────────────────┐  commands / replies / invalidations  ┌───────────────────────────┐
│ javelin                    │ <───────────────────────────────────> │ javelind                  │
│ Widgets + WebEngine        │                                       │ application coordination  │
│ read-only cache models     │                                       │ JMAP, sync, mutations      │
│ editing and navigation     │                                       │ settings, tray, background │
└──────────────┬─────────────┘                                       └─────────────┬─────────────┘
               │ reads                                                             │ writes
               └──────────────────────────┬─────────────────────────────────────────┘
                                          ▼
                             SQLite cache and MIME vault
                                          │
                                          ▼
                                      JMAP server
```

The central invariants are:

1. `javelind` is the sole operational authority and the sole SQLite/settings writer.
2. `javelin` sends typed commands and renders committed cache state through read-only connections.
3. Cache commits precede invalidations; invalidations are hints, not object state.
4. Server snapshots are rebased with active optimistic projections before becoming visible.
5. Cache changes never clobber selection, navigation, viewport, focus, or newer editor intent.
6. Transport ambiguity is classified as unknown, never guessed to be success or rejection.
7. Foreground work may pass queued background work but never interrupts an atomic transaction.

The CMake graph enforces these boundaries: GUI targets cannot link daemon/JMAP implementations or
access canonical `QSettings`, and daemon targets cannot link Widgets or WebEngine.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and
[`docs/DAEMON_GUI_ARCHITECTURE.md`](docs/DAEMON_GUI_ARCHITECTURE.md) for the complete design.

## Testing and quality checks

Qt test discovery needs a private runtime directory in headless or remote sessions:

```sh
install -d -m 700 /tmp/javelin-mail-xdg-runtime
export XDG_RUNTIME_DIR=/tmp/javelin-mail-xdg-runtime
```

Run the standard Debug build, tests, and formatting check with:

```sh
cmake --workflow --preset debug-check
```

Sanitizers:

```sh
cmake --workflow --preset asan-check
```

Individual commands:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --test-dir out/build/debug --output-on-failure
cmake --build --preset debug-format-check
```

The repository also provides:

- `jmap-query` — issue configured diagnostic JMAP requests;
- `jmap-transport-benchmark` — compare method-transport behavior; and
- `javelin-undo-live-check` — exercise live Undo/Redo workflows.

See [`docs/JMAP_QUERY_TOOL.md`](docs/JMAP_QUERY_TOOL.md),
[`docs/LIVE_UNDO_REDO_CHECK.md`](docs/LIVE_UNDO_REDO_CHECK.md), and
[`docs/DIAGNOSTICS.md`](docs/DIAGNOSTICS.md).

## Performance diagnostics

Set `JAVELIN_UI_PROFILING=1` before starting both processes to emit opt-in machine-readable metrics
through normal Qt logging:

```sh
JAVELIN_UI_PROFILING=1 make run 2>javelin-performance.log
```

Metrics cover GUI event-loop stalls and end-to-end remote actions, daemon execution and admission,
work-scheduler contention, RSS/CPU samples, and WAL size. Profiling is disabled by default and does
not persist telemetry in SQLite.

## Documentation

| Document | Purpose |
| --- | --- |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Current code and target boundaries |
| [`docs/DAEMON_GUI_ARCHITECTURE.md`](docs/DAEMON_GUI_ARCHITECTURE.md) | Authoritative process split and IPC design |
| [`docs/DAEMON_GUI_IMPLEMENTATION_PLAN.md`](docs/DAEMON_GUI_IMPLEMENTATION_PLAN.md) | Split implementation history and release gate |
| [`docs/OPTIMISTIC_CONSISTENCY.md`](docs/OPTIMISTIC_CONSISTENCY.md) | Mutation projection, reconciliation, and ambiguity |
| [`docs/OFFLINE_MAIL_ARCHITECTURE.md`](docs/OFFLINE_MAIL_ARCHITECTURE.md) | Complete-offline mirrors, vault, tasks, and indexes |
| [`docs/QUERY_WINDOWS.md`](docs/QUERY_WINDOWS.md) | Pagination and authoritative query membership |
| [`docs/DATABASE_ACCESS.md`](docs/DATABASE_ACCESS.md) | SQLite connection, transaction, and process ownership |
| [`docs/UNDO_REDO.md`](docs/UNDO_REDO.md) | History ownership, preconditions, and deferred send |
| [`docs/RENDERING.md`](docs/RENDERING.md) | Message rendering and dark appearance |
| [`docs/DEVELOPMENT_PLAN.md`](docs/DEVELOPMENT_PLAN.md) | Current product and engineering roadmap |

Normative JMAP and related specifications used by the implementation are vendored under `specs/`.

## Contributing

Keep changes within the existing ownership model:

- protocol validity and transactional cache integrity belong in `src/jmap`;
- application semantics, orchestration, settings, tasks, and history belong in daemon-side
  application services;
- process-boundary values and codecs belong in `src/protocol`;
- visual interaction, editing state, and presentation policies belong in `src/gui`; and
- the GUI must never acquire a writable cache or direct transport path.

New mutations need deterministic coverage for projection, success, rejection, ambiguous transport,
stale-refresh rebasing, and crash/retry safety. Prefer small focused commits, precise model updates,
and deleting obsolete compatibility paths over adding new fallback branches.

## Licence

Javelin Mail is licensed under **GPL-3.0-only**. Bundled third-party assets retain their own licences
under `res/`.
