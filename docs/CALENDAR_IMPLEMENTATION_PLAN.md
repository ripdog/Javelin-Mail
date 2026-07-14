# Calendar implementation plan

## Scope and protocol baseline

Javelin Mail targets `draft-ietf-jmap-calendars-26` exactly, together with RFC 8984
JSCalendar. The committed protocol sources are
`specs/draft-ietf-jmap-calendars-26.txt` and `specs/rfc8984.txt`. A later draft is not
an implicit upgrade: changing the supported version requires an explicit design,
fixture, implementation, and interoperability update.

Calendar creation and deletion, a full invitation inbox, CalDAV, and a complete
offline calendar mirror remain out of scope.

## Current state

The first usable calendar slice is present:

- Session discovery enables Calendar only for accounts advertising the draft-26
  capability, including the capability shape used by the tested Stalwart server.
- Typed Calendar and CalendarEvent methods, domain values, service commands, and
  SQLite-backed calendars, events, occurrences, memberships, query windows, and
  state storage exist.
- The application queries the full visible 42-day interval for each capable
  account, displays cached results, and refreshes online.
- The custom six-week month widget supports adjacent-month dates, locale week
  starts, selection, navigation, calendar colors, overflow agendas, and event
  dialogs.
- Single events can be created, updated, moved, and deleted. Mutation failures are
  logged, leave the dialog open, and do not update the cache before server success.
- All-day/floating event parsing, base-event lookup for expanded occurrences, time
  editors, automatic end-time adjustment, account-labelled tabs, calendar
  visibility controls, equal columns, and today highlighting are implemented.

The remaining work is ordered by correctness and data-loss risk, then synchronization,
cache lifecycle, product completeness, and quality gates.

## Milestone 1: lossless ordinary edits — complete

An editor must not erase information merely because its controls cannot represent
the full JSCalendar object.

- Hydrate recurrence and attendee controls from the loaded event.
- Preserve recurrence rules, recurrence overrides, attendee scheduling metadata,
  multi-calendar membership, and a missing/floating `timeZone` unless the user
  explicitly changes the corresponding field.
- When attendees are edited, retain matching attendee identifiers, ownership,
  participation status, and scheduling state; generate identifiers only for newly
  added attendees. Owners are never silently removed.
- Show unsupported/custom recurrence as an unchanged value instead of flattening it
  into one of the simple presets.
- Add deterministic regression tests for attendee reconciliation and preservation
  decisions.

Acceptance: opening a recurring, scheduled, floating, or multi-calendar event,
changing only its title, and saving cannot alter those unrelated properties.

## Milestone 2: occurrence and series editing — in progress

- Preserve enough base-event and recurrence identity in the cache/UI to distinguish
  an expanded occurrence from its series.
- On an occurrence mutation, offer an explicit “this occurrence” or “entire series”
  choice.
- Implement occurrence-only update/delete through `recurrenceOverrides`, including
  exclusions, changed start/duration/title, and existing override composition.
- Implement whole-series edits without accidentally addressing a synthetic expanded
  identifier.
- Cover recurrence expansion boundaries, exclusions, overrides, DST transitions,
  and Stalwart-expanded response shapes with fixtures and service tests.

Acceptance: both mutation scopes produce the draft-26 wire shape and refresh the
visible occurrence set correctly.

## Milestone 3: incremental synchronization — in progress

- Feed `Calendar/changes` and `CalendarEvent/changes` into the existing synchronization
  coordinator and persist per-account state tokens transactionally.
- Apply created, updated, and destroyed objects without replacing unrelated cached
  data. Recover clearly from `cannotCalculateChanges` by refreshing only affected
  visible windows.
- Refresh visible windows after a committed change and emit one typed cache-change
  notification. Connect the GUI to that notification rather than protocol events.
- Use `ifInState` for mutations and expose stale-state conflicts as typed actionable
  failures.
- Subscribe push transports to calendar state types where the server supports them.

Acceptance: remote calendar changes appear without manual navigation, duplicate
refreshes are coalesced, and stale writes never overwrite newer server state.

Current incremental coverage applies non-recurring creates/updates and all destroys
directly across overlapping cached windows. A changed recurring series still uses the
bounded visible-window refresh because its new occurrences must be expanded by the
server for the display time zone.

## Milestone 4: cache bounds and request concurrency

- Define and implement eviction for old covered windows while retaining an event or
  occurrence referenced by another window.
- Add request generation/cancellation so a slow response for an old month or account
  cannot replace the active view.
- Enforce advertised query and object limits with bounded pagination/batching.
- Complete tests for overlapping windows, stale membership removal, per-account
  isolation, transactional rollback, cache-first startup, and failed mutations.

Acceptance: repeated navigation keeps the database bounded and out-of-order network
responses cannot corrupt or rewind the visible calendar.

The cache retains the 12 most recently refreshed or loaded windows per account and
display time zone. Eviction and orphan pruning run in the same reconciliation
transaction, while occurrences referenced by any retained overlapping window remain.
Each owner account also has a refresh generation: starting a newer range or state-change
refresh supersedes older in-flight work, which is discarded before it can commit an
obsolete window or rewind an opaque JMAP state token.
Full refreshes split expanded occurrence and stable base-event `CalendarEvent/get`
requests at the Core capability's `maxObjectsInGet` limit. Each batch uses one method
call and all batches must report the same event state before reconciliation.

## Milestone 5: calendar product and month-view completeness

- Persist per-account calendar visibility and writable default destination.
- Represent multi-calendar membership without pretending it is a single-calendar
  property; make rights and read-only state explicit in the editor.
- Calculate chip capacity from actual cell geometry and fonts.
- Render multi-day/all-day spans coherently across cells, improve timed ordering and
  overflow agenda navigation, and retain full keyboard/focus behavior.
- Make long UI errors inspectable/copyable while keeping complete diagnostics in the
  logs.

Acceptance: preferences survive restart, rights are enforced before a request, and
the fixed 42-cell view remains legible across font/DPI/locale changes.

## Milestone 6: interoperability and quality gates

- Commit sanitized session and method fixtures captured from the supported Stalwart
  release, including capability, calendar rights, expanded recurrence, floating and
  all-day events, and set errors.
- Expand serialization tests for pagination, malformed responses, bounded recurrence,
  service errors, and draft-26 scheduling behavior.
- Add service tests using a scripted transport for create/update/move/delete,
  attendees, permission failures, scheduling failures, and unchanged cache on error.
- Add widget tests for the full fixed-grid, locale, navigation, adjacent-month,
  selection, ordering, overflow, dialog hydration, and validation behavior.
- Pass the CMake debug build, Catch2 suite, clang-tidy/clazy, and sanitizer-enabled
  tests.

Acceptance: all supported behavior is reproducible without a live server and the
documented quality gates pass from a clean checkout.
