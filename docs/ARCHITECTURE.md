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
