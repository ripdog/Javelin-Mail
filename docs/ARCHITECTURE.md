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

`JmapMethodTransport` is the request/response boundary for typed JMAP envelopes. The current
`HttpJmapMethodTransport` adapts it to HTTP. A future RFC 8887 connection may implement the same
interface and the state-change source interface over one physical WebSocket without changing
synchronization or GUI code. Session discovery and binary resource transfers remain distinct HTTP
operations.
