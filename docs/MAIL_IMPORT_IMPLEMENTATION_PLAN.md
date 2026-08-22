# Mail Import Implementation Plan

## Status

This document defines the implementation plan for importing mail into Javelin from the two formats
mail export produces: raw `.eml` files (singly or as a directory tree) and mboxrd mailbox files
(singly or as a directory tree of `.mbox` files). See
[MAIL_EXPORT_IMPLEMENTATION_PLAN.md](MAIL_EXPORT_IMPLEMENTATION_PLAN.md) for the export contract this
feature consumes in reverse.

Import is surfaced through drag and drop onto the mailbox tree and through **File -> Import** menu
and context-menu actions, mirroring how export is exposed.

The central rule is:

> Upload the authoritative raw RFC 5322 bytes to the destination server and let `Email/import`
> create the Email objects. Never reconstruct messages from parsed body parts, compose values, or
> hand-serialized JMAP properties, and never write imported Emails into the cache directly — they
> materialize through normal synchronization.

A large import is long-running network and storage work. Like export, it must stay bounded in
memory, survive restart, pause safely at durable boundaries, and never require the GUI process to
remain alive.

## Relationship to existing infrastructure

Import reuses existing daemon infrastructure rather than forking new machinery:

- `Email/import` protocol types are already defined and tested in `jmap/api/MailMethods.h`
  (`EmailImportRequest`, `EmailImportResponse`, `emailImport()`,
  `serializeEmailImportRequest()`, `parseEmailImportResponse()`).
- `jmap/api/BlobUpload.h` provides `uploadBlobFromFile()`: file-backed streaming upload that enforces
  the negotiated `maxSizeUpload` and never loads whole messages into memory.
- The cross-server transfer executor already implements the per-item pipeline shape import needs:
  source acquisition -> blob upload -> destination `Email/get` state preflight ->
  `Email/import` with `ifInState` -> per-item durable phase transitions -> retryable-error
  classification into waiting states -> ambiguous-dispatch handling.
- Export established the durable-journal pattern: SQLite operation/mailbox/item tables
  (`Migrations61To69.cpp`), one semantic `background_jobs` entry per operation with a stable job-id
  prefix, WorkScheduler admission, and restart recovery. Import follows the same pattern with its
  own journal and job prefix; it does not reuse the transfer or export journals.

Like export versus cross-server transfer, import shares low-level primitives but owns separate
destination, duplicate, hierarchy, and failure semantics.

## Goals

- Import single `.eml` files, directories of `.eml` files, mboxrd `.mbox` files, and directory trees
  containing both layouts, including trees produced by Javelin account/mailbox export.
- Preserve raw message bytes end to end: read bytes from the source file, stream them through the
  JMAP upload endpoint, and reference the returned blob id. Do not normalize, refold, decode, or
  otherwise transform message content.
- Preserve received time. Derive `receivedAt` deterministically from available metadata instead of
  letting every imported message inherit an import-time date.
- Preserve mailbox hierarchy when importing an exported directory tree by recreating it under a
  user-chosen parent mailbox.
- Keep parsing, uploading, and server mutation entirely inside the daemon, off the GUI thread.
- Persist enough state to resume large imports safely after daemon restart without duplicating
  already-imported messages.
- Integrate with WorkScheduler/Task Center: progress, pause, resume, waiting-for-network/auth
  states, and clear terminal errors.
- Treat duplicates honestly: detect them where cheap, report them explicitly, and never silently
  count a skipped duplicate as newly created mail.

## Non-goals

- Do not support formats Javelin does not export. Maildir, PST, iCalendar, compressed archives, and
  generic "any mbox" migration are out of scope for the initial implementation.
- Do not restore keywords/read/starred state. Export deliberately does not preserve mutable JMAP
  state in `.eml`/mboxrd output; import cannot conjure it back. Private client headers such as
  `X-Mozilla-Status` are ignored, not interpreted. A future versioned sidecar manifest is the
  correct vehicle for full round-trip metadata, as noted in the export plan.
- Do not make import undoable as a mail-history operation. A bulk import creates thousands of
  server objects and must not flood Undo history; wrong imports are corrected by deleting the
  imported selection, not by undo.
- Do not write imported Emails, mailbox membership, or query windows into the cache from the
  importer itself. Imported state becomes visible through the ordinary synchronization path.
- Do not silently merge foreign mailbox hierarchies into existing mailboxes by name without an
  explicit user-visible mapping decision.
- Do not trust container framing hints such as mboxcl/mboxcl2 `Content-Length:` headers for record
  boundaries.

## Formats and detection

### Detection rules

Admission classifies each dropped/chosen path by content first, name second:

1. A directory is an EML tree if recursively it contains `.eml` files, and an mboxrd tree if it
   contains `.mbox` files; mixed trees combine both conventions exactly as Javelin's paired-layout
   export writes them (`.mbox` file plus sibling child-directory for a parent mailbox).
2. A regular file whose first bytes begin with `From ` followed by an envelope line is parsed as one
   mboxrd mailbox regardless of extension (this accepts `.mbx` and extensionless mbox files).
3. Any other regular file is treated as a single RFC 5322 message regardless of extension.

Admission rejects clearly non-message content early (binary magic, unparseable structure) with an
error naming the offending path, before a long-running job is queued.

### mboxrd reading rules

The reader is the exact inverse of `appendMboxRdRecord()`:

- Records are delimited by lines matching `From ` at the start of a line where the previous line is
  blank (or start-of-file), scanned incrementally over bounded buffers.
- Every line consisting of one or more `>` characters followed by `From ` loses exactly one leading
  `>`. This reverses the exporter's escaping. This is inherently lossy for messages that genuinely
  contain such lines; that is a property of mboxrd, not of Javelin's writer, and is documented
  behavior rather than a bug to fix.
- A missing final newline on the last record is tolerated.
- An empty file yields zero messages and is reported, not failed.

### From_ separator parsing

The `From_` envelope line is container metadata. Parse leniently:

- Sender: accept the exporter's space-to-underscore form and raw forms; never fail because the
  sender is unparsable.
- Date: try the exporter's C-locale asctime form, then common asctime variants, then give up
  quietly. An unparsable From_ date only means no explicit `receivedAt` contribution from the
  container.

## Metadata fidelity

For each imported message, derive `receivedAt` with this precedence:

1. The From_ separator date of the enclosing mbox record (the exporter wrote the captured
   `receivedAt` there);
2. otherwise omit `receivedAt` and let the server apply RFC 8621 semantics (time of the most recent
   `Received` header), which is normally the correct original receipt time for `.eml` sources.

Do not parse `Date:` headers to synthesize `receivedAt`; the server's Received-header rule is more
accurate than a header scan, and header scanning invites MIME-parsing drift inside the importer.

Keywords default to empty. `Email/import` requires at least one destination mailbox per message;
admission guarantees this by construction (target mailbox or recreated hierarchy).

If the server fixes malformed input (e.g., strips NUL octets) it returns a different blob id in the
import response. Record the returned blob id; do not treat the difference as an error. Uploaded
bytes remain byte-exact; server-side storage may legitimately differ.

## User-facing surfaces

### Drag and drop

- Dropping one or more files/directories whose detection matches an importable format onto a real,
  writable mailbox node in the mailbox tree opens the import dialog pre-targeted at that mailbox
  (or at that account root for tree imports).
- The mailbox tree model currently accepts only the internal message-drag MIME type. It is extended
  to also accept `text/uri-list` payloads carrying local file URLs. External drops and internal
  message moves/copy drags remain distinct code paths end to end; a file drop never becomes a
  message-move command.
- The GUI performs only presentation-level validation (existence, sniffed-format plausibility for
  cursor feedback). The daemon re-validates authoritatively at admission.
- Drop targets are limited to nodes representing real writable mailboxes (`mayAddItems`), consistent
  with internal drop policy. Dropping onto non-targets gives clear rejection feedback rather than
  silence.

### Menu and context menus

- **File -> Import Messages...** opens a file chooser (multi-select files or one directory) plus a
  destination account/mailbox picker.
- **File -> Import Backup Tree...** opens a directory chooser plus destination account and target
  parent mailbox, with an explicit "recreate folder hierarchy" choice.
- Real mailbox context menus gain **Import Into This Mailbox...**; account context menus gain the
  same entries as the File menu scoped to that account.
- Actions follow the existing KXMLGUI action-collection patterns used by the export actions.

### Import dialog

One small dialog mirrors the export dialog:

- detected summary: format, file/folder count, and — after a cheap daemon-side scan — message count
  and total size when knowable;
- destination account/mailbox (pre-filled from the drop target);
- hierarchy option for directory sources: recreate folders / import everything flat into the target;
- duplicate policy display (see below);
- explanation that large imports continue in Task Center and that missing connectivity pauses the
  job.

## Destination semantics

### Flat imports

Single `.eml` files, loose sets of `.eml` files, and single mboxrd files import into exactly one
destination mailbox: the drop target or the picker choice. No hierarchy is implied.

### Tree imports

Directory-tree sources recreate their layout under a chosen parent mailbox when the user selects
hierarchy recreation:

- The relative directory path (EML tree) or `.mbox` file path (mboxrd tree) maps to a mailbox path;
  the exporter's paired layout (`.mbox` file plus sibling directory) merges into one logical
  mailbox, exactly mirroring export.
- Missing ancestor mailboxes are created under the target parent through the daemon's mailbox
  protocol access, journaled in the import operation like any other item, and retried/resumed like
  any other item. These creations are part of the durable workflow, not interactive mutations, and
  produce no Undo entries.
- Path-to-mailbox resolution sanitizes components with the inverse of `ExportPathPlanner` rules:
  separators inside names, `.`/`..`, empty names, length bounds, and case-folding collisions get the
  same treatment; siblings whose sanitized names collide receive deterministic discriminators.
- Before creating a mailbox for a path segment, resolve against existing mailboxes under the same
  parent by sanitized name so repeated imports reuse an existing folder instead of duplicating it.
  Name-based matching is best effort and user-visible in the summary ("will reuse existing folder
  'Projects'"); it is not presented as identity.
- Mailbox creation failures (rights, invalid name, quota) fail only the affected subtree's items as
  `NoDestination`; other subtrees continue.

### Duplicate policy

Initial policy, shown in the dialog and applied durably:

- Within one operation, skip a later message whose SHA-256 matches an earlier accepted item; count
  it as skipped.
- Server-reported `alreadyExists` SetErrors (with `existingId`) count as skipped duplicates and
  record the existing id.
- No whole-mailbox pre-scan for cross-run deduplication in the first version; rerunning an import
  relies on the server's duplicate rejection plus the durable journal preventing intra-operation
  repeats. The result summary distinguishes created/skipped/failed counts so a partially rerun
  import is honest about what happened.

## Daemon command boundary

Typed intents mirror the export boundary. Conceptually:

```cpp
enum class MailImportSourceKind { SingleMessage, MessageFiles, MboxFile, DirectoryTree };

struct MailImportIntent {
    std::string accountId;                 // connection-qualified local account key
    std::optional<std::string> mailboxId;  // required unless DirectoryTree with hierarchy recreation
    MailImportSourceKind sourceKind;
    QString sourcePath;                    // file or directory
    bool recreateHierarchy = false;        // DirectoryTree only
};
```

Multiple dropped paths become one intent per top-level path, admitted as separate operations sharing
one Task Center grouping, rather than one opaque multi-path blob.

The daemon validates at admission:

- connection-qualified account exists and resolves to live session settings;
- source paths exist, are readable regular files/directories owned by the invoking user context,
  contain no symlink escapes outside the chosen roots during enumeration, and pass content
  detection;
- destination mailbox is real/writable when required;
- rights allow creating mail under the destination (and `mayCreateChild` along hierarchy targets);
- no conflicting active import/export owns overlapping destinations in a way policy forbids.

Raw message bytes never travel over JVIP. As with the export destination path, the source path is
user-granted filesystem authority: if a future sandbox model replaces paths with portal grants, the
typed command gains an opaque grant field instead of moving bulk reading into the GUI.

## Import journal and background job

Create an import-specific repository mirroring the export journal schema:

```text
operation fields
    import_id
    destination_mail_account_key
    scope/source kind, source_path, recreate_hierarchy
    status, timestamps
    message_total?, message_completed, byte_total?, bytes_completed
    created_count, skipped_count, failed_count
    last_error

item fields
    import_id, ordinal
    source_relative_path?      eml tree member
    mbox_path + record_offset? mbox member location for diagnostics
    source_sha256              dedupe identity, computed while streaming upload
    captured_size, captured_from_date?
    destination_relative_mailbox_path?   tree imports
    resolved_destination_mailbox_id?
    phase                      Pending | Reading | Uploading | Uploaded |
                               Creating | Created | SkippedDuplicate | NoDestination | Failed
    created_email_id?, existing_email_id?
    uploaded_blob_id?
    last_error
```

Mailbox-creation work for tree imports is recorded as items too (kind-tagged), so a resumed import
never recreates a mailbox it already made and never imports into a mailbox it failed to make.

Job identity follows the export convention: `WorkKind::MailImport` (new enum value), job id prefix
`mail-import:`, one `background_jobs` row per operation, progress reconstructed from the journal
after restart.

## Pipeline mechanics

Per item, executed by the daemon application service with bounded concurrency:

1. **Reading** — open the source region (file directly, or the current record's byte range within an
   mbox file). Compute SHA-256 while streaming. Skip immediately if the hash matches an earlier
   accepted item in the operation.
2. **Uploading** — `uploadBlobFromFile()` streams the bytes as `message/rfc822`. A message exceeding
   the negotiated `maxSizeUpload` fails definitively for that item with a clear message; the job
   continues.
3. **Preflight** — capture destination `Email` state once per batch, stored with the batch's items.
4. **Creating** — submit bounded batches of `Email/import` creations (fixed conservative cap; no
   reliable per-set capability exists to negotiate against). Each creation succeeds or fails
   independently via `created`/`notCreated`. `alreadyExists` maps to `SkippedDuplicate`;
   `overQuota` pauses the job with a quota-specific detail; other SetErrors mark the item failed.
5. **Recording** — transition the item phase and persist the created/existing email id atomically
   before advancing past it.

Batch handling:

- `stateMismatch` on a batch discards the captured state, refreshes it, and retries that batch once
  with fresh state; items already confirmed in that response are recorded before any retry.
- An ambiguous dispatch (request sent, outcome unknown) marks affected items unknown rather than
  succeeded/failed; resume reconciles unknown items by checking the server for the blob/message
  before re-submitting, so restart-after-ambiguity cannot duplicate mail.

Ordering follows manifest ordinal; unlike export there is no user-facing ordering contract beyond
deterministic enumeration (directories walked breadth-first in sorted order; mbox records in file
order).

## Synchronization and visibility

Import deliberately does not use per-message optimistic projections:

- Bulk creation of thousands of Emails would flood optimistic journals, Undo history, and rebase
  machinery designed for interactive mutations.
- The cross-server transfer executor establishes the accepted pattern for bulk durable workflows:
  typed journal + `ifInState` preflight + reconciliation through synchronization.

After each committed batch, the service requests an account synchronization demand so imported
messages materialize into cache through the authoritative sync path promptly. All cache writes
happen in those sync transactions; the importer holds no second source of truth. Offline-synced
destination mailboxes hydrate raw sources through their normal background hydration as imported
Email rows appear.

Undo/Redo is not involved; Task Center owns pause/resume/retry visibility for the operation.

## Network, auth, and offline behavior

Unlike export from an offline-complete mailbox, import always requires destination-server
connectivity and valid authentication. Transient/auth failures move the job to
waiting-for-network/waiting-for-auth using the shared error classification; resume continues from
the durable item checkpoint. Source files are local, so no source-account dependency exists.

Disk-space concerns are limited to the source side (none: files are read in place) plus server
quota. `overQuota` responses pause the job with a message naming the account; resume retries after
the user frees space. There is no local staging copy to run out of room.

## Pitfalls and mitigations

The following failure modes are known hazards of mail import. Each is listed with the mitigation
this plan adopts.

1. **Re-import duplication after crash or ambiguity.** The worst import bug is silent duplicate
   mail. *Mitigation:* per-item durable phases committed before advancing; ambiguous outcomes become
   `unknown` and reconcile against the server before resubmission; server `alreadyExists`
   classified as skip, not failure; content-hash dedupe within the operation; acceptance tests
   restart the job at every item phase including mid-batch and mid-upload.

2. **mbox unescaping corruption.** Stripping the wrong number of `>` characters corrupts bodies or
   splits mailboxes. *Mitigation:* the reader implements exactly the inverse of the exporter's
   documented escape rule (strip one `>` from lines matching `^>+From `); round-trip property tests
   compare export->import output hashes against originals for all our fixtures; lossiness for
   genuine embedded `>From ` text is documented, not hidden.

3. **Format misdetection.** Trusting extensions breaks on `.mbx`, extensionless mbox files, and
   `.eml`-named junk; trusting content alone breaks on mbox files whose first record was trimmed.
   *Mitigation:* the ordered content-first rules above, explicit rejection with the offending path,
   and a detection unit-test corpus including hostile names.

4. **Foreign mbox variants (mboxcl/mboxcl2).** `Content-Length:` headers are frequently stale after
   client edits; honoring them misframes records and truncates or merges messages. *Mitigation:*
   ignore `Content-Length` entirely; frame strictly by From_-line scanning with mboxrd unescaping,
   which parses mboxcl output correctly even though we never rely on its hints.

5. **Import-time `receivedAt` wiping chronology.** Servers default `receivedAt` toward import time
   when not supplied, which destroys sorting and retention ordering for historical mail. *Mitigation:*
   always send the derived value (From_ date for mbox members; omitted only for plain `.eml` where
   the server's Received-header rule recovers the true time).

6. **Metadata asymmetry surprises.** Users expect flags/tags to survive an export->import cycle;
   the portable formats cannot carry them. *Mitigation:* the import dialog states plainly which
   metadata transfers (content, placement, received time) and which does not (read/starred/tag
   state), consistent with the export documentation; private `X-Mozilla-*` headers are ignored.

7. **Whole-file memory blowups.** Multi-gigabyte mbox files loaded into memory freeze or OOM the
   daemon. *Mitigation:* incremental record scanning over fixed-size buffers; `uploadBlobFromFile`
   streaming; the same acceptance rule as export — no bulk path retains whole-message payloads in
   memory.

8. **Oversized single messages.** One attachment-heavy message exceeding `maxSizeUpload` must not
   fail a 10,000-item import. *Mitigation:* definitive per-item failure with a message naming the
   file and both sizes; job continues; summary reports it.

9. **Hierarchy collisions on recreate.** Folder names containing separators, case-folded twins,
   names that sanitize identically, and servers rejecting some names can corrupt or fork a restored
   tree. *Mitigation:* reuse the export planner's sanitization discipline in reverse; match existing
   mailboxes by sanitized path before creating; deterministic discriminators for residual
   collisions; per-subtree `NoDestination` failures instead of aborting the import.

10. **Drop-target ambiguity in DnD.** External file drops must not be mistaken for internal
    message-move drops, and dropping onto non-mailboxes must not invent destinations. *Mitigation:*
    separate MIME-type branch in the mailbox model, real-writable-mailbox-only targets, typed
    intents carrying the destination explicitly; the daemon re-validates every destination right.

11. **Vanishing drag sources.** Files dragged from temporary locations can disappear before the
    daemon reaches them. *Mitigation:* admission stats and opens every source up front; unreadable
    sources fail admission before queueing; mid-job disappearance fails only that item.

12. **Symlink redirection during enumeration.** A swapped symlink could redirect reads outside the
    intended source root. *Mitigation:* enumeration refuses entries that escape the rooted tree
    (same containment stance as the export-root rule).

13. **Quota exhaustion loops.** Repeatedly hammering an over-quota server wastes requests and
    obscures the cause. *Mitigation:* `overQuota` pauses the job with an explicit account-naming
    message instead of retrying blindly.

14. **GUI-thread work creeping in.** Sniffing, hashing, counting, or uploading in the GUI process
    violates the architecture and blocks interaction on large sources. *Mitigation:* the GUI only
    collects URLs and shows cheap existence/sniff feedback; every scan and transfer belongs to the
    daemon job.

15. **Silent partial success.** An import finishing with failures but reporting success teaches
    users to distrust completion states. *Mitigation:* terminal states are `Complete` only with zero
    failed items; otherwise `Partial` with durable created/skipped/failed counts and a reviewable
    item list, mirroring the export partial-result policy.

## State machine

Operation:

```text
PreparingScan -> Ready -> Importing -> WaitingForNetwork -> WaitingForAuth
                          |  Paused
                          v
                     Finalizing -> Complete | Partial | Failed
```

Item phases are listed in the journal schema above. On restart, items persisted in `Uploading`,
`Uploaded`, or `Creating` are not assumed complete; unknown-outcome items reconcile against the
server before resubmission, then the pipeline resumes at the first non-terminal ordinal.

## Progress

Task Center shows: phase (scanning, importing, paused/waiting), messages completed/total once the
scan seals, bytes completed/total when source sizes make totals reliable, and current
mailbox/file for tree imports. Scanning a huge tree first is acceptable and communicated as its own
phase; the sealed count prevents later progress lies.

## Accessibility and messaging

Dialogs, actions, and progress expose semantic names and rate-limited announcements like export.
Errors identify the concrete file, mailbox, and account involved — never a bare "Import failed".
Status messages follow the house rule: actionable outcomes only, no narration of internal
housekeeping.

## Tests

### Format layer

- detection rules across extensions, sniffed content, empty files, binary files, hostile names;
- mboxrd framing: records split correctly, missing final newline tolerated, empty mbox yields zero
  messages;
- unescape correctness incl. multi-`>` prefixes and genuine `>From ` content;
- From_ date variants parse or fall back without failing the item.

### Round-trip

- export `.eml` -> import -> vault object hash equality for fixture corpora;
- export mboxrd tree -> import with hierarchy recreation -> byte-equal raw sources, isomorphic
  mailbox tree, preserved membership multiplicity (one Email in N exported mailboxes imports as N
  memberships or N copies exactly as policy defines — pinned by test);
- mboxrd export -> mboxrd import round-trip preserves content modulo the documented escaping rule.

### Pipeline and durability

- batch submission honors the fixed cap; per-item SetError isolation inside a batch;
- `stateMismatch` rebases once and records already-confirmed creations before retry;
- ambiguous dispatch reconciles against the server without duplicating;
- oversized message fails its item only; quota pauses with detail;
- restart at every item phase resumes without duplication or lost items;
- pause/resume preserves exact next ordinal;
- duplicate-in-operation hash skipping counts as skipped;
- scripted transports only; no live-network tests.

### GUI

- external uri-list drops vs internal message drags stay distinct;
- drop enablement matches real writable mailboxes only;
- dialog choices carry through the typed JVIP command intact;
- keyboard traversal and accessible names cover chooser, destination, hierarchy option, progress,
  and errors;
- destructive-path confirmations name actual paths.

### Hierarchy recreation

- sanitized-path collision fixtures (separators, case folding, dot components, overlong names)
  resolve deterministically;
- existing-folder reuse matches by sanitized path and reports it;
- `mayCreateChild` denial fails only the affected subtree;
- interrupted mailbox creation resumes without duplicates.

## Implementation order

1. Add the mboxrd reader and source enumerator as transport-independent components with fixture
   tests against the existing exporter's output.
2. Add the import journal repository and schema migration, plus `WorkKind::MailImport`
   reconstruction, with no GUI caller.
3. Implement `MailImportApplicationService`: scan/admission, bounded pipeline, waiting states,
   batching, ambiguity reconciliation, and post-batch synchronization demand.
4. Add the typed JVIP action and daemon dispatcher wiring.
5. Add menu/context actions, the import dialog, and Task Center integration.
6. Extend the mailbox tree model for external file drops and route them into the typed port.
7. Run focused production-path tests throughout, then `scripts/check-debug.sh --full` and a separate
   regression review before merging.

Steps 5 and 6 can proceed in parallel once step 4 lands.

## Acceptance invariants

1. Imported raw bytes reach the server unchanged from the source file; any transformation happens
   server-side and is recorded, not hidden.
2. No bulk import path loads a whole message or mailbox file into memory.
3. Restart, ambiguity, and retry never produce duplicated mail.
4. Duplicates are counted and visible; a partial import is never labeled complete.
5. Received times survive import for mbox-derived messages and follow server Received-header
   semantics for `.eml`.
6. Hierarchy recreation is collision-safe and rights-checked; failures are contained per subtree.
7. Cache state changes only through synchronization transactions; the importer writes only its own
   journal.
8. Account identity is connection-qualified throughout intents, journal, and routing.
9. The daemon owns detection, enumeration, upload, and mutation; the GUI owns only path collection,
   targeting, and presentation.
10. Import shares raw-source primitives with export/transfer without sharing their journals.
