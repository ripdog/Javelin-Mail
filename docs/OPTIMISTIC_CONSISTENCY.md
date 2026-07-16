# Optimistic Consistency Architecture

## Purpose

Javelin renders SQLite-backed state while JMAP mutations and refreshes run asynchronously. A
refresh that started before a successful mutation may therefore return afterward with an older
server snapshot. The consistency subsystem prevents that snapshot from replacing a newer locally
known result, while still exposing definitive server rejection and later remote changes.

The subsystem applies to stateful JMAP object types. Uploads, downloads, validation calls, and
other procedural operations do not use optimistic projection.

## Boundaries

The internal JMAP library owns:

- durable mutation records and their lifecycle;
- per-account, per-data-type causal generations;
- JMAP PatchObject encoding and protocol response classification;
- confirmed cache state, optimistic projection, and atomic reconciliation;
- refresh fencing, crash recovery, and targeted uncertainty resolution.

The application coordination layer owns:

- interpretation of user intent;
- operation groups spanning multiple JMAP data types;
- batching and dependency policy;
- partial-failure policy and which affected views should refresh.

The GUI raises typed commands and renders the effective cache state. It does not construct JMAP
patches, manage mutation states, or retain a second optimistic object store.

## State Model

Consistency is scoped by a domain:

```text
(accountId, dataType)
```

Examples are `Email`, `ContactCard`, `AddressBook`, `CalendarEvent`, and `SieveScript`. Each domain
has an independent monotonic mutation generation because JMAP state tokens are scoped to a data
type.

The effective state rendered by the application is:

```text
confirmed server state + active mutation projection
```

Refreshes update confirmed state. They never remove an active projection merely because an older
snapshot does not contain the requested change.

## Mutation Lifecycle

Every durable mutation has one of these states:

- `pending`: committed locally but not submitted;
- `in_flight`: dispatched, with no definitive response yet;
- `accepted`: the server returned per-object success;
- `rejected`: the server returned a definitive per-object error;
- `unknown`: dispatch may have occurred but the transport outcome is ambiguous.

`pending`, `in_flight`, and `unknown` records remain projected. An accepted result is folded into
confirmed state atomically and advances the domain generation. A rejected result removes its
projection and restores the current confirmed state. Unknown results are reconciled with a
targeted `/get` or `/changes` request before retry or rollback.

On startup, persisted `in_flight` records become `unknown`; a process restart cannot prove that the
server did not apply them.

## Refresh Fencing

A refresh captures the current domain generation before network I/O. Immediately before its cache
transaction, it verifies that the generation is unchanged.

If the generation changed, the complete response is obsolete. The refresh discards all fetched
objects, query windows, and state tokens and schedules a coalesced replacement refresh. Partial
application is forbidden.

This fence is a causality check, not a timeout or an optimistic grace period. A refresh begun after
an accepted mutation remains authoritative and may legitimately show a subsequent remote change.

## Protocol Rules

- Mutations use exact PatchObject paths for changed properties. They do not send complete
  collection-valued properties when the intent changes individual entries.
- Per-object `/set` success and failure are handled independently.
- Server-returned transformed properties are merged into confirmed state.
- `ifInState` is used when the application command requires compare-and-swap semantics. It is not a
  substitute for exact patches or local causal fencing.
- Capability and rights checks fail before optimistic projection when the operation is known to be
  unsupported.

## Atomicity

These transitions are single SQLite transactions:

- append mutation and materialize its projection;
- mark accepted, update confirmed fields, advance generation, and retire the projection;
- mark rejected and rebuild effective state;
- commit a refresh snapshot, its state tokens, and reapplied projections.

Repositories expose transaction-compatible primitives. Service code does not compose several
independent repository transactions for one consistency transition.

## Service Adapters

The generic engine stores lifecycle and causality. A typed adapter provides:

- mutation payload serialization;
- exact JMAP patch construction;
- local projection and rebase behavior;
- affected object and query identifiers;
- response transformation handling;
- targeted reconciliation;
- service-specific conflict classification.

Cross-type actions, such as sending a draft through `Email/set` and `EmailSubmission/set`, are
application-owned operation groups containing typed mutations with explicit dependencies.

## Required Invariants

1. A refresh response cannot overwrite a mutation accepted after that refresh began.
2. Definitive rejection is never hidden by an optimistic projection.
3. An ambiguous transport outcome is never treated as definitive success or failure.
4. A mutation changes only the properties explicitly requested.
5. Cache objects, projections, mutation lifecycle, and state tokens advance atomically.
6. Recovery after a crash preserves uncertainty and cannot silently duplicate a mutation.
7. The GUI observes only committed effective states, never intermediate reconciliation writes.

## Migration Order

1. Add persisted consistency domains and refresh fences.
2. Replace the mail-only pending action schema with the generic mutation journal.
3. Migrate Email mailbox and keyword mutations to exact patches and atomic projection.
4. Fence Email, ContactCard, AddressBook, CalendarEvent, and SieveScript refresh commits.
5. Migrate direct Contacts, Calendar, Sieve, and compose mutations to typed adapters.
6. Add operation groups, crash recovery, targeted unknown-result reconciliation, and coalescing.
7. Remove all direct mutation/cache paths that bypass the consistency subsystem.
