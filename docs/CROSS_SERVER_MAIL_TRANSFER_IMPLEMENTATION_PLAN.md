# Cross-Server Mail Transfer Implementation Plan

## Status

This document defines the implementation plan for copying and moving Email objects between
mailboxes that may belong to different JMAP accounts and different configured JMAP servers.

The feature extends the existing **Move to** and **Copy to** actions and mailbox-tree drag and drop.
It deliberately preserves the current same-account mailbox-membership path rather than replacing it
with a slower generic transfer mechanism.

Cross-server transfer and mail export both need exact scope enumeration, complete raw RFC 5322
materialization, file-backed MailVault access, and bounded long-running work. Those facilities must be
implemented as shared daemon infrastructure rather than hidden inside the transfer coordinator. Mail
export remains a separate application workflow with its own persistence and filesystem semantics; see
[MAIL_EXPORT_IMPLEMENTATION_PLAN.md](MAIL_EXPORT_IMPLEMENTATION_PLAN.md).

The central safety rule is:

> A cross-account move is a confirmed copy followed by source cleanup. Javelin must never remove the
> source until it has durably established that the destination Email exists.

Cross-account moves are not atomic. Partial completion is therefore a first-class durable state, not
an exceptional corner case to hide with rollback.

## Goals

- Copy and move selected mail to any writable mailbox on any configured mail account.
- Preserve the current whole-Thread selection semantics and mixed-mailbox residency rules.
- Use the cheapest correct JMAP operation for each source/destination topology.
- Preserve the original RFC 5322 message bytes across independent servers whenever the destination
  accepts them unchanged.
- Preserve `receivedAt` and Email keywords for newly created destination Emails.
- Keep transfers bounded in memory and by the negotiated JMAP upload/request/object limits.
- Survive process restart, authentication interruption, network loss, ambiguous transport outcomes,
  partial server rejection, and a source-cleanup failure without duplicating or losing mail.
- Integrate with durable Undo/Redo rather than creating an irreversible exception to normal mail
  actions.
- Expose destinations consistently through context menus, toolbar drop-down menus, and drag and drop.
- Keep credentials, JMAP method construction, and writable cache access in the daemon/JMAP layers.
- Build exact mail-scope enumeration and raw-source materialization as reusable infrastructure for
  transfer, export, offline storage, and other future raw-message consumers.

## Non-goals

- Do not synchronize arbitrary mailbox trees between accounts.
- Do not automatically create matching destination mailboxes.
- Do not force the destination server to preserve the source JMAP Email id, blob id, or Thread id.
- Do not invent cross-server server-side atomicity that JMAP does not provide.
- Do not silently strip keywords, alter message content, or delete a confirmed destination copy to
  make a failed move appear atomic.
- Do not make generic mail transfer responsible for migrating an active local compose session. Active
  drafts need an explicit compose-aware policy described below.
- Do not make the transfer journal double as a mail-export journal. Export consumes the same raw-mail
  source infrastructure but has different destination, checkpoint, overwrite, cancellation, and
  completion semantics.

## Existing behavior to preserve

The current same-account implementation has several important semantics that remain correct:

- `MessageSelection` retains whole-Thread intent until the daemon has complete authoritative Thread
  membership and child Email coverage.
- The open mailbox is selection context, not proof that every visible Email resides there.
- For a move from a mailbox view, an Email actually resident in the source mailbox loses that source
  membership only. An expanded Thread member not resident in the view mailbox is moved from its real
  cached residency instead.
- A search selection has no implied source mailbox; moving from search operates on each Email's real
  source residencies.
- Copying to a mailbox that already contains an Email is a no-op for that Email.
- Destination rights and source-removal rights are checked before a same-account optimistic patch is
  admitted.
- Large whole-Thread actions retain one logical user operation while preserving per-Email results.

Cross-account transfer must start from these resolved per-Email semantics rather than from the
representative row ids or from the active mailbox id alone.

## Transfer topologies

A single user-facing Move/Copy command has three protocol implementations.

### 1. Same JMAP account

```text
source connection == destination connection
source JMAP account == destination JMAP account
```

Keep the existing `Email/set` mailbox-membership mutation path. No new Email object is created and no
raw MIME transfer occurs.

This remains the fastest path and retains the current optimistic projection and mail-patch Undo
implementation.

### 2. Different JMAP accounts on the same JMAP session

```text
source connection == destination connection
source JMAP account != destination JMAP account
```

Use `Email/copy`.

For a newly copied Email set only values valid in the destination account:

- destination `mailboxIds`;
- source `keywords`;
- source `receivedAt`.

Never copy source mailbox ids. They are scoped to the source account and are not valid in the
destination.

Do not normally use `onSuccessDestroyOriginal` for Move. Its implicit source operation destroys the
source Email object. That is wrong when the user's move should remove only one source mailbox
membership or otherwise leave the source Email in another mailbox. Source cleanup must be planned
explicitly per Email after destination confirmation.

### 3. Different configured JMAP sessions/servers

```text
source connection != destination connection
```

Use the RFC 5322 representation:

```text
source Email
  -> acquire raw message/rfc822 into the MailVault
  -> stream file to destination uploadUrl
  -> Email/import(blobId, destination mailbox, keywords, receivedAt)
  -> confirm/materialize destination Email
  -> for Move only: perform exact source cleanup
```

`Email/import` is preferable to reconstructing an Email with `Email/set create`: it preserves the
actual message representation and lets the destination server parse/thread it normally.

## Phase 0: Correct account identity before cross-server routing

### Problem

This is a prerequisite, not cleanup that can be deferred.

A JMAP `accountId` is scoped to a JMAP session. Two unrelated servers may both expose a mail account
named `u1`, `default`, or any other identical id. Javelin currently stores `accounts.account_id` as a
primary key and many mail/cache/runtime APIs address an account by remote `accountId` alone.
`owner_account_id` records which cached account came from a configured connection, but it does not
namespace the primary key.

Cross-server transfer makes collisions a normal supported configuration. Routing either side of a
transfer by remote `accountId` alone could therefore select the wrong credentials, cache rows,
mailboxes, or Email objects.

### Target model

Introduce a first-class connection-qualified account identity. Prefer a stable local surrogate key
rather than concatenating opaque remote identifiers into strings:

```text
MailAccountKey          stable local cache/application identity
connectionId            configured JMAP connection identity
remoteAccountId         JMAP accountId, scoped by connectionId
```

The persisted account relation should enforce:

```text
PRIMARY KEY (mail_account_key)
UNIQUE (connection_id, remote_account_id)
```

All child mail state should reference `mail_account_key`. Wire calls still use `remoteAccountId`.
Application routing values that cross process or service boundaries should carry either the stable
key or a typed locator that contains enough information to resolve it without guessing:

```cpp
struct MailAccountLocator {
    std::string connectionId;
    std::string accountId; // remote JMAP accountId
};
```

Do not expose credentials in that locator.

### Migration and coverage

The migration must cover every account-keyed reader, writer, state token, query window, Thread,
Email, mailbox, vault reference, optimistic journal, background job, settings selection, notification
route, workspace tab, and IPC value. Prefer changing the model rather than adding transfer-specific
lookup exceptions.

Add a deterministic fixture with two configured connections that both expose:

```text
accountId = "u1"
mailboxId = "inbox"
emailId   = "m1"
```

and prove that browsing, mutations, sync, raw-source lookup, and transfer routing remain isolated.
This fixture is the acceptance gate for the rest of the feature.

If account-identity hardening is split into a separate implementation branch, cross-server transfer
must remain disabled until it lands.

## Phase 1: Typed transfer command and destination model

Do not stretch `MailboxSelectionMutationIntent` into a structure containing optional source and
destination connections. Same-account membership mutation and cross-account transfer have different
failure, persistence, and identity semantics.

Add focused types along these lines:

```cpp
enum class MailTransferOperation { Copy, Move };

struct MailTransferEndpoint {
    MailAccountLocator account;
    std::string mailboxId;
};

struct MailTransferIntent {
    MailAccountLocator sourceAccount;
    std::optional<std::string> sourceMailboxId;
    MessageSelection selection;
    MailTransferEndpoint destination;
    MailTransferOperation operation;
};
```

A search source keeps `sourceMailboxId = nullopt`, exactly as today.

Add one typed `MailCommandPort` operation for transfer. The daemon decides whether that intent maps
to the existing same-account mutation path, same-session `Email/copy`, or cross-server import path.
The GUI must not know or choose the protocol strategy.

Extend the JVIP codec, validation, remote port, daemon action handler, and service composition with
the typed command. Boundary validation must reject empty connection/account/mailbox identifiers and
invalid operation values before application execution.

## Phase 2: JMAP and resource-transfer primitives

### `Email/copy`

Add typed `EmailCopyRequest`/`EmailCopyResponse` support to the mail method layer, including:

- `fromAccountId`;
- destination `accountId`;
- `ifFromInState` and `ifInState`;
- per-creation source Email id plus destination mailbox/keywords/receivedAt overrides;
- `onSuccessDestroyOriginal = false` for this workflow;
- full created response containing destination id/blobId/threadId/size;
- structured `notCreated` errors.

### `Email/import`

Add typed `EmailImportRequest`/`EmailImportResponse` support including per-item:

- uploaded `blobId`;
- destination mailbox set;
- keywords;
- `receivedAt`;
- created id/blobId/threadId/size;
- structured `notCreated` errors.

The existing generic `SetError` is insufficient for duplicate import/copy handling because it drops
`existingId`. Extend the typed error representation so `alreadyExists` retains that id.

### Streaming upload

The current HTTP resource transport streams downloads to files but request bodies are `QByteArray`.
Do not implement mail transfer by loading a multi-gigabyte `.eml` into RAM.

Add a file-backed upload primitive to `AbstractTransport`, for example `sendFromFile`, implemented by
`QtNetworkTransport` using a file/QIODevice body. The refreshing/authentication wrapper must support
the same path.

Use it for raw-message transfer and consider migrating attachment/media upload callers to the shared
primitive where useful. This is infrastructure, not transfer policy.

Before upload:

- read the destination Core capability;
- reject a message larger than advertised `maxSizeUpload` without transferring it;
- bound simultaneous uploads by advertised `maxConcurrentUpload` when present;
- continue honoring request/object limits for JMAP method calls.

An upload is procedural and does not create an Email object. An ambiguous/repeated upload may leave
an unreferenced server blob, but retrying the upload itself must not be confused with retrying
`Email/import`.

## Phase 3: Shared mail-scope resolution and raw-message materialization

Do not implement the expensive source side as transfer-private helpers. Introduce focused daemon
infrastructure that can be consumed by both this workflow and
[mail export](MAIL_EXPORT_IMPLEMENTATION_PLAN.md):

```text
MailScopeResolver
    selection/mailbox/account scope
        -> stable exact Email manifest

RawMailMaterializer
    Email manifest item + captured blob identity
        -> matching raw RFC 5322 MailVault object
        -> file-backed lease/path
```

`MailScopeResolver` owns complete enumeration and exact identity, not transfer policy. It must be able
to resolve a `MessageSelection` now and be extendable to mailbox/account export without making either
caller depend on GUI rows or bounded visible query windows. Whole-Thread selection still requires
complete authoritative Thread materialization before its manifest is final.

`RawMailMaterializer` owns reuse of a matching vault object, bounded download of a requested captured
blob identity when raw source is absent, file-backed leases, eviction protection, and
waiting-for-network/auth/space behavior. It must not silently substitute a newer blob when a durable
consumer asks for an older captured revision. It also must not know whether the consumer intends to
upload the bytes to another server, write an `.eml`, append mboxrd, index the message, or retain it
for offline use.

The existing single-message `requestMessageSource()`/`MessageSourceDownload` path returns a
`QByteArray` to the GUI and is suitable for today's **View Message Source** action, not for bulk
transfer/export. Do not build the shared service on that API. Introduce a daemon-side file-backed
source handle and migrate the viewer later only if doing so remains behaviorally simple.

For transfer, after `MessageSelection` has been fully materialized, resolve the exact Email set and
capture, per Email:

- source Email id;
- source mailbox memberships;
- source keywords;
- `receivedAt`;
- source blob id and size;
- the exact source cleanup plan for Move;
- the state/generation needed for later validation;
- whether the Email is associated with an active local compose session.

The cleanup plan must be derived before destination work starts so later UI/cache changes cannot
reinterpret what the original command meant.

For a cross-server transfer, ask `RawMailMaterializer` for an already-current MailVault raw source
when available. Otherwise it reuses the existing bounded message-source download mechanism to stream
`message/rfc822` into the vault. Do not fetch body parts and reconstruct MIME.

Do not obtain that source through an API that materializes the vault object as a `QByteArray`.
Introduce a file-backed MailVault lease/path abstraction so resource transports and filesystem
consumers can stream the stored object directly. A transfer lease must pin the content against
eviction for the lifetime of the active transfer, including across a restart when the transfer
journal says the source is ready. Release the operation pin only after the item is terminal or
transfer ownership has been handed to an Undo history pin.

A source raw object should be content-addressed through the existing MailVault and shared between
multiple transfer items when deduplication naturally applies. If the vault cannot reserve enough
space to spool a required source, put the work into the existing waiting-for-space state rather than
falling back to an in-memory MIME copy.

## Phase 4: Durable transfer journal and state machine

Cross-account transfer spans multiple servers and procedural blob I/O. It should not be represented
as one generic `EmailMutationJournal` record.

Add a daemon-owned transfer journal with one operation row and bounded per-Email item rows. The
application coordination layer owns the workflow and partial-failure policy; typed JMAP services own
individual protocol operations.

A useful per-item state machine is:

```text
Prepared
  -> AcquiringSource
  -> SourceReady
  -> Uploading              # cross-server only
  -> Uploaded               # cross-server only
  -> CreatingDestination    # Email/import or Email/copy
  -> DestinationUnknown     # ambiguous creation outcome
  -> DestinationConfirmed
  -> RemovingSource         # Move only
  -> SourceCleanupUnknown   # ambiguous source outcome
  -> Complete
```

Terminal/settled alternatives include:

```text
RejectedBeforeCopy
DestinationRejected
CopiedSourceRetained
CompleteCopy
CompleteMove
CancelledBeforeCopy
```

Persist enough data to resume safely after restart:

- operation id and user-visible grouping;
- source/destination typed account locators;
- source Email identity and snapshot;
- cleanup intent;
- source content hash/vault reference;
- uploaded destination blob id, when obtained;
- destination created/reused Email id;
- source and destination state checkpoints;
- per-phase dispatch/unknown state;
- typed failure/partial outcome.

The operation row should aggregate counts and byte progress without retaining every MIME object in
memory.

### Crash rule

Recovery may continue from the last proven phase. It may never infer that an in-flight destination
creation failed merely because the daemon restarted.

Most importantly, `RemovingSource` is reachable only after `DestinationConfirmed` is durably
committed.

## Phase 5: Destination creation and duplicate handling

### New destination Email

For a genuinely new Email, preserve:

- raw RFC 5322 content;
- source `receivedAt`;
- source JMAP keywords;
- only the selected destination mailbox membership.

The destination server assigns its own Email id, blob id, Thread id, and possibly a transformed blob
if it repairs invalid source MIME. Accept those returned identities as authoritative.

Never copy the source Thread id. Thread membership is a destination-server decision.

### `alreadyExists`

Both `Email/copy` and `Email/import` may reject a duplicate with `alreadyExists` and an `existingId`.
Treat that as a resolvable destination identity rather than immediately failing the action:

1. Fetch the existing destination Email authoritatively.
2. If it is already in the requested destination mailbox, the destination requirement is satisfied.
3. Otherwise add only the requested destination mailbox membership with an exact destination
   `Email/set` patch after checking rights.
4. Preserve the existing destination Email's pre-existing keywords and other mutable metadata.

Do **not** overwrite an already-existing Email's seen/star/tag state from the source. A user asking to
copy mail into a mailbox did not ask to mutate an independent destination copy that happened to
already exist.

Record whether the destination Email was newly created or pre-existing because Undo must treat them
differently.

### Destination cache materialization

The create/copy response does not provide enough Email data to render the normal list row. After
creation/reuse, fetch the full destination Email in a bounded `Email/get` and commit it through the
normal optimistic-consistency/materialization transaction.

Refresh/materialize destination Mailbox counts as part of the normal acceptance envelope where
possible.

Do not guess ordered query-window insertion from `receivedAt`. A visible query may use another sort
and JMAP `/changes` does not provide a universally authoritative insertion position. Mark affected
destination windows stale/partial as required and schedule their normal bounded reconciliation.

## Phase 6: Ambiguous destination creation

This is one of the highest-risk cases.

A server may successfully process `Email/import` or `Email/copy` and the response may then be lost.
Blindly retrying can create a duplicate on servers that permit duplicate message content. Therefore:

- mark the transfer item `DestinationUnknown` after a dispatched request with an ambiguous transport
  outcome;
- keep the source untouched;
- do not retry destination creation until targeted reconciliation proves whether the first creation
  took effect.

Reconciliation should use the captured destination Email state and a bounded `Email/changes` plus
`Email/get` of newly created candidates. Correlate using the strongest available evidence, including
creation timing/state, destination mailbox, Message-ID/content metadata, receivedAt, uploaded/source
content hash, and blob identity where it remains meaningful.

A server may repair invalid MIME during import and return a different blob id, so blob equality alone
cannot be the universal correlation rule.

If Javelin can uniquely prove the destination object, adopt its id and continue. If it can prove the
creation did not happen, retry is safe. If neither can be proved, keep the item blocked/unknown and
surface that state rather than risking a duplicate.

The same principle applies to an ambiguous same-session `Email/copy` create.

## Phase 7: Source cleanup for Move

Only a confirmed destination is eligible for source cleanup.

Apply the source-residency semantics already used by same-account moves. For each Email, the captured
cleanup plan says which source mailbox memberships the original user action intended to remove.
Immediately before cleanup, perform the normal authoritative precondition check so a relevant remote
change is not overwritten blindly. Reconciliation is objective-based: if the exact source
membership Javelin intended to remove is already absent, that part of cleanup is satisfied; if the
source Email has already been destroyed after the destination was confirmed, the removal objective
is also satisfied. Incompatible newly added/changed memberships remain a conflict rather than being
silently removed.

### Membership removal versus destruction

RFC 8621 requires an Email in the mail store to belong to at least one mailbox at all times.
Therefore:

- if source cleanup leaves one or more source mailbox memberships, submit exact `Email/set`
  `mailboxIds/<id> = null` patches;
- if cleanup would leave zero source memberships, destroy the source Email instead.

This is especially important for a Move from search, where there is no source view mailbox and the
existing semantics may remove all actual residencies.

### Source failure after destination success

If source cleanup is rejected or remains ambiguous:

- keep the confirmed destination Email;
- never delete the destination as speculative compensation;
- classify the item as `CopiedSourceRetained` or `SourceCleanupUnknown`;
- reconcile the source independently;
- report that the copy succeeded but the source could not yet be removed.

This is the data-preserving failure direction. Attempting to restore apparent atomicity by deleting
the confirmed destination creates unnecessary data-loss risk.

## Phase 8: Batch policy, limits, and scheduling

One user selection is one transfer operation, but each Email retains an independent outcome.

Use bounded batches/concurrency. A later failure must not roll back earlier confirmed transfers.
Aggregate user-visible results such as:

```text
18 moved
2 copied but retained on source
1 already present at destination
1 failed: destination over quota
```

Respect:

- source/destination `maxObjectsInGet`;
- `maxObjectsInSet`;
- `maxCallsInRequest`;
- `maxSizeRequest`;
- destination `maxSizeUpload`;
- destination `maxConcurrentUpload`;
- WorkScheduler foreground/background fairness.

For cross-server transfer, stream one or a bounded number of raw messages rather than accumulating
message bodies in RAM.

A transfer that is waiting for network or authentication should remain a durable work item. Source
and destination connections have independent authentication state; pausing one must not corrupt the
other or lose the operation checkpoint.

If cancellation is exposed, honor it only at safe item boundaries. A cancellation must never erase a
confirmed destination merely to make an already-started Move look unstarted. Once a Move item has a
confirmed destination, either finish its source cleanup or retain the explicit partial state before
stopping subsequent items.

## Phase 9: Keywords, tags, drafts, and metadata policy

### Keywords

For a newly created destination Email, copy the source keyword set exactly unless the destination
server rejects it. Preflight destination `maySetSeen` when `$seen` is present and `maySetKeywords`
for other keywords; do not intentionally submit metadata the cached rights already prove cannot be
set. Do not silently retry after stripping keywords when rights, `tooManyKeywords`, or another
keyword constraint blocks preservation; report the per-item failure.

The destination account may not have a Javelin local tag definition/color for a custom keyword. The
server keyword and Javelin's tag presentation metadata are separate things. V1 should preserve the
keyword without automatically inventing or copying local display/color definitions. A later UI
feature may offer tag-definition synchronization explicitly.

For an `alreadyExists` destination Email, preserve its existing keyword set as described above.

### Drafts

An active compose session is more than a raw Email: Javelin also owns compose revisions, attachment
manifests, inline assets, and draft replacement identity. Generic raw-message transfer cannot safely
migrate that editing state.

V1 should reject moving/copying an Email that is currently attached to an active local compose
session with a clear explanation. A closed server-side draft with no active local compose state may
be transferred as an ordinary Email, including `$draft`, unless testing uncovers a server-specific
interoperability problem.

Do not silently close or migrate a compose session as a side effect of drag and drop.

### Message integrity

Raw-message import preserves headers, body structure, inline content, and attachments as one RFC
5322 object. A destination server is allowed to repair invalid MIME; when it does, its returned blob
identity is authoritative. Javelin should not claim byte identity after such a transformation.

## Phase 10: Undo/Redo and MailVault retention

Cross-account transfer must have a dedicated typed history payload. Reusing `MailPatchHistory` would
lose destination identities and the distinction between a newly created destination Email and a
pre-existing duplicate.

### Copy Undo

If the destination Email was newly created:

- remove/destroy the destination effect according to its current memberships and validated
  preconditions;
- do not blindly destroy it if unrelated remote work has since added memberships or otherwise made
  the inverse unsafe.

If `alreadyExists` reused an existing destination Email:

- undo only the destination mailbox membership Javelin added;
- never destroy the pre-existing Email;
- never restore/replace its pre-existing keywords.

### Move Undo

If the source Email survived because Move removed only some memberships:

- restore the exact source memberships removed by the command;
- reverse the destination effect with the same created-vs-pre-existing distinction as Copy Undo.

If Move had to destroy the source Email because no source membership could remain:

- recreate the source Email from the retained raw MIME with its original memberships, keywords, and
  `receivedAt`;
- accept a new source Email id/blob id/Thread id and update the history payload;
- then reverse the destination effect.

### Vault retention

The current MailVault reference retention only distinguishes `full_sync` and `evictable`. A Move
whose Undo may need to recreate a destroyed source must pin raw content independently of normal
online-cache eviction.

Extend the vault retention model with explicit operation/history retention relations rather than
turning an Email ref into an overloaded string sentinel. A content hash may have several retention
reasons at once: normal Email/offline retention, an active-transfer lease, and an Undo-history pin.
Transfer pins survive restart and are released on terminal operation state; a destructive-Move pin
is handed to history before that release. The history pin is released when the corresponding
history entry is pruned or becomes irreversibly invalid.

Never implement Undo of a destructive source cleanup by assuming the old source server blob remains
available.

### Redo and blocked partial history

Redo repeats the logical transfer using the then-current object identities and records newly assigned
ids. Unknown destination creation or uncompensated partial source cleanup blocks history execution in
the same way other `BlockedUnknown`/`BlockedPartial` operations do.

## Phase 11: Shared destination-menu presentation

The current destination builder is same-account and directly constructs `QMenu` actions. Separate
presentation data from the widget population so context menus and toolbar menus cannot drift.

Build a read-only destination model containing all configured mail-capable accounts and writable
mailboxes. Every destination entry has a typed connection-qualified account identity.

Suggested menu layout:

```text
Move to
  Inbox
  Archive
  Projects
  ─────────────
  Work Account                >
      Inbox
      Archive
      Customers               >
          Example Ltd
  Personal / Other Server     >
      Inbox
      Archive
```

For the active account, retain today's direct mailbox entries so common operations do not become one
submenu deeper. Add each other account as a submenu after a separator.

Within account submenus, preserve the normal special-use-first and hierarchical mailbox presentation.
Omit mailboxes without `mayAddItems`. Omit an account submenu entirely if it has no writable
mailbox.

Account labels must be human-readable and unambiguous. Prefer the configured connection display name
plus the cached JMAP account name where needed. If names collide, disambiguate with login/server
context rather than exposing opaque remote ids.

Rebuild menus when shown so account/mailbox rights and configuration changes are reflected without
keeping a second long-lived GUI source of truth.

### Toolbar behavior

Turn the Move and Copy toolbar actions into proper drop-down menu actions rather than only opening a
cursor-position popup from a triggered signal. Both toolbar and context-menu actions must use the
same destination model/builder and execute the same typed command.

Keyboard activation must be able to open and traverse the destination hierarchy; accessible names
should include account context for cross-account submenu entries.

## Phase 12: Drag and drop

### Stable drag payload

The current drag MIME payload contains a source account id plus representative Email ids, and the
drop handler then ignores those ids and re-reads the current GUI selection. That is fragile for a
longer asynchronous cross-account operation and does not encode whole-Thread intent explicitly.

Replace it with a versioned internal payload containing:

- source connection-qualified account locator;
- optional source mailbox id/search context;
- the exact `MessageSelection` captured at drag start;
- a payload schema version.

The daemon still materializes/revalidates the selection. The payload is intent, not trusted state.

This prevents a selection change during the drag from changing which messages are transferred and
prevents collapsed Threads from being narrowed to representative Email ids.

### Copy versus Move gesture

Advertise both `Qt::MoveAction` and `Qt::CopyAction`:

- normal drag defaults to Move;
- Ctrl-drag selects Copy, following desktop convention;
- Shift may force Move where Qt/platform conventions provide it.

Use the resolved Qt drop action to construct `MailTransferOperation`.

### Drop validation

A mailbox is drop-enabled only when it is a real mailbox and cached rights say `mayAddItems`. Account
roots, separators, pending-created mailboxes, and non-writable mailboxes are not valid destinations.

Do not retain the current `sourceAccountId == destinationAccountId` restriction. Same-account,
same-session cross-account, and cross-server destinations all pass through the same typed transfer
command and daemon validation.

The drag cursor/drop indicator should accurately show Copy versus Move and reject invalid targets
before drop where cached information is sufficient.

## Phase 13: User feedback and error presentation

Same-account operations can keep their existing optimistic `Queued move/copy` feedback.
Cross-account/server transfer may involve real byte transfer and should appear in the existing work
surface with meaningful progress:

- item count;
- bytes transferred when known;
- current source/destination account names;
- waiting-for-network/auth state;
- final partial summary.

Do not claim a Move succeeded merely because destination import succeeded. Distinguish:

- copied successfully;
- moved successfully;
- destination already contained the message;
- copied, but source retained;
- destination creation unknown;
- failed before destination creation.

Authentication errors should route through the normal per-connection sign-in coordinator. A problem
with the destination connection must not incorrectly mark the source connection as failed.

## Phase 14: Test matrix

### Account identity

- two unrelated sessions with identical remote account/mailbox/Email ids stay isolated;
- destination menus show both independently;
- command routing resolves correct credentials for both sides.

### Selection semantics

- one Email;
- multi-select;
- collapsed Thread with uncached children waits for complete materialization;
- expanded Thread member outside the view mailbox uses real residency;
- mailbox view versus search source;
- destination already contains some selected messages.

### Topology

- same account remains exact `Email/set` without raw transfer;
- same session/different accounts uses `Email/copy`;
- different sessions uses download/upload/`Email/import`;
- cached raw source skips source download;
- large MIME transfer remains file-streamed and memory-bounded.

### Rights and limits

- destination lacks `mayAddItems`;
- source mailbox lacks `mayRemoveItems`;
- source destruction is not permitted;
- destination `maxSizeUpload` too small;
- `overQuota`;
- `tooManyKeywords`;
- request/object/upload concurrency limits are respected.

### Duplicate handling

- `alreadyExists` already resides in target mailbox;
- `alreadyExists` requires adding target mailbox;
- pre-existing destination keywords are preserved;
- Undo never destroys a reused pre-existing Email.

### Move cleanup

- source Email has another mailbox and only requested membership is removed;
- last source membership causes source destroy;
- search Move destroys when all real residencies are removed;
- source changed remotely between snapshot and cleanup causes a conflict rather than blind mutation.

### Partial and ambiguous outcomes

- destination reject leaves source untouched;
- one item fails while earlier items remain complete;
- copy succeeds and source removal rejects;
- copy succeeds and source removal response is lost;
- destination create response is lost and reconciliation proves success;
- reconciliation proves no create and safely retries;
- reconciliation cannot prove either result and remains blocked without retry;
- daemon crash at every persisted state-machine edge resumes without duplicate creation or premature
  source deletion.

### Undo/Redo

- undo/redo newly created copy;
- undo reused duplicate membership;
- undo Move where source Email survives;
- undo Move where source Email was destroyed and must be re-imported;
- raw source remains pinned until destructive-Move history is no longer needed;
- blocked unknown/partial outcome blocks history correctly.

### GUI

- context Move/Copy menus show active-account direct destinations and other-account submenus;
- toolbar drop-downs use the same destination set/order;
- writable rights filter applies in all surfaces;
- duplicate account names are disambiguated;
- keyboard and screen-reader traversal communicates account and mailbox context;
- normal drag moves;
- Ctrl-drag copies;
- drag of collapsed Thread preserves Thread intent;
- changing selection during a drag does not change the transfer payload.

## Implementation order

1. Land the connection-qualified account identity migration and collision fixture.
2. Add `Email/copy`, `Email/import`, structured `existingId`, and file-stream upload primitives.
3. Add shared exact mail-scope enumeration, raw-message materialization, and file-backed MailVault
   lease primitives. Keep them independent of transfer/export policy.
4. Add the durable transfer journal/repository and recovery state machine with no GUI caller yet.
5. Add transfer source snapshot plus destination copy/import with ambiguity reconciliation, consuming
   the shared source services rather than a transfer-private download path.
6. Add exact Move source cleanup, partial-outcome policy, and bounded batching/work scheduling.
7. Add destination cache materialization and invalidation.
8. Add typed Undo/Redo payloads and history-owned MailVault retention.
9. Expose the typed transfer command over JVIP.
10. Refactor destination presentation and add other-account context/toolbar submenus.
11. Replace the drag payload and enable cross-account Move/Copy drops.
12. Add work/progress and partial-result presentation.
13. Run focused production-path tests throughout, then the normal full build/test suite and a separate
    regression review before merging.

The separate mail-export work may proceed in parallel once step 3 is stable and account identity is
available. Export is not a transfer completion dependency; it consumes the shared foundation without
joining the transfer journal or state machine.

## Acceptance invariants

The feature is not complete unless all of the following hold:

1. A remote JMAP account id is never treated as globally unique across configured connections.
2. Same-account transfer still uses mailbox-membership mutation and does not round-trip MIME.
3. Cross-server transfer never reconstructs a message from rendered/body-part data when raw RFC 5322
   source is available from the server.
4. Raw MIME upload is file-streamed and bounded by negotiated upload limits.
5. A Move never begins source cleanup before destination existence is durably confirmed.
6. A rejected destination copy/import leaves the source unchanged.
7. An ambiguous destination creation is never blindly retried.
8. A confirmed destination is never deleted merely to compensate for failed/unknown source cleanup.
9. Source cleanup never leaves an Email with zero mailbox memberships; it destroys the Email when
   removal of the final membership is the intended move.
10. `alreadyExists` never causes Javelin to overwrite unrelated mutable state on a pre-existing
    destination Email.
11. Whole-Thread actions never narrow to a representative or opportunistically cached subset.
12. Batch partial success remains represented per Email and survives restart.
13. Undo can recreate a source Email destroyed by Move without depending on the old server retaining
    its blob.
14. Context menus, toolbar menus, and drag/drop use the same connection-qualified destination and
    transfer semantics.
15. Exact scope enumeration and raw-message materialization are reusable daemon services, not
    transfer-journal implementation details, and bulk consumers never require full MIME in GUI RAM.
16. The GUI never receives credentials, performs upload/import/copy calls, or writes transfer/cache
    state directly.
