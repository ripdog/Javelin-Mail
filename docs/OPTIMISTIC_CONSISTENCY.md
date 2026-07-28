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
When a protocol cannot safely correlate a server-created object after a lost create response, the
record remains unknown and the same logical command is blocked from being submitted again.

On startup, persisted `in_flight` records become `unknown`; a process restart cannot prove that the
server did not apply them.

## One-Request Acceptance

In the normal case, a user mutation is settled by its single JMAP API envelope. The typed adapter
returns a `MutationCommitReceipt` containing each affected account/data-type state transition,
accepted and rejected object IDs, affected cache views, and whether requested materialization was
incomplete. The response transaction advances exact state tokens and folds server-created IDs or
transformed documents into the projection. Success does not schedule a follow-up `/changes`,
`/query`, or `/get`.

Dependent reads required to materialize the result are method calls in the same sequential
envelope. Mail membership/count mutations use `Email/set` followed by `Mailbox/get`. Recurring
event mutations use `CalendarEvent/set`, an expanded range query, and a result-referenced get. A
successful set remains accepted if dependent materialization is incomplete.

Extra API requests are reserved for ambiguity, `stateMismatch`, partial rejection, protocol
pagination or method/object limits, and genuinely newer server state.

## Push Coordination

Push changes remain an `accountId -> dataType -> state` map through transport and routing. A pushed
state is held while that account/data-type has an in-flight mutation. Once the response commits, an
equal state is discarded; only a newer advertised state starts the typed incremental refresh.
Calendar and contact accounts are never flattened into the primary mail account.

Refresh and materialization work is serialized per account/data-type. Repeated pushes coalesce,
and a matching push cannot supersede materialization already carried in the mutation envelope.

## Refresh Fencing

A refresh captures the current domain generation before network I/O. Immediately before its cache
transaction, it verifies that the generation is unchanged.

If the generation changed, the complete response is obsolete. A refresh also cannot commit while
the domain has an active `pending`, `in_flight`, `accepted`, or `unknown` projection unless its
typed adapter rebases those mutations as part of the same cache transaction. The conservative
default is to discard the complete response and schedule a coalesced replacement refresh. Partial
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

The concrete library boundary is:

- `MutationJournalRepository`: durable, policy-neutral lifecycle queries and transitions;
- `ConsistencyDomainRepository`: monotonic generations and refresh-fence checks;
- `MutationProjectionTransaction`: one transaction for journal, cache, and generation changes;
- a service-specific mutation journal: typed payload, projection, restoration, and rebase;
- a service: capability checks, protocol dispatch, response classification, and reconciliation.

Application code invokes typed service commands. It does not read or write generic journal records
to decide product behavior.

## Implemented Service Policy

| JMAP action | Optimistic projection | Ambiguous-result reconciliation |
| --- | --- | --- |
| `Email/set` mailbox, keyword, and destroy changes | Exact Email patch projected into SQLite | Refreshed server Email is compared with the requested patch; satisfied unknown patches retire atomically |
| `ContactCard/set` and `AddressBook/set` | Full typed document projection; contact-group membership uses exact `members/{uid}` patches | Full snapshots are rebased; exact updates, absent destroys, and correlatable creates retire unknown records |
| `ContactCard/copy` move/copy workflows | Destination and optional source projections are one operation group | Destination and source outcomes reconcile independently, preserving RFC 8620 non-atomic copy semantics |
| `CalendarEvent/set` | Event and visible occurrence projection | Full ranges are rebased; stale recurrence expansions are suppressed until the base event is confirmed |
| `SieveScript/set` | Complete effective script-list projection | Full script snapshots are correlated and rebased |
| Draft `Email/set` replacement | New local draft is projected before dispatch; old draft remains hidden | Lost creates remain unknown and duplicate saves for that compose session are blocked |
| `EmailSubmission/set` send | Draft moves to the Sent projection before dispatch | Submission and implicit Email changes are tracked as dependent mutations; ambiguity preserves the Sent projection |

Uploads, downloads, Sieve validation, identity reads, and other procedural calls do not own
persistent JMAP object state and therefore do not create optimistic records.

Calendar recurrence expansion remains server-owned. While a recurring CalendarEvent mutation is
active, the cache uses one local anchor occurrence and suppresses stale expanded occurrences. The
normal mutation envelope queries and gets the visible expansion after the set, replacing that
anchor without a second API request.

## Required Invariants

1. A refresh response cannot overwrite a mutation accepted after that refresh began.
2. Definitive rejection is never hidden by an optimistic projection.
3. An ambiguous transport outcome is never treated as definitive success or failure.
4. A mutation changes only the properties explicitly requested.
5. Cache objects, projections, mutation lifecycle, and state tokens advance atomically.
6. Recovery after a crash preserves uncertainty and cannot silently duplicate a mutation.
7. The GUI observes only committed effective states, never intermediate reconciliation writes.

Ordered Email query membership follows the additional invariants in
[`QUERY_WINDOWS.md`](QUERY_WINDOWS.md). A mailbox or search mutation invalidates affected query
windows to locally projected coverage inside the same projection transaction; cached object counts
are never treated as proof of authoritative ordered pagination.

All Email materialization paths, including explicit pagination and notification-anchored queries,
must rebase active Email projections after writing confirmed server objects and before publishing a
cache change. A specialized page loader is not permission to bypass the mutation journal or expose
a raw server snapshot to the GUI.

Unmatched mail push is reconciled account-wide before any query is refreshed. One sequential JMAP
envelope obtains `Mailbox/changes`, `Email/changes`, and result-referenced `/get` materialization.
The confirmed pre-change and fetched Email documents determine which cached queries can actually
have changed. Keyword-only updates therefore commit object metadata and Mailbox counts without
querying unrelated mailboxes. State tokens are opaque: equality suppresses a push, while a
different token is resolved through `/changes`; tokens are never ordered lexically.

## Extension Checklist

Every new stateful JMAP mutation must:

1. define a consistency domain and typed durable payload;
2. validate capability and rights before projection;
3. atomically append the mutation and materialize its effective cache state;
4. classify per-object success, definitive rejection, and transport ambiguity separately;
5. atomically accept or restore the projection;
6. make every refresh either fence the domain or rebase its active mutations;
7. define correlation and safe-retry behavior for unknown outcomes;
8. add deterministic tests for success, rejection, ambiguity, stale refresh, and recovery.
