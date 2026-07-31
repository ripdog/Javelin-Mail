# Undo and Redo Architecture

Javelin implements Undo and Redo as durable application commands. It does not use
`QUndoStack`: a history command may require an authoritative JMAP read, optimistic mutation
projection, asynchronous transport, compensation, and restart recovery.

## Ownership and boundaries

The application layer owns history policy, user-visible grouping, labels, stack ordering, and
domain executor routing. Domain executors consume typed payloads and call typed JMAP or local
preference services. The JMAP library owns protocol validity, state tokens, mutation-journal
causality, cache projections, and reconciliation. GUI code selects user intent, renders manager
state, and presents typed failures; it never constructs inverse JMAP patches or reads generic
mutation rows.

`DaemonServices` owns one `UndoManager`, so history and deferred sends outlive any `MainWindow`.
Nested work uses `CommandOrigin::SystemChild` and never registers a second history entry.

## Durable entry lifecycle

Every entry has a stable UUID, domain and command kind, versioned typed payload, label, stack and
monotonic stack order, status, timestamps, and optional operation-group, expiry, failure, and
recovery metadata.

Normal mutations follow this sequence:

1. Persist a `Preparing` entry with an application-generated operation-group ID.
2. Queue the typed domain mutation and its optimistic projection using that group ID.
3. Discard the entry if no object was queued or every object failed definitively.
4. Remove definitively rejected objects from a batch payload.
5. Mark the remaining entry `Ready` once it is durably queued or accepted.
6. In the same history transaction, place it on Undo and clear Redo.
7. Preserve `Unknown` or partial results as blocking states until targeted reconciliation proves
   the effective result.

Only one history command executes at a time. Undo transitions `Ready` to `ExecutingUndo`; Redo
transitions it to `ExecutingRedo`. A successful executor result may return an updated payload, for
example after a recreated object receives a new server ID. Payload replacement and movement to the
opposite stack are one transaction. Conflicts and definitive failures restore `Ready` without
moving the entry. Unknown and uncompensated partial outcomes remain blocking.

The stack is last-in-first-out by `stack_order`. A successful normal mutation clears all Redo
entries. A multi-object user action is one entry. Targeted cancellation of a scheduled send may
remove that non-top entry and clears Redo because the history branch changed.

## Preconditions and consistency

Server-backed Undo and Redo never apply a blind inverse to cached state. The executor:

1. reads the authoritative affected objects;
2. validates only the properties changed by the command;
3. preflights every object in a batch before changing any object;
4. captures the current data-type state token;
5. submits the exact direction-specific mutation using `ifInState` where supported; and
6. treats `stateMismatch` as a conflict and refreshes the affected objects.

Unrelated remote changes do not block a command. A missing object, changed relevant property, lost
permission, or state mismatch does. Offline execution fails without moving the entry unless an
original mutation is proven to be local and undispatched, in which case the executor may cancel
that mutation and projection atomically.

JMAP set operations may partially succeed. The executor compensates every successfully changed
object back to its pre-command state. Successful compensation reports failure while retaining the
entry. Failed or ambiguous compensation marks `BlockedPartial`, records per-object outcomes, and
blocks subsequent history execution pending targeted reconciliation.

The mutation journal and operation history are deliberately separate. The journal is the source of
sync causality; history is durable user policy. Their shared operation-group and mutation IDs allow
crash recovery without conflating those responsibilities.

## Recovery

At startup, entries left in an executing state are correlated with their operation group. An
in-flight request is never assumed to have failed. A proven success completes the interrupted
stack transfer, a proven rejection restores `Ready`, and an ambiguous result becomes
`BlockedUnknown`. No inverse or duplicate command is permitted while its result is unknown.

Entries are bounded to 200 rows and approximately 32 MiB of serialized payloads. Pruning removes
the oldest non-executing completed entries first. It never removes executing, unknown, partial, or
scheduled-send entries.

## Impossible and expired entries

An irreversible action, such as permanent email deletion, creates an `Impossible` entry only after
the action is durably queued or accepted. A send entry becomes `Expired` at the atomic transition
to transport dispatch, not when its timer merely becomes due. Invoking either kind emits a typed
failure for a modal GUI error. After acknowledgement the manager deletes the entry without moving
it to Redo, allowing the next older entry to become reachable.

Ordinary conflicts and server failures retain their entry. The GUI may offer **Remove from
History** when the typed failure says that explicit removal is safe.

## Serialization

In-memory payloads are a closed `std::variant` of domain types. Persistent JSON contains a schema
version and a command-specific typed document; arbitrary application-level JSON patches, lambdas,
and `std::function` closures are forbidden. Unknown payload versions fail loudly and remain
recoverable for a future application version.

Object recreation preserves logical identifiers such as JSCalendar or contact UIDs while updating
the payload with new server IDs. Mail patch payloads store the exact forward and inverse mailbox
and keyword changes recorded when the normal command was prepared.

## UI routing

The Edit menu and shortcuts use the same focus-aware router. A focused `QLineEdit`, `QTextEdit`,
`QPlainTextEdit`, or editable `QComboBox` receives native Undo or Redo when available. Otherwise
the process-wide manager executes the top global entry. Both actions are disabled during execution
or blocking reconciliation and show the current entry labels when available.

`UndoManager` emits typed state and failure values. `MainWindow` owns `QMessageBox` presentation
and status-bar feedback. Domain executors report affected accounts, object types, and views so
refresh coordination remains outside the window.

## Deferred send boundary

Undo Send schedules a durable prepared draft and delays `EmailSubmission/set`. Cancellation and
dispatch serialize on the pending-send row, so exactly one wins. Dispatch atomically marks the send
`dispatching` and its history entry `Expired` before transport starts. Ambiguous transport remains
expired and is reconciled; it never restores Undo eligibility. Redo schedules the same logical
draft with a fresh delay, while editing an Undo-restored draft invalidates that Redo branch.
