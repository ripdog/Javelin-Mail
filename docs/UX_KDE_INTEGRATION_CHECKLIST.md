# UX and KDE integration checklist

## Purpose

This is a collection of small, mostly independent UX and desktop-integration improvements discovered
in the August 2026 UI audit. It is intentionally a checklist rather than a detailed design: each item
should be suitable for picking up in a separate implementation thread.

Items are ordered approximately from lowest to highest implementation difficulty, not by product
priority. Preserve the existing daemon/GUI ownership model and SQLite source of truth throughout.
Check off completed items as you commit.

## Low difficulty

### [x] Add Sonnet spell checking to compose

- Add KF6 Sonnet spell checking to rich and plain-text composition, including context-menu
  suggestions and the normal dictionary/configuration UI.
- Reuse Javelin's existing language detection where useful for initial dictionary selection.
- Do not introduce a second text-editing stack solely for spell checking.

### [x] Add a command palette

- Use `KXmlGuiWindow`'s built-in `open_kcommand_bar` / `KCommandBar` integration over Javelin's
  existing `QAction` / KXMLGUI actions rather than constructing a parallel command palette.
- Keep actions as the source of truth so shortcuts, menus, toolbars, and the command palette invoke the
  same commands.
- Keep action availability truthful for Find Action: workspace-level commands should activate the
  required tab and run, while selection-, editor-, and context-menu-only commands should be disabled
  when their required context is absent rather than silently doing nothing.

### [x] Improve tab conventions and keyboard navigation

- Make tabs movable, use a themed close icon, support middle-click close, `Ctrl+W`, next/previous tab,
  and reopen-last-closed where state can be restored safely.
- Add configurable shortcuts for next/previous message, next/previous unread, and focusing the mailbox
  tree, message list, reader, and search field.
- Expose everything through KDE's existing shortcut configuration rather than hard-coding a second
  shortcut system.

### [x] Replace bare transient feedback with actionable inline feedback

- Use `KMessageWidget` or equivalent non-modal inline feedback for ordinary operation results and
  recoverable failures.
- Prefer messages such as "Archived 7 messages — Undo" or an error with Retry/Sign In actions over a
  status-bar-only message.
- Keep modal dialogs for genuinely blocking decisions or intervention.

### [x] Improve message-list empty states

- Distinguish an actually empty mailbox from no filter matches, no search results, disconnected/auth
  state, failed query, and not-yet-synced state.
- Offer the relevant local action such as Clear Filters, Edit Search, Retry, or Sign In Again.

### [x] Add message reader Find, Zoom, and Print actions

- Add first-class actions for Find in Message, Zoom In/Out/Actual Size, and Print.
- Make them discoverable through menus, configurable shortcuts, and the command palette rather than
  relying on incidental WebEngine behavior.

### [ ] Add notification privacy controls

- Allow full preview, sender + subject, sender only, or a private generic notification.
- Apply the same principle to calendar notification content where sensitive details may appear.
- Keep privacy policy in Javelin even though presentation remains through desktop notifications.

### [x] Expand the tray menu and clarify quit semantics

- Add useful daemon-backed actions such as New Message, Inbox unread count, Contacts, Calendar, and
  Task Center.
- Rename the tray daemon-stop action to something explicit such as "Stop Background Service".
- Avoid using the same "Quit" label for GUI-only exit and daemon shutdown.
- Use attention state for authentication/sustained failure rather than ordinary unread mail.

### [x] Improve the desktop entry

- Add `GenericName`, useful `Keywords`, and `SingleMainWindow=true` where appropriate.
- Add launcher actions for New Message, Inbox, Calendar, and Contacts.
- Route launcher actions through Javelin's existing typed/singleton activation path; do not add a
  parallel activation mechanism.

### [ ] Make application theme preference explicitly Follow System / Light / Dark

- Treat Follow System as the normal/default behavior.
- Avoid forcing Breeze Light/Dark when the user has chosen another KDE color scheme.
- Keep HTML message color handling separate from the application palette.

### [ ] Prefer theme icons for standard semantic actions

- Use `QIcon::fromTheme()` where a standard semantic icon exists.
- Keep bundled/custom icons for Javelin-specific visuals or as deliberate fallbacks so custom Plasma
  icon themes have maximum effect.

### [ ] Add AppStream screenshots when the UI is stable

- Add representative screenshots and review release metadata for Discover/software-center presentation.
- Review desktop/AppStream/application IDs for consistency before wider distribution.

## Low to medium difficulty

### [x] Add searchable Move/Copy mailbox menus

- Refactor mailbox destination discovery away from immediate `QAction` creation so the same collected
  destination model can feed hierarchical and searchable presentations.
- Keep the existing hierarchy when the search field is empty.
- Put a `QLineEdit` in the `QMenu` using `QWidgetAction`; while typing, show a flat fuzzy-ranked result
  list.
- Match mailbox name, parent path, and account display name. Disambiguate duplicates with path/account.
- Support Up/Down/Enter and make printable typing focus the search immediately.
- Optionally show a short recent-destinations section before the hierarchy.
- Reuse the same destination/ranking code for toolbar Move/Copy and later Tag pickers.

### [ ] Add a searchable Tag picker

- Apply the same fuzzy picker pattern used by Move/Copy to tags when the tag set is large.
- Preserve the ordinary menu/checkable presentation for small sets and mouse-first use.

### [ ] Use KIO for opening attachments

- Replace local `QDesktopServices::openUrl()` attachment opening with `KIO::OpenUrlJob` plus the
  appropriate UI delegate.
- Gain KDE MIME association/Open With behavior, safe executable handling, and cleaner temporary-file
  lifecycle.
- Add Open With… where useful.

### [ ] Keep file choosers separate from KIO opening

- Continue using `QFileDialog` for normal open/save/import/export selection; Plasma's Qt platform
  integration already supplies the native chooser.
- Treat XDG FileChooser portal behavior as a packaging/runtime concern and test it explicitly for
  Flatpak/sandboxed builds.
- Do not assume `KIO::OpenUrlJob` changes which Open/Save dialog backend is used.

### [ ] Add "Show in Folder" after file saves

- After exporting/saving an attachment or message, offer a direct way to reveal the resulting file in
  the user's file manager using the standard KDE/freedesktop mechanism.

### [ ] Add mailbox favourites

- Let users pin/favourite frequently used mailboxes near the top of the mailbox tree without modifying
  the server hierarchy.
- Keep this as local presentation preference, not a new mailbox/sync concept.

### [ ] Fill common mail-navigation/action gaps

- Add Mark All Read and make next/previous unread navigation first-class configurable actions.
- Evaluate Snooze and Mute Conversation separately as product features rather than folding them into
  basic keyboard/navigation work.

### [ ] Open `.eml` files in Javelin

- Advertise `message/rfc822` in the desktop file association and accept `.eml` through the existing
  singleton activation path.
- Open the message in a read-only viewer; do not silently import it into an account.
- This should provide a natural round trip for Javelin's existing mail export feature.

### [ ] Add actionable new-mail notifications

- Add daemon-owned actions such as Archive, Mark Read, and Reply where the notification protocol allows
  them.
- Route mutations through the same application command/optimistic-consistency paths as the GUI.
- Keep notification actions useful when the main GUI is closed.

## Medium difficulty

### [ ] Add reading-pane layout modes

- Preserve the current three-column layout as Wide mode.
- Add Classic mode (folders left, list above reader), List Only/no-reader mode, and optionally a
  reader-focused layout.
- Persist layout selection and splitter geometry without replacing the existing workspace/tab restore
  machinery.

### [ ] Add message-list density options

- Add at least compact and comfortable row/card density presets.
- Preserve sender/subject legibility and accessibility rather than making every delegate metric freely
  configurable.

### [ ] Add external drag-out for attachments

- Allow attachments to be dragged from Javelin to Dolphin/other applications, materializing or
  downloading the file as required.
- Define temporary-file lifetime carefully so the drag target does not receive a path that disappears
  too early.

### [ ] Add external drag-out for messages

- Extend drag-out to messages by materializing RFC 5322 `.eml` using the existing export/raw-message
  infrastructure.
- Keep internal mail-transfer drag MIME separate from external file drag MIME.

### [ ] Add KDE Purpose sharing

- Add Share… for attachments, saved `.eml`, and other sensible shareable artifacts using KDE Purpose.
- Do not add sharing to content where doing so would expose hidden/private data unexpectedly.

### [ ] Surface long user-initiated tasks through Plasma job progress

- Explore a thin `KJobWidgets`/UI-server adapter for long operations such as export, cross-account
  transfer, or large offline downloads.
- Do not expose routine background synchronization as user-visible Plasma jobs.
- Keep Widgets out of daemon core; adapt existing daemon task events at a desktop/UI boundary.

### [ ] Add first-class Day / Week / Month calendar modes

- Keep Month view and the existing Day Agenda behavior.
- Add a conventional Week view as the largest missing calendar navigation mode, with a Day mode if it
  naturally shares the implementation.
- Treat recurrence-aware event dragging/resizing as a later correctness-sensitive enhancement rather
  than bundling it into the first view implementation.

## Medium to high difficulty

### [ ] Add KRunner integration

- Provide explicit-trigger searches such as `mail invoice`, `contact Alice`, and `calendar dentist`.
- Search Javelin's local cache through a narrow read-only interface/IPC path; the runner must not become
  another JMAP client or cache authority.
- Route result activation through Javelin's existing singleton activation mechanism.
- Require explicit trigger words by default to avoid leaking mail/contact content into generic desktop
  searches.

### [ ] Investigate KPeople integration

- Determine whether a Javelin-backed KPeople provider would materially improve current Plasma contact
  integration.
- Prefer a narrow read-only/provider bridge over introducing another contact storage authority.
- Implement only if contemporary Plasma/KDE consumers make the integration worthwhile.

### [ ] Add saved searches / smart mailbox presentation

- Build on the normal search/query infrastructure rather than creating a second synchronization path.
- Store user-defined query definitions locally and expose them alongside mailbox/favourite navigation.
- Consider this together with unified views so both use the same virtual-list abstraction where
  possible.

## High difficulty

### [ ] Add Unified Inbox and other cross-account virtual views

- Introduce an aggregate/virtual message-list abstraction that combines per-account authoritative
  query results without becoming a new cache or sync authority.
- Start with Unified Inbox, then consider Unified Unread and Unified Starred. Avoid Unified Drafts
  initially because editing/account semantics are more complicated.
- Preserve account identity on every row and operation so commands are routed to the correct account.
- Define ordering, pagination/windowing, thread expansion, selection, and refresh behavior across
  multiple account-backed result sets before implementation.

### [ ] Add cross-account/global mail search

- Reuse the same aggregate-list primitive as unified views.
- Fan out authoritative searches per account and merge results deterministically in the GUI/read-model
  layer without introducing another data authority.
- Preserve each result's account and mailbox provenance for rendering and commands.

### [ ] Design account indication for unified message cards

- Do not add a full account-name text field to the already dense message card.
- Preferred presentation: a narrow stable account-colour strip on the card edge, with the same account
  color echoed in account roots/other unified surfaces.
- Provide the full account name in accessibility text, tooltip, and message details so color is never
  the only identifier.
- If color alone proves insufficient in usability testing, consider a tiny colored account monogram
  rather than another text line.
- In Unified Inbox the mailbox is implicit and need not be shown. In global search, render location as
  `Mailbox · Account` in the existing mailbox/location area.

## Deferred / deliberately avoid

### [ ] Reconsider an optional Akonadi bridge only if interoperability requires it

- Do **not** make Akonadi part of Javelin core or replace Javelin's daemon-owned SQLite cache with it.
- If KOrganizer/KAddressBook interoperability later justifies the cost, investigate an optional
  external Akonadi resource that talks to Javelin through a narrow interface.
- Treat duplicated cache/state ownership and synchronization complexity as the primary design risk.

### [ ] Revisit `.ics` calendar import/export separately

- Do not bundle calendar file association/import into `.eml` support merely because desktop MIME
  integration is being changed.
- Design iCalendar import/export semantics explicitly before advertising `text/calendar` handling.

## Architectural guardrails for all items

- The daemon remains the owner of JMAP transports, writable repositories, background work, and
  stateful application commands.
- The GUI remains operationally read-only and does not become a second source of truth.
- Extend existing read models and activation routes instead of adding parallel stores or command paths.
- Keep KDE integrations at the desktop/shell boundary unless there is a concrete architectural reason
  to go deeper.
- Do not replace working native abstractions solely for KDE purity: direct freedesktop notifications,
  `QFileDialog`, and Javelin's existing workspace restoration are valid until a specific missing
  capability justifies changing them.
