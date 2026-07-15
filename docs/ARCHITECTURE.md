# Architecture

Javelin uses a one-way dependency and control flow:

```text
GUI -> application intents -> synchronization/cache policy -> typed JMAP client -> transport
```

The GUI renders cache-backed models and reports user intent. It does not select JMAP methods,
resolve credentials, or initiate transport operations. Mailbox visibility is represented by an
opaque observation registered with the application coordinator. Pagination, search, content,
downloads, contact operations, mutations, bootstrap, and explicit synchronization are typed
application requests.

The account synchronization service owns state-change consumption, debounce and single-flight
refresh, mailbox interest, state tokens, cache reconciliation, retries, and post-commit events.
Consumers receive one `MailCacheChange` after a synchronization pass commits and reload affected
views from SQLite.

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
