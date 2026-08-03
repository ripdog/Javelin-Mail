<p align="center">
  <img src="res/icon.svg" width="128" height="128" alt="Javelin Mail icon">
</p>

<h1 align="center">Javelin Mail</h1>

<p align="center">
  A modern native desktop client for JMAP mail, contacts, calendars, and Sieve.
</p>

Javelin Mail provides a conventional desktop mail experience on top of JMAP. It combines a fast,
cache-backed Qt interface with a lightweight background service, so mail synchronization,
notifications, offline downloads, and delayed sending continue after the main window closes.

> **Project status:** Javelin is under active pre-1.0 development. The main mail client is usable,
> but packaging, authentication, calendar interoperability, and crash/recovery testing are still
> being hardened. Linux is currently the first-class platform.

## Highlights

### A complete desktop mail client

- Multiple accounts, mailbox tabs, threaded message lists, pagination, and advanced search
- HTML and plain-text message viewing, attachments, inline images, and source inspection
- Archive, move, copy, delete, star, read/unread, Undo, and Redo
- Stable selection and navigation while new mail and synchronization update the cache
- Remote-content controls remembered by sender or domain

### Rich compose and reliable sending

- New message, Reply, Reply All, Forward, and edit-draft workflows
- Rich HTML or plain-text composition, including HTML source and preview modes
- Multiple identities, Cc/Bcc, attachments, and embedded images
- Server-backed drafts and configurable Undo Send
- Delayed sends continue in the background when the GUI is closed

### Contacts, calendars, and server-side tools

- JMAP contacts and address books, including groups, photos, vCard import/export, and duplicate merge
- JMAP calendar month view, recurring events, reminders, and calendar management
- JMAP Sieve script editing
- Features appear according to the capabilities and permissions advertised by the server

Calendar support is still being hardened, especially for complex recurrence and interoperability
workflows.

### Background and offline operation

- `javelind` keeps synchronizing and showing notifications without an open window
- Complete offline copies can be enabled per mailbox, including messages and attachments
- Large downloads and indexing jobs are resumable and visible in Task Center
- Local full-text search indexes are rebuildable from the downloaded mail vault
- JMAP-over-WebSocket and push are used when supported by the server

### Translation and appearance

- Local language detection
- Optional Google Translate integration with a configurable target language
- Per-sender and per-domain automatic translation choices
- HTML message colours can follow the application theme, retain their original appearance, or use a
  dark presentation

Translation is opt-in. Text selected for translation is sent to Google Translate.

## Requirements

Javelin currently targets Linux desktops with Qt 6 and a systemd user session. Tray integration uses
the StatusNotifierItem protocol supported by KDE Plasma and many other desktop environments.

A JMAP server with Core and Mail support is required. Sending requires JMAP Submission. Contacts,
calendars, Sieve, and WebSocket push require their corresponding server capabilities.

Account setup currently uses a server-issued bearer token or API key. Interactive OAuth login is not
yet available.

## Getting started

### 1. Build and install

There are not yet official stable binary releases. On Arch Linux, the repository can be packaged
with:

```sh
makepkg -si
```

For dependency setup, generic CMake builds, development builds, testing, and packaging details, see
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).

### 2. Start the background service

An installed build provides a systemd user service:

```sh
systemctl --user daemon-reload
systemctl --user enable --now javelind.service
```

Then launch **Javelin Mail** from the desktop menu or run:

```sh
javelin
```

Javelin can also offer to start the daemon from its connection-recovery window.

### 3. Add an account

Open **Settings → Configure…**, select **Accounts**, and add:

- **Display Name** — the local name shown for the connection
- **Login Email** — the account login or primary address
- **API Key** — the server-issued bearer token or API key
- **Server** — an optional JMAP Session URL or server base URL

Leave **Server** empty to discover `/.well-known/jmap` from the login email domain. Applying the
settings starts capability discovery and the initial mailbox refresh.

### 4. Choose background behaviour

Under **Mailbox Sync**, choose independently:

- which mailboxes should be retained as complete offline copies; and
- which mailboxes should produce desktop notifications.

Large offline jobs continue in Task Center and yield to foreground browsing and commands.

## Using Javelin

Closing the last window exits the heavy GUI process but leaves `javelind` running. Opening Javelin
again reconnects to the daemon and restores the previous workspace. A second launch raises the
existing window rather than opening another GUI instance.

The main toolbar and menus provide compose, reply, forward, archive, delete, move, copy, search,
contacts, calendar, Sieve, and refresh actions. `Ctrl+Z` and `Ctrl+Y` invoke daemon-owned Undo and
Redo. The status-bar task summary opens Task Center.

Useful service commands are:

```sh
systemctl --user status javelind.service
systemctl --user restart javelind.service
journalctl --user -u javelind.service -f
```

## Important current limitations

- Linux is the only actively supported desktop target.
- Authentication currently requires a static bearer token or API key.
- Account credentials are stored in the daemon's native per-user settings store rather than a
  desktop secret service.
- Calendar support should not yet be treated as fully interoperability-hardened.
- The project is pre-1.0; cache formats and settings may change without long-term compatibility
  guarantees.

The server remains authoritative for mail, drafts, contacts, and calendars. Javelin's local cache,
offline vault, search indexes, task state, and Undo history are designed to be rebuildable rather
than independently backed-up user data.

## Documentation

- [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) — build environment, compilation, testing, and developer workflow
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — major components and how they interact
- [`docs/DEVELOPMENT_PLAN.md`](docs/DEVELOPMENT_PLAN.md) — current product and engineering roadmap

Focused design documents for synchronization, offline storage, pagination, Undo/Redo, rendering,
and the daemon/GUI split are linked from the architecture document.

## Licence

Javelin Mail is licensed under **GPL-3.0-only**. Bundled third-party assets retain their own licences
under `res/`.
