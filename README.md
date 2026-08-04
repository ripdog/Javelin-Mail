<p align="center">
  <img src="res/icon.svg" width="128" height="128" alt="Javelin Mail icon">
</p>

<h1 align="center">Javelin Mail</h1>

<p align="center">
  A KDE-focused desktop client for JMAP mail, contacts, calendars, and Sieve.
</p>

<p align="center">
  <a href="https://github.com/ripdog/Javelin-Mail/actions/workflows/tests.yml"><img src="https://github.com/ripdog/Javelin-Mail/actions/workflows/tests.yml/badge.svg" alt="Tests"></a>
  <a href="https://github.com/ripdog/Javelin-Mail/actions/workflows/packages.yml"><img src="https://github.com/ripdog/Javelin-Mail/actions/workflows/packages.yml/badge.svg" alt="Packages"></a>
</p>

Javelin Mail provides a conventional desktop mail experience on top of JMAP, designed first for KDE
Plasma. It combines a fast, cache-backed Qt Widgets and KDE Frameworks interface with a lightweight
background service, so mail synchronization, notifications, offline downloads, and delayed sending
continue after the main window closes.

Javelin is aggressively modern. It targets the latest JMAP standards, including some still in the draft
phase. It does not work with most mail services! There is no support for IMAP, CalDAV or CardDAV. 

### Recommended Servers

Javelin has been tested with the latest Stalwart server. This is recommended, as Stalwart has good
support for JMAP Mail, Contacts, and Calendar. Fastmail (with thanks for their work on the JMAP standard)
is supported, but only for Mail and Contacts.

> **Project status:** Javelin is under active pre-1.0 development. The main mail client is usable,
> but packaging, authentication, calendar interoperability, and crash/recovery testing are still
> being hardened. Linux is currently the first-class platform.

## Highlights

### Built for KDE Plasma

- Native KDE menus, configurable toolbars, standard actions, shortcuts, icons, and theme integration
- KDE configuration dialogs and remembered window, toolbar, and workspace state
- KTextEditor-powered plain-text composition, HTML source editing, and Sieve script editing
- Plasma tray integration through KDE's StatusNotifierItem protocol
- Desktop notifications on configurable mailboxes
- Background startup through the Plasma/systemd user session while the heavy GUI remains optional
- Memory efficient - the background service uses between 5-40MiB of RAM when idle

Javelin should run on other modern Linux desktops that provide the same freedesktop interfaces, but
KDE Plasma is the primary design, integration, and testing target.

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

- Local language detection in the GUI process
- Google Translate or private on-device translation using Firefox-compatible models on x86-64
- Configurable target language and per-sender or per-domain automatic translation choices
- Downloadable local model directions with verified, atomic installation and removal from Preferences
- HTML message colours can follow the application theme, retain their original appearance, or use a
  dark presentation

Google remains the default translation provider. Text is sent to Google only after an explicit
Translate action or a saved automatic-translation rule. Choosing **Local (Firefox models)** keeps
message text on the machine; required Mozilla-hosted model packs are downloaded on demand and remain
installed until removed. Choosing **Disabled** also disables language detection.

## Requirements

Javelin currently targets KDE Plasma on Linux with Qt 6, KDE Frameworks 6, and a systemd user
session. Other Linux desktops may work when they provide compatible StatusNotifierItem, desktop
notification, icon-theme, and session-service integration, but they are not the primary target.

A JMAP server with Core and Mail support is required. Sending requires JMAP Submission. Contacts,
calendars, Sieve, and WebSocket push require their corresponding server capabilities.

Account setup can discover JMAP and OAuth metadata from the login address. Providers that support
Open Public Client registration can use browser-based Authorization Code with PKCE; manual JMAP URL
and bearer-token setup remains available as a fallback.

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

When no accounts exist, Javelin opens its onboarding wizard after connecting to the daemon. Enter the
login email and let Javelin discover the provider. When the provider advertises compatible OAuth
metadata and dynamic client registration, authentication continues in the system browser using PKCE.

Manual setup remains available for private or self-hosted servers. It accepts a display name, login
email, optional JMAP Session URL or server base URL, and a server-issued bearer token. Leaving the
server field empty discovers `/.well-known/jmap` from the login email domain.

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

- KDE Plasma on Linux is the actively supported desktop target; other Linux desktops are best-effort.
- Automatic browser OAuth depends on provider metadata and dynamic client registration; other servers
  require manual API key setup.
- Account credentials are stored in the daemon's native per-user settings store rather than a
  desktop secret service.
- Calendar support should not yet be treated as fully interoperability-hardened.


## Documentation

- [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) — build environment, compilation, testing, and developer workflow
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — major components and how they interact
- [`docs/DEVELOPMENT_PLAN.md`](docs/DEVELOPMENT_PLAN.md) — current product and engineering roadmap

Focused design documents for synchronization, offline storage, pagination, Undo/Redo, rendering,
and the daemon/GUI split are linked from the architecture document.

## Acknowledgements

Javelin Mail is made possible by these projects and resources:

- [fastText](https://github.com/facebookresearch/fastText), including the `lid.176.ftz`
  language-identification model distributed by Facebook Research under
  [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/).
- [Firefox Translations](https://github.com/mozilla/translations) and the
  [Bergamot](https://github.com/browsermt/bergamot-translator) translation engine. Translation
  model packs are distributed by Mozilla under the Mozilla Public License 2.0; the relevant
  notices and provenance are recorded in the downloaded model installation.
- [Thunderbird](https://github.com/thunderbird/thunderbird) for the adapted interface icons,
  licensed under the Mozilla Public License 2.0.
- [Dark Reader](https://github.com/darkreader/darkreader) for the HTML message appearance runtime,
  licensed under the MIT License.
- [Qt](https://www.qt.io/), [KDE Frameworks](https://develop.kde.org/products/frameworks/),
  [QCoro](https://github.com/qcoro/qcoro), [glaze](https://github.com/stephenberry/glaze), and
  [Catch2](https://github.com/catchorg/Catch2).

Licence and provenance notices are recorded in the repository under [`res/`](res/) and bundled
with installed builds as applicable. Javelin Mail is not affiliated with or endorsed by any of
these projects.

## Licence

Javelin Mail is licensed under **GPL-3.0-only**. Bundled third-party assets retain their own licences
under `res/`.
