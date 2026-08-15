# Mail Export Implementation Plan

## Status

This document defines the implementation plan for exporting mail from Javelin as raw `.eml` files
or mboxrd mailbox files.

Mail export is a separate application workflow from cross-account/server Move and Copy. It shares
exact scope enumeration, raw RFC 5322 materialization, file-backed MailVault access, and durable work
scheduling with [cross-server mail transfer](CROSS_SERVER_MAIL_TRANSFER_IMPLEMENTATION_PLAN.md), but
it does not share the transfer journal or transfer failure policy.

The central rule is:

> Export the authoritative raw RFC 5322 message source. Do not reconstruct exported messages from
> parsed body parts, rendered HTML, cached snippets, or compose values.

A large mailbox/account export is long-running storage work. It must stay bounded in memory, survive
restart, pause safely, and never require the GUI process to remain alive.

## Goals

- Export one selected Email as an `.eml` file.
- Export multiple selected Emails as a directory of `.eml` files.
- Export one mailbox as either a directory of `.eml` files or one mboxrd file.
- Export an entire mail account as either:
  - a mailbox directory tree containing `.eml` files; or
  - a mailbox directory tree containing one `.mbox` file per mailbox.
- Preserve exact raw message bytes for `.eml` export.
- Preserve mailbox membership and hierarchy in mailbox/account exports.
- Support mail that is not already stored for offline use by materializing raw source on demand.
- Reuse existing current MailVault objects without downloading them again.
- Keep network and file I/O bounded and off the GUI thread.
- Persist enough state to resume large exports safely after daemon restart.
- Integrate with the existing WorkScheduler/Task Center, including progress, pause, resume, cancel,
  waiting-for-network/auth/space states, and clear terminal errors.
- Make filename/path decisions deterministic, portable, collision-safe, and independent of internal
  cache paths.
- Preserve the current daemon/GUI ownership boundary: GUI chooses intent and filesystem target;
  daemon owns scope resolution, raw materialization, durable job state, and export execution.

## Non-goals

- Do not make export a special case of cross-server Copy.
- Do not use the transfer journal for filesystem export.
- Do not make `Keep complete offline copy` a prerequisite for exporting a mailbox/account.
- Do not expose the internal MailVault projection tree as the public export format.
- Do not guarantee byte-for-byte recovery of a message after an mbox round trip. Mbox is a framed,
  escaped container; `.eml` is the byte-preserving format.
- Do not automatically create a compressed archive in the first implementation. `.zip`, `.tar`, or
  similar packaging can be layered over a completed directory export later.
- Do not implement mail import as part of this feature. Import should consume the same raw-message
  infrastructure later, but it has separate duplicate, destination, and server mutation semantics.
- Do not inject Javelin/JMAP mutable state into exported RFC 5322 messages through invented headers.
  Read/unread, starred, answered, draft, custom keywords, and Javelin tag presentation metadata are
  not portably represented by plain `.eml` or mboxrd.
- Do not export virtual search results, synthetic mailbox roots, separators, or other non-Mailbox UI
  nodes as account mailboxes.

## Metadata fidelity

`.eml` and mboxrd are interoperable message/container formats, not complete JMAP-account backup
formats.

The initial exporter preserves:

- authoritative raw RFC 5322 message content (`.eml` byte-for-byte; mboxrd subject to container
  framing/escaping);
- mailbox hierarchy;
- mailbox membership, including duplicate placement of one Email in multiple exported mailboxes;
- deterministic message ordering for mbox output.

It does **not** have a portable place to preserve all mutable JMAP Email state such as `$seen`,
`$flagged`, `$answered`, `$draft`, custom keywords, or Javelin's account-local tag color/display-name
metadata. Some mail clients encode subsets of this state using private mbox headers or filename
conventions, but doing so would modify the exported message and still would not be reliably portable.
Do not silently choose one client's convention.

If full Javelin round-trip backup is desired later, add an optional versioned sidecar/archive manifest
that records mailbox identities and mutable Email metadata while leaving the `.eml`/mbox payloads
standards-friendly. That is a separate compatibility contract from the initial portable export.

## Relationship to existing source handling

Javelin already downloads and stores complete raw message sources in the MailVault for offline mail.
It also has a single-message **View Message Source** path through
`MessageContentPort::requestMessageSource()`. That current path returns `MessageSourceDownload` with a
`QByteArray` payload to the GUI and writes a temporary `.eml` there.

That is acceptable for viewing one ordinary message, but it is not a bulk-export architecture. A
multi-gigabyte message or a large account export must never require whole MIME payloads in GUI memory.

Cross-server transfer therefore introduces shared daemon infrastructure of this shape:

```text
MailScopeResolver
    selection/mailbox/account scope
        -> stable exact Email manifest

RawMailMaterializer
    manifest item
        -> current raw RFC 5322 MailVault object
        -> file-backed lease/path
```

Export must consume those services directly. The existing viewer can remain unchanged initially and
may later migrate to a file-backed daemon response if that simplifies memory behavior without
complicating temporary-file ownership.

## Account identity prerequisite

Mail export must use the same connection-qualified account identity required by cross-server mail
transfer. Remote JMAP `accountId` is scoped to one JMAP session/server and is not globally unique.

All export intents, persisted manifests, background jobs, raw-source references, and account/mailbox
lookups must therefore identify the source through the stable local mail-account key or an explicit
`MailAccountLocator`, never a bare remote account id.

Account export is a particularly strong regression case because two configured servers may both
expose `accountId = "u1"` while the user expects two distinct export roots.

## User-facing export scopes

### 1. One selected Email

Expose **Export Message...** for an exact single-Email selection.

The file chooser defaults to a sanitized descriptive `.eml` filename. Export writes the exact raw
RFC 5322 bytes to that file.

If a visible row represents a collapsed Thread rather than exactly one Email, the action must not
silently export only the representative. Either:

- present the action as **Export Messages...** and preserve the existing whole-Thread
  `MessageSelection` semantics; or
- only offer singular **Export Message...** when the resolved selection is exactly one Email.

Prefer the first approach for consistency with Javelin's existing action semantics. The daemon still
materializes the Thread authoritatively before fixing the manifest.

### 2. Multiple selected Emails

Expose **Export Messages...** and choose a target directory.

Each exact Email becomes one `.eml` file. This scope has no mailbox hierarchy requirement because the
user explicitly selected messages. If the same Email is represented only once in the resolved
selection, export it once even if it belongs to multiple mailboxes.

### 3. One mailbox

Expose **Export Mailbox...** on real mailbox nodes and from mailbox properties.

Offer two formats:

- **EML folder** — one directory containing one `.eml` per exact Email in the mailbox;
- **mboxrd** — one `.mbox` file containing every exact Email in the mailbox.

Mailbox export is based on authoritative mailbox membership, not the currently loaded query window,
thread-collapsed rows, quick filters, sort order, or hidden UI state.

### 4. Entire account

Expose **Export Account...** from the account/server node and account management UI.

Offer two formats:

```text
EML directory tree
account export root/
    Inbox/
        ... .eml
    Archive/
        ... .eml
    Projects/
        Javelin/
            ... .eml

mboxrd directory tree
account export root/
    Inbox.mbox
    Archive.mbox
    Projects/
        Javelin.mbox
```

An account export includes every readable real JMAP Mailbox in that mail account, including mailboxes
hidden from Javelin's normal mailbox tree. UI hide/subscription preferences are presentation policy,
not archival scope. Skip only objects that are not real readable mailboxes, and report any mailbox
that becomes inaccessible while the export is running.

Do not initially offer one monolithic account-wide `.mbox`. Mbox has no useful representation of the
JMAP mailbox hierarchy or membership graph; one mboxrd file per mailbox preserves the user's
organization much more clearly.

## Snapshot semantics

Large exports must have a stable logical scope, but do not claim an atomic snapshot at the instant
the user clicks **Export**. Authoritative mailbox/account enumeration itself may require many paged
JMAP calls while the server continues changing.

Admission first creates a durable `PreparingManifest` generation. The daemon enumerates in bounded
pages, records the relevant JMAP query/object states, reconciles changes that occurred during the
crawl, and **seals** the manifest only after it has reached an authoritative final state for that
generation. New mail and membership changes after the seal do not alter the export.

For selected-message export, the manifest can normally seal as soon as the exact `MessageSelection`
has been authoritatively materialized and its source identities captured. For mailbox/account export,
the UI/Task Center should distinguish **Preparing export** from the later byte-writing phase.

This rule prevents a 100 GB account export from remaining a moving target, gives restart recovery a
precise definition, and does not promise cross-object transactional snapshot semantics that JMAP does
not provide.

### Selected-message manifest

Resolve the stable `MessageSelection` exactly as other mail actions do:

- complete whole-Thread materialization before fixing the manifest;
- never use representative row ids as the final Email set;
- deduplicate exact Email ids within the selection;
- preserve the source account locator.

### Mailbox manifest

Enumerate authoritative, uncollapsed mailbox membership in bounded pages. Capture the query/object
state used by the generation. If membership changes during the crawl, reconcile from the captured
state with the same bounded `Email/queryChanges`/`Email/changes` principles used by Javelin's durable
mail synchronization. If the server can no longer calculate changes, or an anchor becomes invalid in
a way that prevents proving complete coverage, discard/restart the unsealed generation rather than
seal a maybe-incomplete manifest.

Only mark the mailbox manifest sealed once the recorded membership and captured Email metadata are
reconciled to the accepted final state. Do not derive the manifest from:

- visible list rows;
- current query-window prefix;
- collapsed Thread representatives;
- FTS/search results;
- notification cache coverage.

The offline full-mirror enumeration machinery is conceptually similar and should share low-level
bounded enumeration primitives where practical, but an export has its own generation/checkpoint and
must not mutate the user's offline-storage preference.

### Account manifest

Do not enumerate every mailbox independently and download/fetch the same Email metadata repeatedly.
Prefer one account-wide, uncollapsed Email enumeration, materializing each Email's authoritative
`mailboxIds` plus the metadata needed by export. Snapshot the readable real Mailbox tree separately,
then project each captured Email into every captured readable mailbox membership.

Reconcile Email changes that occur during the account crawl before sealing. Likewise verify/reconcile
Mailbox state so names, hierarchy, readability, and membership targets used by the export are known.
JMAP does not provide one transactionally atomic state spanning Mailbox and Email data types, so the
contract is a sealed, authoritatively reconciled export generation—not a fictional single-instant
server snapshot. If changes keep arriving, bounded reconciliation advances the generation; if the
server cannot calculate required changes, restart the unsealed manifest rather than guess.

A JMAP Email may belong to more than one mailbox. Preserve that fact in the export manifest. The same
Email may therefore appear in multiple exported mailbox destinations.

Do not globally deduplicate a message across an account export: doing so would lose mailbox
membership information in both the EML-folder and mboxrd formats. Deduplicate **source acquisition**
by Email/blob identity, not logical output membership.

An Email belonging to two mailboxes should be represented in both exported mailboxes. Distinct JMAP
Email objects should remain distinct export entries even if their raw content hashes happen to be
identical.

## Ordering

A deterministic message order makes exports reproducible and mbox files easier to inspect.

For mailbox/account export, snapshot an explicit stable order at enumeration time. Prefer the
mailbox's server order equivalent to newest-first query semantics, then write mbox entries in the
reverse of that manifest if the desired interoperable output is oldest-to-newest. Pick one behavior
and test it; do not let restart timing alter order.

Recommended public behavior:

- `.eml` directory filenames carry date information, but directory order is not semantically relied
  upon;
- `.mbox` writes oldest-to-newest, matching common mailbox-file expectations and making appended
  records chronological.

The manifest stores an ordinal so retries/restarts never need to infer order from current server
state.

## Raw source materialization

For every source manifest item, obtain a `RawMailMaterializer` file-backed lease for the **captured
blob identity**, not whatever blob happens to be current when the writer eventually reaches it.

The materializer must:

1. inspect whether a MailVault reference matching the Email's captured `blobId` already exists;
2. reuse it when valid;
3. otherwise request that captured blob directly through the source JMAP download URL as
   `message/rfc822` and stream it into the vault;
4. hash and atomically install the object using the existing vault rules;
5. return a file-backed lease/path without reading the entire object into `QByteArray`;
6. pin the object only while required by unfinished export work;
7. enter waiting-for-network/auth/space instead of failing or buffering unbounded data when a
   dependency is unavailable.

If the server Email later changes so its current `blobId` no longer matches the sealed export
manifest, that does not by itself invalidate the item. The exporter wants the captured revision: use
the matching vault object or request the captured blob id directly while the account still has access
to it.

If that exact captured blob is neither cached nor downloadable anymore, mark the item
changed/unavailable and surface a partial export result. Do not substitute the Email's newer blob
revision invisibly under the old manifest entry.

## EML export writer

### Byte semantics

An `.eml` export is the raw RFC 5322 object. Copy the MailVault object's bytes directly to the target
file. Do not:

- normalize line endings;
- re-fold headers;
- decode/re-encode transfer encodings;
- alter MIME boundaries;
- inject Javelin headers;
- rewrite Date/Received/Message-ID;
- serialize from the parsed JMAP Email representation.

A successful `.eml` file should hash identically to the captured raw MailVault object.

### Filename policy

User-visible filenames must be descriptive but cannot depend on subject/date uniqueness.

Use a deterministic shape conceptually like:

```text
YYYY-MM-DD - Sender - Subject - <short-stable-id>.eml
```

The exact presentation may be refined, but the implementation must:

- sanitize `/`, path separators, NUL/control characters, and filesystem-invalid components;
- collapse dangerous `.`/`..` components;
- bound component byte/character length so long subjects do not exceed filesystem limits;
- handle empty/missing subject/sender/date;
- include a stable discriminator derived from Email identity, not message subject;
- compare collision candidates according to the destination filesystem behavior where practical;
- never overwrite a sibling message merely because sanitized names collide.

Do not use internal MailVault projection filenames as the public naming policy.

### Independent output files

The internal offline projection uses hard links for efficient cache representation. Do not expose
those hard links as the default EML export mechanism.

Exported mailbox copies should behave like ordinary independent user files. If one Email belongs to
two mailboxes, changing one exported file later should not unexpectedly mutate another path through a
shared hard link. Filesystem-specific reflink/copy optimizations may be used only if they preserve
ordinary copy semantics.

## mboxrd export writer

Use **mboxrd**, not an ambiguous generic `mbox` writer.

Each exported Email is framed as one mbox record with a conventional `From_` separator. While
streaming the raw RFC 5322 content, quote body/message lines according to mboxrd rules so lines that
could be interpreted as record separators cannot split the mailbox.

The writer must operate incrementally on bounded byte buffers. It must not load the whole message or
whole mailbox into memory.

Mbox framing necessarily transforms the representation:

- a `From_` separator is added;
- separator-like lines are escaped;
- a record-ending newline may be required.

Therefore mboxrd is an interoperable mailbox container, not the byte-identical archival format. The
`.eml` option is the format for exact source preservation.

### Envelope sender/date

The mbox `From_` separator is container metadata, not the RFC 5322 `From:` header.

Derive a deterministic best-effort envelope sender value from cached message metadata when available,
falling back to a safe placeholder rather than failing the export. Derive the separator date from
captured received/date metadata with a deterministic fallback. Do not alter the embedded RFC 5322
headers to make them match the separator.

### Append safety

Do not implement account/mailbox export by blindly appending to an arbitrary existing `.mbox`.

The first version should create a new export target. If the chosen path exists, the UI should require
an explicit replace/new-name decision before the durable job is queued. Avoid merge semantics until
there is a well-defined duplicate policy.

Write through a sibling temporary/in-progress file and publish the completed mbox atomically with a
rename when the destination filesystem permits it. A crash must never make an incomplete file look
like a completed export.

For very large files where a full restart would be expensive, keep durable record-boundary
checkpoints. On resume:

- validate the in-progress file identity;
- truncate to the last durably recorded complete-message offset if necessary;
- continue with the next manifest ordinal;
- never duplicate an already committed mbox record.

Persist a checkpoint only after the record bytes have been flushed according to the chosen durability
contract.

## Directory export safety

Directory exports need the same distinction between **in progress** and **complete**.

Prefer a dedicated export root selected/created by Javelin. Persist an export marker/manifest file in
that root or a daemon-side journal with a unique export id so restart can prove that it is resuming its
own output rather than an unrelated directory with similar names.

Do not delete or replace unrelated files encountered in a non-empty target directory.

Recommended first-version rule:

- require a new/empty directory for mailbox/account EML export;
- create files with temporary sibling names and atomically rename each file after complete write;
- on restart, validate already-completed files by size/hash or durable manifest evidence before
  skipping them;
- remove only Javelin-owned temporary files when cancelling/cleaning an incomplete job;
- reject or safely contain unexpected symlink/reparse-point entries inside a Javelin-owned export
  root so a path changed externally during pause cannot redirect later writes outside that root.

A later feature may support merging into an existing directory, but that requires explicit duplicate
and overwrite semantics and should not be smuggled into the initial exporter.

## Mailbox hierarchy and path collisions

JMAP Mailbox names are user data and sibling names are not safe filesystem path components.

The exporter must preserve hierarchy while handling:

- `/` or platform separators inside names;
- `.` and `..`;
- names that sanitize to the same filesystem component;
- case-folding collisions on case-insensitive filesystems;
- empty/whitespace-only names;
- extremely long names/paths;
- mailbox rename during export;
- two different mailbox ids with the same display name where the server permits it.

Snapshot mailbox id, parent id, name, and ordinal at export admission. Generate deterministic safe
path components from that snapshot and add a short stable mailbox-id discriminator only when needed
to resolve a collision. A later server rename does not change the in-progress export path.

For mboxrd account export, mailbox content and child hierarchy must coexist because a JMAP mailbox
may both contain mail and have children. Prefer a deterministic paired layout such as:

```text
Projects.mbox
Projects/
    Javelin.mbox
```

Here `Projects.mbox` contains the parent mailbox's own messages and `Projects/` contains its child
mailboxes. This keeps every mailbox as an ordinary `.mbox` file without inventing a magic mailbox
filename inside the directory. `ExportPathPlanner` still resolves collisions caused by real mailbox
names that sanitize to these same components. The directory-tree convention itself is Javelin export
layout; individual `.mbox` files remain standard mboxrd, while direct hierarchy import behavior is
necessarily client-dependent.

The EML tree has the same issue: a parent mailbox can simply be a directory containing both its own
`.eml` files and child mailbox directories, provided message filenames and child-directory names are
collision-safe.

## Export journal and background job

Create an export-specific durable journal/repository. It should be linked to one semantic
`background_jobs` entry rather than encoding the whole manifest into scheduler checkpoint JSON.

Suggested operation fields:

```text
export_id
source_mail_account_key
scope_type                 selected | mailbox | account
scope_mailbox_id?          for mailbox scope
format                     eml | mboxrd
output_root/path
status
created_at/started_at/completed_at
manifest_generation
message_total
message_completed
byte_total?                when knowable
bytes_completed
last_error
```

Suggested manifest/item fields:

```text
export_id
ordinal
mailbox_id?                output membership for mailbox/account export
email_id
captured_blob_id
captured_size
captured_received_at
captured metadata needed for naming/mbox From_
output_relative_path
status
written_size
raw_content_hash?
mbox_committed_offset?
error
```

For account export, one Email with three mailbox memberships produces three output entries, but raw
materialization may be shared through one captured Email/raw-object record rather than downloading
the source three times.

Keep manifest rows bounded/streamable. Do not load an account's entire export manifest into an
in-memory vector merely because it is persisted in SQLite.

## State machine

A useful high-level operation state is:

```text
PreparingManifest
  -> Ready
  -> Exporting
  -> WaitingForNetwork
  -> WaitingForAuth
  -> WaitingForSpace
  -> Paused
  -> Finalizing
  -> Complete
```

Terminal alternatives:

```text
Cancelled
Partial
Failed
```

Per output item:

```text
Pending
  -> AcquiringSource
  -> SourceReady
  -> Writing
  -> Written
  -> Verified
```

On restart, any item persisted as `Writing` is not assumed complete. Reconcile its temporary/output
file against the last durable checkpoint, then either resume safely or rewrite that item.

## Progress

Task Center should show useful progress for large exports without making a pre-scan mandatory solely
for a byte total.

Always provide:

- messages completed / total once the manifest is fixed;
- current mailbox for account export;
- current phase (preparing, downloading source, writing, finalizing, paused/waiting).

Provide byte progress when captured source sizes make the total reliable. Account exports may derive
an approximate or exact total from manifest metadata after enumeration. Do not count duplicated
mailbox memberships as network bytes if one raw source is reused, but output-byte progress may count
each written copy. Keep labels clear about which quantity is being shown.

## Work scheduling and bounded resource use

Export is user-initiated long-running work, but it should not starve message viewing, compose sends,
foreground synchronization, or other interactive operations.

Use WorkScheduler priorities and the same quiet-period/background rules as offline hydration.

Keep a small bounded pipeline, for example:

```text
manifest page -> raw materialization -> one/few active file writers
```

Constraints:

- never retain raw MIME for multiple large messages in memory;
- file-backed leases only;
- limit concurrent source downloads according to the existing scheduler/connection policy;
- avoid concurrent writers to the same mbox file;
- account EML export may write distinct files concurrently only if doing so materially improves
  throughput without causing unbounded disk seeks or memory growth;
- reuse one MailVault object for repeated account-mailbox memberships;
- release export leases as soon as all output entries depending on that raw object are durable.

Unlike destructive cross-server Move, export does not need to pin a raw source for Undo history after
its bytes have been durably written.

## Network/auth/offline behavior

An export may be admitted while offline if the daemon can resolve the requested scope from
sufficient authoritative cached state. If exact enumeration is not known, the job waits for network
rather than silently exporting a partial visible cache.

For each source message:

- already-current raw source in MailVault requires no network;
- missing raw source waits for source account connectivity;
- expired/revoked source authentication moves the job to waiting-for-auth;
- reauthentication resumes from the durable item checkpoint;
- one mailbox/account failing does not erase completed output from other items.

A complete-offline mailbox should normally export without network access because its invariant
already guarantees every Email has current raw RFC 5322 source. The exporter should benefit from
that state naturally through `RawMailMaterializer`, not special-case copy the offline projection
layout.

## Disk-space behavior

Export writes user-selected destination storage and may also need temporary MailVault space for raw
sources not already cached.

Before starting a known-size individual message write, detect obvious destination-space failure where
possible. For a large mailbox/account, do not require a potentially expensive exact filesystem free-
space proof before admission; handle `ENOSPC`/write failure durably and surface it as a waiting/error
state with the exact target path.

If MailVault cannot stage a required source, use `waiting_for_space` and preserve the manifest/output
checkpoint. Never fall back to a whole-message RAM buffer.

The export journal distinguishes source-cache space from output-filesystem space so the UI can tell
the user which location is full.

## Cancellation and pause

Pause only at recoverable boundaries and persist the checkpoint before reporting the job paused.

Cancellation semantics:

- stop admitting new source downloads/writes;
- allow an already-running bounded filesystem write to reach a safe checkpoint or cancel it into a
  clearly marked temporary file;
- leave completed exported files intact by default;
- remove only Javelin-owned temporary/in-progress files;
- never remove unrelated user files from the chosen directory;
- mark the export `Cancelled`, not `Complete`.

The UI may later offer **Delete incomplete export** as an explicit separate cleanup action based on
journal ownership. Do not make cancellation itself recursively delete a large output tree without a
separate confirmation.

## Error and partial-result policy

An export can be partially useful even if one message becomes unavailable.

Differentiate:

- source Email disappeared or captured blob revision unavailable;
- source authentication/network failure;
- raw-source download/protocol failure;
- unreadable source due to rights change;
- destination permission failure;
- destination full;
- target path collision/change;
- filesystem I/O failure;
- output modified externally during pause/restart;
- corrupt/inconsistent resume checkpoint.

Transient dependency failures wait/retry through normal scheduler policy. Definitive per-item source
failure should normally allow the remaining account/mailbox export to continue and finish as
`Partial`, with a durable list/count of omitted items.

Do not silently label a partial account export successful.

For a single-message export, any item failure is simply operation failure because there is no useful
partial set.

## Existing-output and overwrite UX

All destructive filesystem choices happen before the daemon begins writing.

For single `.eml` export, normal save-dialog replace confirmation is sufficient, but the daemon still
writes a temporary sibling and atomically replaces/publishes the target where possible.

For mailbox/account export:

- default to a new target path;
- if an existing non-empty directory/file is chosen, do not merge silently;
- offer **Choose another location** or an explicit **Replace Javelin export** only when Javelin can
  prove ownership of that existing export;
- do not offer generic recursive replacement of arbitrary directories as a convenience shortcut.

Confirmation text must name the actual path and what will be replaced, consistent with Javelin's
rule that destructive confirmations include relevant object details.

## GUI integration

### Message actions

Add **Export Message...** / **Export Messages...** to the message context menu and appropriate File or
Message menu. Do not overload **View Message Source**; viewing a temporary source and exporting a
user-owned file are different actions.

The action uses the stable `MessageSelection`, not current row indexes after a dialog closes.

### Mailbox actions

Add **Export Mailbox...** to real mailbox context/properties UI. The format chooser may be part of one
small export dialog containing:

- format: EML folder / mboxrd;
- destination path;
- summary: mailbox name and known message count if authoritative;
- explanation that missing raw mail may be downloaded.

### Account actions

Add **Export Account...** to account/server context and account management. Show:

- account display name/server context;
- EML tree / mboxrd tree format;
- destination directory;
- count of included readable mailboxes when known;
- clear statement that hidden mailboxes are included because this is a complete account export.

### Accessibility

Every export action/dialog must expose semantic names and status text to assistive technology.
Progress announcements should be rate-limited: announce meaningful phase/status changes and coarse
progress, not every message written.

Error dialogs must identify the account/mailbox/message/path involved rather than saying only
"Export failed".

## Daemon command boundary

Introduce a typed export intent rather than giving the GUI a generic filesystem/job API.

Conceptually:

```cpp
enum class MailExportFormat { Eml, MboxRd };
enum class MailExportScopeKind { Selection, Mailbox, Account };

struct MailExportIntent {
    MailAccountLocator sourceAccount;
    MailExportScopeKind scopeKind;
    std::optional<std::string> mailboxId;
    std::optional<MessageSelection> selection;
    MailExportFormat format;
    QString destinationPath;
};
```

Use separate constructors/variant payloads in production if that makes invalid combinations
unrepresentable. An Account scope must not carry a message selection; Selection must carry one;
Mailbox must carry one real mailbox id.

The daemon validates:

- connection-qualified account exists;
- scope/fields are coherent;
- mailbox is real/readable for mailbox scope;
- destination path policy is satisfied;
- format is supported;
- no conflicting active export owns the same output target.

Do not transmit raw message bytes over JVIP for export.

The destination path is user-granted filesystem authority. In the normal unsandboxed desktop build,
the daemon can validate/open that path directly. Do not assume forever that a QString path is enough:
if a sandbox/portal packaging model grants access only through a portal document, file descriptor, or
capability token, extend the typed command to carry the appropriate opaque grant instead of moving
bulk file writing back into the GUI. Admission must verify that the daemon actually has destination
access before committing a large export job.

## Filesystem abstraction and tests

Keep output writing behind focused components so mbox rules and crash-safe publication can be tested
without GUI dialogs or live servers.

Suggested boundaries:

```text
MailExportApplicationService
MailExportRepository
MailExportManifestRepository
EmlExportWriter
MboxRdExportWriter
ExportPathPlanner
```

The application service consumes shared:

```text
MailScopeResolver
RawMailMaterializer
MailVault file-backed lease
WorkScheduler
```

Qt GUI code owns only destination selection and presentation.

## Tests

### Shared scope/materialization

- selected exact Email resolves once;
- collapsed Thread resolves all authoritative children, never only representative;
- mailbox enumeration is uncollapsed and not limited to visible query windows;
- account enumeration includes hidden readable real mailboxes;
- mailbox changes during manifest preparation reconcile before seal;
- account-wide Email membership changes during manifest preparation reconcile before seal;
- `cannotCalculateChanges`/invalidated enumeration restarts the unsealed generation rather than
  producing a partial manifest;
- two connections with identical remote `accountId/mailboxId/emailId` remain isolated;
- cached current raw source causes no network request;
- missing raw source streams to vault with bounded memory;
- stale blob id is not silently replaced by a newer revision;
- waiting-for-network/auth/space resumes correctly;
- file-backed lease prevents eviction while a writer is active.

### EML writer

- output bytes hash exactly equal raw source;
- CRLF/LF and arbitrary MIME bytes are preserved;
- exporter does not inject read/star/tag/status headers into raw RFC 5322 content;
- long/empty/path-hostile subject and sender produce safe filenames;
- two messages sanitizing to the same name remain distinct;
- no `..`, separator, or externally introduced symlink escape can leave export root;
- partial temporary write is not published as complete;
- restart verifies/skips completed file safely;
- cancellation removes only owned temporary file;
- account message in two mailboxes produces two independent output files;
- distinct Email objects with identical raw hash remain distinct entries.

### mboxrd writer

- correct `From_` separator is emitted;
- separator-like message lines are quoted according to mboxrd rules;
- already quoted `>From ` cases round-trip according to mboxrd semantics;
- message with no final newline gets valid record framing;
- empty message/body edge cases remain parseable;
- large source is streamed in bounded chunks;
- multiple records preserve manifest order;
- crash in middle of a record truncates/resumes at prior committed offset;
- restart never duplicates committed record;
- completed output is atomically published where filesystem supports it;
- parent mailbox with children has a collision-free `.mbox` tree representation.

### Manifest/recovery

- new mail arriving after manifest fixation is not exported;
- message moved after manifest fixation retains captured mailbox output membership;
- account Email in multiple mailboxes gets one raw acquisition and multiple output entries;
- mailbox rename after start does not change output path;
- source disappearance yields partial result rather than unrelated-message rollback;
- daemon restart at every item phase resumes correctly;
- pause/resume preserves exact next ordinal;
- output file externally modified while paused is detected before resume;
- Task Center reconstructs accurate progress after restart.

### GUI

- singular/plural message export action is correct;
- Thread selection keeps full selection intent through dialog delay;
- mailbox action appears only for real exportable mailboxes;
- account export includes hidden-mailbox explanation;
- format and target path are carried through typed JVIP command;
- destructive replace confirmation names target path/details;
- keyboard traversal and accessible names cover format, target, progress, pause/cancel, and errors.

## Implementation order

1. Land the connection-qualified account identity migration required by cross-server transfer.
2. Land the shared `MailScopeResolver`, `RawMailMaterializer`, and file-backed MailVault lease/path
   primitives from the transfer plan.
3. Add export journal/manifest repositories and durable WorkScheduler job reconstruction with no GUI
   caller.
4. Implement collision-safe `ExportPathPlanner` and single-message EML writer using file-backed raw
   source.
5. Implement mailbox/account authoritative manifest enumeration and EML directory export.
6. Implement streaming mboxrd writer plus record-boundary checkpoint/recovery tests.
7. Add mailbox/account mboxrd tree planning, including parent-mailbox-with-children representation.
8. Add typed JVIP export command and daemon application service admission/validation.
9. Add message, mailbox, and account GUI actions/dialogs.
10. Add Task Center progress, pause/resume/cancel, partial-result details, and owned-output cleanup.
11. Run focused production-path tests throughout, then the normal full build/test suite and a separate
    regression review before merging.

Steps 3 onward may proceed in parallel with later cross-server transfer phases once step 2 is stable.
Export must not fork private raw-source or account-enumeration machinery merely to land sooner.

## Acceptance invariants

The feature is not complete unless all of the following hold:

1. `.eml` export writes the authoritative raw RFC 5322 bytes without reconstruction or normalization.
2. Bulk export never requires whole-message MIME payloads in GUI memory.
3. Mailbox/account scope comes from authoritative uncollapsed membership, not visible/collapsed query
   windows.
4. Account export includes every readable real mailbox regardless of Javelin hide/subscription UI
   state.
5. Multi-mailbox membership is preserved by representing the Email in every exported mailbox.
6. Distinct JMAP Email objects are not collapsed merely because their raw content matches.
7. Export scope is fixed durably before long-running output, so later server changes do not silently
   change membership.
8. Raw sources already present in MailVault are reused; missing sources are materialized through the
   shared bounded daemon service.
9. No bulk path depends on `MessageSourceDownload`/`QByteArray` raw payloads.
10. mbox output is explicitly mboxrd and applies correct `From_` escaping incrementally.
11. A crash cannot publish a partial `.eml` or `.mbox` as a completed export.
12. Restart resumes from durable file/message boundaries without duplicating output.
13. Export never silently overwrites or deletes unrelated files in a user-selected destination.
14. Filenames/mailbox paths are deterministic, path-safe, bounded, and collision-safe.
15. Complete-offline mail naturally exports without network while online-only mail can download raw
    source on demand; offline preference is never changed as a side effect.
16. Account identity is connection-qualified throughout persistence, scheduling, materialization, and
    export routing.
17. The daemon owns export execution and filesystem/network state; the GUI owns only user interaction
    and presentation.
18. Cross-server transfer and export consume the same exact-scope/raw-source infrastructure while
    retaining separate journals and failure semantics.
