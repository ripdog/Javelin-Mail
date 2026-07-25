# Architecture

Complete-offline mail storage, scheduling, pagination, and search-index invariants are defined in
[OFFLINE_MAIL_ARCHITECTURE.md](OFFLINE_MAIL_ARCHITECTURE.md).

Javelin uses a one-way dependency and control flow:

The project targets Qt 6 and C++23. C++23 is required by the typed Glaze protocol parser used at
the JMAP boundary.

```text
GUI -> application intents -> synchronization/cache policy -> typed JMAP client -> transport
```

The GUI renders cache-backed models and reports user intent. It does not select JMAP methods,
resolve credentials, or initiate transport operations. Mailbox visibility is represented by an
opaque observation registered with the application coordinator. Pagination, search, content,
downloads, contact operations, mutations, bootstrap, and explicit synchronization are typed
application requests.

Message-list commands are coordinated by the GUI-owned `MessageCommandController`. It converts
Qt selection rows into typed `MessageSelection` values, presents destination and confirmation UI,
and invokes `MailApplicationService`. It reports cache invalidation and submission intents back to
the window; `MainWindow` retains only active-tab context and tab/view refresh policy.
`AccountRefreshController` owns configured-account synchronization fallback, bootstrap single-flight
state, resolved-session persistence, cached-account association, and the follow-up contact refresh.
Plain `ConnectionSettings` data and its application-intent conversion are independent of the KDE
preferences dialog; the window only applies busy/status/error and visible-cache refresh effects.

Mailbox and search tabs are backed by application-layer message-list sessions. These sessions own
query-window cache reads, request generations, observation lifetimes, pagination, stale recovery,
and search promotion/prefetch. The GUI owns only tab presentation, selection, and binding the
active session page to the message model; it does not coordinate mailbox or search loading.

Tab variants and shared selection state live in the shell-level `TabWorkspace` model rather than
inside the main window. Pure workspace policy validates the active index, protects the mail home
tab, and selects the next active tab after closure. `TabBarPresenter` renders account-qualified
titles, unread mailbox counts, icons, visibility, and close controls. Mailbox and search session
creation, signal binding, reuse, stale propagation, pagination, sorting, and release belong to the
narrow `MessageListTabController`; pure `MessageListTabPolicy` decides identity matches and stale
targets.
`TabPersistence` converts runtime tabs into storage records and reconstructs cache-only
mailbox/search restore plans. `MainWindow` still owns widget creation, active-tab changes, concrete
widget restoration, and visible-shell side effects, but it does not calculate tab-bar state,
orchestrate message-list session lifetimes, or serialize message-list sessions.
Message selection restoration is likewise split into deterministic workspace policy and a Qt
adapter: the policy decides surviving multi-selection, current-message fallback, and the nearest
row after removal. `MessageSelectionController` extracts current and multi-selection state,
builds selected-message summaries, synchronizes tab selection, and applies restoration plans;
`MainWindow` only presents the resulting message context and triggers application intents.
`MessageNavigationController` owns routed-message matching, one-shot mailbox reveal requests,
refresh waiting, and route completion; pure navigation policy keeps those decisions independent of
Qt indexes and widgets. `MessageContentController` owns content-request deduplication, stale
completion fencing, and typed result dispatch. Pure content ownership policy decides whether a
completion still belongs to the active selection or routed detail before the window refreshes the
visible message.
`MessageActionPolicy` decides command availability from tab context, selection size, Drafts
membership, and read state; the window only gathers those facts and updates actions.
`MessageListPresentationPolicy` maps tab state into page headers and empty-state semantics, while
`MessageListTabPresenter` adapts live sessions and applies that plan to the message-list pane.
`MessageListTabBindingPresenter` binds the active mailbox/search page to the Qt model and keeps the
mailbox tree and search field synchronized; recursive mailbox selection lives in a reusable mailbox
adapter. Pure `TabActivationPolicy` selects mailbox-pane visibility, message presentation behavior,
and whether activation needs one remote refresh. Concrete widget switching remains in
`MainWindow`, while message-list cache loads, page movement, sorting, search promotion, and refresh
calls go through `MessageListTabController`.

The account synchronization service owns state-change consumption, debounce and single-flight
refresh, mailbox interest, state tokens, cache reconciliation, retries, and post-commit events.
Consumers receive one `MailCacheChange` after a synchronization pass commits and reload affected
views from SQLite.

## Cache materialization and navigation

A synchronization result is not UI state until its typed cache materializer has committed it.
Each JMAP data type owns its own adapter, schema, consistency domain, window semantics, and
optimistic rebase rules. The shared contract is deliberately narrow: capture the domain fence,
materialize confirmed objects and authoritative query membership, rebase active projections, then
publish one typed post-commit cache change. There is no generic cross-type object table and no
untyped JMAP value bag.

For Email, an authoritative mailbox materialization includes both the fetched Email/Thread objects
and the exact ordered `Email/query` window. Background watched-mailbox refresh uses the canonical
received-at-descending collapsed window, so a synchronized mailbox is immediately loadable from
SQLite. Any page fetch that writes server Email objects reapplies active Email projections before
the cache can be rendered. Contacts continue to materialize AddressBook and ContactCard snapshots
through their repositories; calendars continue to materialize CalendarEvent objects and bounded
occurrence windows through `CalendarService`. Their state tokens, eviction rules, and optimistic
adapters remain independent.

Starting or restarting an account coordinator schedules an immediate synchronization pass for all
configured mailboxes; a quiet push stream is not proof that their cache already exists. Likewise,
an advertised Email state that is already recorded may suppress redundant object reconciliation
only when every watched mailbox still has authoritative canonical query coverage. A missing or
optimistically invalidated window always requires materialization.

External navigation is an application intent, not a transient widget selection. A notification
activation creates a typed Email route containing stable account, mailbox, thread, and Email ids.
The process-owned coordinator keeps that route alive while the GUI restores, renders any cached
message immediately, and—only when necessary—materializes an anchored authoritative mailbox page.
The route completes after the target has been selected/rendered, or is cancelled by superseding
user navigation. Contact and calendar routes may use the same lifecycle with their own typed route
values; they do not acquire Email pagination semantics.

Mail notification discovery writes a persistent pending outbox before publication. Entries become
delivered only after the desktop-notification signal is emitted, making a process failure in that
gap retryable instead of silently losing the notification. Calendar reminder acknowledgement and
snooze state remains in its separate calendar notification repository.

`JmapMethodTransport` is the request/response boundary for typed JMAP envelopes.
`PreferredJmapMethodTransport` uses the RFC 8887 capability advertised by the cached Session to
keep an authenticated `jmap` WebSocket per owning account, correlate concurrent requests, and send
typed method envelopes over that connection. It falls back to `HttpJmapMethodTransport` only when
the request was not dispatched, so an uncertain disconnect cannot replay a mutation.

The transport decision is persisted per owning account and advertised WebSocket URL. A failed
endpoint uses HTTP for a bounded retry period; a newly advertised URL is probed immediately.
State-change synchronization consults the same decision, preferring RFC 8887 push and switching to
the JMAP EventSource endpoint when WebSocket push fails. Startup performs lightweight Session
rediscovery before restarting account synchronization, while account bootstrap discovers the same
capability during account addition. Session discovery and binary resource transfers remain HTTP
operations.

## Message translation

`TranslationService` is an application-owned provider boundary. It owns persisted translation
preferences, provider credentials, target-language selection, network request batching, and the
SQLite translation cache. The built-in Google credential remains the default when no override is
configured; Preferences may override it, select another target language, or disable translation
entirely. Message text is sent to the configured remote provider only after an explicit Translate
action or a persisted sender/domain auto-translate choice.

The message-view GUI extracts and reapplies plain-text or HTML text chunks, but it does not know the
provider endpoint, construct provider requests, or read and write the translation cache. Translation
requests carry a view generation so a response cannot be applied after navigation or a preference
change. Language-offer policy compares the detected primary language with the configured target
language rather than assuming English is always the target.

## Calendar protocol baseline

Calendar support is implemented against `draft-ietf-jmap-calendars-26` (published
5 November 2025) and RFC 8984 JSCalendar. The exact normative texts are vendored as
`specs/draft-ietf-jmap-calendars-26.txt` and `specs/rfc8984.txt`. Later JMAP Calendars
drafts are deliberately not accepted implicitly: changing the supported draft requires
an explicit protocol review, fixture update, and architecture change.

Calendar protocol envelopes and JSCalendar wire documents remain inside `javelin_jmap`.
The GUI consumes typed calendar domain values and commands through `CalendarService` and
renders committed SQLite state; it never constructs method names or raw JSON.

## Contacts synchronization

Contacts support follows RFC 9610 and preserves complete JSContact documents at the protocol
boundary. The initial synchronization fetches all AddressBooks and ContactCards. Later explicit
refreshes reconcile the small AddressBook set and advance the cached ContactCard state with
`ContactCard/changes`, fetching only created or updated ids in batches bounded by the server's
`maxObjectsInGet` capability. Every intermediate state is committed before requesting the next
changes page. A `cannotCalculateChanges` response invalidates the delta path and performs an
atomic full ContactCard replacement, as required by RFC 8620.

Contact cache commits publish through the process-owned `ContactRepository`. Compose completion,
message sender identity rendering, and the contacts view then reload from SQLite; they do not
retain a second contact data store.

Contact editing projects common JSContact maps into repeatable typed fields while retaining each
map key, label, preference rank, context set, and any unprojected properties in the original
document. Group members are stored as JSContact UIDs, including unresolved UIDs, so temporarily
inaccessible shared contacts are not silently removed. Photos use RFC 9610 blob-backed Media
objects and are fetched on demand rather than retained in the long-lived contact cache.

The application coordination layer evaluates account and AddressBook rights before exposing or
submitting create, update, move, star, merge, copy, and destroy operations. Duplicate discovery is
deliberately high-confidence: normalized email addresses and sufficiently long normalized phone
numbers connect cards of the same kind. A merge keeps the chosen primary UID and name, unions set
properties, preserves colliding mapped entries under new keys, and submits the update and redundant
card destruction together. vCard 4.0 import/export, line unfolding/folding, typed field parameters,
group members, and JSContact document preparation live in the non-GUI contacts layer.
