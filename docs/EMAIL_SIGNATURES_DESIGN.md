# Email Signatures and JMAP Identities Design

## Status

This document defines Javelin's intended email-signature implementation. JMAP `Identity` objects are
the sole authoritative store for signatures. SQLite contains only the disposable synchronized cache
and optimistic mutation state required to present and edit them.

There is no local signature library, no imported copy of a server signature, and no signature body in
`QSettings`.

## Core model

RFC 8621 defines these mutable Identity properties:

- `name`;
- `replyTo`;
- `bcc`;
- `textSignature`; and
- `htmlSignature`.

The Identity `email` property is immutable after creation. `mayDelete` is server-set and controls
whether that Identity may be destroyed; it does not guarantee that every other update will be
accepted. A server may still reject an `Identity/set` create or update, and Javelin must surface the
actual SetError rather than silently falling back to local state.

RFC 8621 explicitly permits multiple Identity objects with the same email address so users can choose
between different names, signatures, or other submission settings. Javelin therefore models
"multiple signatures" as multiple server identities, commonly sharing the same email address.

Example:

```text
Johnson <johnson@example.com> — Regards, Johnson
Johnson <johnson@example.com> — Johnson Clark | Example Ltd
Johnson <johnson@example.com> — no signature
```

Each row is a distinct JMAP Identity and has its own `identityId`, `textSignature`, and
`htmlSignature`. The selected Identity ID is also the value submitted in `EmailSubmission.identityId`.

## Non-negotiable invariants

- The JMAP server is the only durable source of signature content.
- SQLite may be deleted and rebuilt without losing server signatures.
- `QSettings` does not contain signature definitions, signature bodies, local signature UUIDs, or a
  shadow identity catalogue.
- Every cached JMAP Identity remains individually selectable, including identities with duplicate
  email addresses.
- Existing drafts and compose working copies contain the exact visible body. Opening or sending one
  never injects another signature.
- A server mutation is never reported as successful merely because an optimistic cache projection was
  displayed.
- The GUI does not call JMAP or write the cache directly.

A future preferred-sender setting may store only an Identity ID as a presentation preference. It is
not part of the initial signature implementation and would not duplicate signature content.

## Current implementation

Javelin already:

- parses `Identity.textSignature`, `Identity.htmlSignature`, and `mayDelete`;
- caches those fields in the `identities` SQLite table;
- reads identities through the GUI's read-only `IdentityReader`;
- passes the chosen `identityId` to EmailSubmission; and
- inserts the first available identity's server signature when opening a new message.

The current implementation is incomplete in several important ways:

- only `Identity/get` exists; there is no `Identity/set` or `Identity/changes` implementation;
- identities are loaded on demand by compose rather than maintained as a synchronized cache domain;
- the From selector deduplicates identities by email address;
- new messages always start from the first sender identity;
- replies and forwards do not receive the selected identity's signature;
- changing identity does not safely replace an automatically inserted signature; and
- there is no user interface for creating, duplicating, editing, or deleting identities.

The email-address deduplication is directly incompatible with RFC 8621's multiple-identity model and
must be removed.

## Storage and synchronization

### Confirmed identity cache

The existing `identities` table remains the confirmed read model:

```text
account_id
identity_id
email_address
name
reply_to_json
bcc_json
text_signature
html_signature
may_delete
```

The table is disposable server-derived state. Signature contents do not require a settings-schema
migration.

`IdentityRepository::replaceAll` must be extended or complemented with transactional incremental
methods for:

- upserting created or updated identities;
- deleting destroyed identities;
- replacing the complete account identity set; and
- storing the corresponding Identity state token in `sync_state` under object type `Identity`.

The identity objects and state token must commit in the same transaction. Cache invalidation is
published only after commit.

### Initial synchronization

Identity synchronization becomes an ordinary account-owned data domain rather than a compose-only
lookup.

On account startup, or when no cached Identity state exists:

1. call `Identity/get` with `ids: null`;
2. validate the account ID and complete response;
3. replace the account's cached identities and Identity state atomically; and
4. publish a `SenderIdentities` cache invalidation.

Compose may request foreground materialization if startup synchronization has not completed, but it
must not implement a separate identity-fetch path with different cache semantics.

### Incremental synchronization

Add typed support for `Identity/changes` using the standard RFC 8620 `/changes` shape.

When an Identity state change is received:

1. read the cached Identity state;
2. call `Identity/changes` until `hasMoreChanges` is false;
3. fetch created and updated IDs with `Identity/get`;
4. apply fetched identities, destroyed IDs, and the new state atomically; and
5. publish one bounded invalidation.

If the state token is missing, rejected, or cannot be advanced coherently, fall back to a complete
`Identity/get` replacement. Do not guess at partial state.

Add `Identity` to WebSocket/EventSource push subscriptions for accounts with mail submission support.
`StateChangePolicy` must classify Identity changes as relevant to compose/sender presentation, not as
message-list changes.

### Cache replacement

After a cache rebuild, Identity sync repopulates signatures from the server. No migration or restore
from `QSettings` is performed.

An already open compose window retains the body and selected Identity ID in its working copy. If that
Identity has not yet been rematerialized, sending remains disabled with a clear status until the
identity is confirmed again.

## JMAP method support

Add typed `Identity/set` request and response structures to `MailMethods` rather than constructing raw
JSON in application code.

### Create

Creating a signature variant creates a JMAP Identity with:

- the same `email` as the source identity;
- an explicitly chosen `name`;
- copied or edited `replyTo` and `bcc` values;
- `textSignature`; and
- `htmlSignature`.

The server may reject creation with `forbiddenFrom`, `forbidden`, `invalidProperties`, or another
standard SetError. Javelin displays that failure and creates no local-only substitute.

### Update

Existing identities may update:

- `name`;
- `replyTo`;
- `bcc`;
- `textSignature`; and
- `htmlSignature`.

The immutable email address is never placed in an update patch.

Use exact RFC 8620 PatchObject paths where individual map entries are changed. For the signature
strings themselves, replacing the complete scalar property is correct.

### Destroy

The Delete action is disabled when `mayDelete` is false. A true value permits attempting destroy but
does not allow Javelin to assume acceptance; the SetResponse remains authoritative.

Javelin must not permit deleting the Identity currently required by an unsent working copy without a
clear confirmation. Existing drafts retain their selected Identity ID; if the server identity is
deleted, those drafts require another sender identity before send.

### Set response fidelity

The method layer must retain structured `notCreated`, `notUpdated`, and `notDestroyed` SetErrors,
including their type, description, and invalid property paths. The application layer converts those
into useful user-facing errors without hiding the server reason.

## Optimistic consistency

Identity mutations are persistent JMAP mutations and must use the optimistic-consistency subsystem.
They do not bypass it merely because they are initiated from a management dialog.

### Updates

An update transaction:

1. records the mutation with the previous and proposed Identity values;
2. projects the proposed fields into the confirmed identity row;
3. advances the Identity consistency generation;
4. commits; and
5. publishes invalidation.

Acceptance reconciles the returned state and refreshed server object. Rejection restores the previous
confirmed fields. An ambiguous transport outcome remains `unknown`; subsequent Identity refresh
rebases the active projection over the newly confirmed server state.

### Creates

A create has no server Identity ID until accepted. Do not invent a fake JMAP ID inside the confirmed
`identities` table.

Use a separate `identity_create_projections` table keyed by account ID and local creation UUID. The
identity manager model presents these rows as pending, while compose only permits confirmed identities
with real server IDs.

On acceptance, atomically:

- insert the returned confirmed Identity;
- remove the create projection;
- reconcile the mutation record and state; and
- publish invalidation.

On rejection, remove the projection and expose the failure. On an unknown outcome, keep the projection
visibly uncertain until a refresh can match the created Identity or the user resolves the ambiguity.
Matching must use the mutation's creation identity and SetResponse semantics, not fuzzy signature text.

### Destroys

A destroy projection hides the confirmed Identity from new selections while retaining enough before
state for rejection and recovery. Existing open drafts that reference it remain visible but cannot be
sent unless the destroy is rejected or another confirmed identity is selected.

## Identity and signature management UI

Server identities are not ordinary application preferences. Editing them performs remote mutations
that cannot be rolled back merely by pressing Cancel in `KConfigDialog`. Use a standalone
**Sending Identities and Signatures** dialog rather than storing staged edits in the Preferences
settings snapshot.

The dialog is also reachable from the compose Signature menu.

### Layout

The left pane groups identities by JMAP account. Each row displays:

- display name and email address;
- a derived signature label from the first non-empty plain-text signature line;
- `No signature` when both signature fields are empty; and
- pending, failed, or uncertain mutation state when applicable.

The derived label is presentation only and is never persisted. When rows remain indistinguishable,
append a stable ordinal within the account rather than exposing a JMAP ID.

The right pane contains:

- editable display name;
- read-only email address for an existing identity;
- Reply-To and automatic-Bcc editors;
- a **Rich Text** toggle;
- the same formatting toolbar and `JavelinComposerEdit` used by message composition; and
- explicit **Save** and **Revert** actions.

Save submits an Identity mutation. Revert reloads the currently confirmed/projected cache value; it
does not attempt to reverse a server update that has already been accepted.

### Actions

- **New Identity…** asks for an allowed sender address and initial values, then attempts
  `Identity/set` create.
- **Duplicate** copies the selected identity, keeping the same email address, and opens the copy for
  editing before create. This is the primary multiple-signature workflow.
- **Delete** attempts destroy and is disabled when `mayDelete` is false.
- **Refresh** explicitly rematerializes Identity state when required.

No action offers "save locally" after a server rejection.

### Signature editor semantics

For rich text:

- store clean composer HTML in `htmlSignature`;
- derive `textSignature` with `JavelinComposerEdit::toCleanPlainText`; and
- persist both properties together.

For plain text:

- store the clean text in `textSignature`;
- generate `htmlSignature` with `htmlFromPlainText`; and
- persist both properties together.

This gives every identity coherent multipart signature variants while still using one editor surface.
The rich-to-plain conversion warning and markup option match normal composition.

Local image insertion and image paste are disabled. `Identity.htmlSignature` has no standard relation
to uploaded blobs or CID attachments, so Javelin will not create a private asset system around it.
Text formatting, links, lists, tables, colours, and other self-contained markup remain available after
normal composer sanitization.

## Compose behaviour

### Sender selection

The From selector lists every confirmed Identity. It no longer deduplicates by email address.

Display text uses:

```text
Name <email> — derived signature preview
```

The preview is omitted when there is only one identity for that email address. The account name is
included when needed to distinguish identities across accounts.

Selecting an identity changes both:

- the sender/submission Identity ID; and
- the default signature content associated with that identity.

### Initial insertion

A newly created compose working copy receives exactly one signature from its selected server Identity:

- rich-text mode prefers `htmlSignature`, falling back to converted `textSignature`;
- plain-text mode prefers `textSignature`, falling back to converted `htmlSignature`;
- an empty result inserts nothing.

Placement is:

- after the authored area in a new message;
- before the attribution and quoted content in replies/reply-all; and
- before the forwarded-message header and content in forwards.

The cursor starts in the authored area before the signature.

EditDraft, an existing working copy, and send paths never perform signature insertion. Their visible
body is authoritative.

### Signature menu

Add a compose-toolbar **Signature** menu with:

- **Use Identity Signature**;
- **No Signature** for this message;
- a **Signature Variant** submenu containing confirmed identities with the same email address;
- **Edit Current Signature…**; and
- **Manage Identities and Signatures…**.

Choosing another same-address variant changes the underlying `identityId` as well as the signature.
The sender address therefore remains visually unchanged while the server submission identity is
correct.

Choosing **No Signature** removes the tracked signature only from this draft. It does not alter the
server Identity.

### Safe identity changes

`ComposeSignatureController` tracks the automatically inserted signature as a live document range and
records the exact inserted representation.

When identity changes:

- an unmodified tracked signature is replaced by the new identity's signature;
- a modified signature is left untouched;
- a signature the user explicitly removed is not automatically reinserted; and
- the selected Identity ID still changes, so the composer indicates that the body signature is
  custom for this message.

Edits, replacements, and removal participate in the document Undo stack.

On rich/plain mode conversion, an unmodified tracked signature is removed before conversion and
reinserted from the selected Identity's native target representation. A modified signature converts
with the rest of the document.

### Draft reopen

Javelin does not need to recover which server signature originally produced arbitrary draft text.
The saved body is authoritative. Tracking may be recovered only when the selected Identity's current
signature exactly matches the standard signature location; otherwise the draft is shown as custom and
is never silently rewritten.

## Process ownership

### JMAP library

Owns:

- typed `Identity/get`, `Identity/changes`, and `Identity/set` methods;
- protocol validation and structured SetErrors;
- identity cache repository primitives;
- Identity synchronization; and
- exact policy-neutral create, update, and destroy operations.

### Daemon application coordination

Owns:

- typed GUI commands for identity management;
- account/credential resolution;
- optimistic mutation admission and reconciliation;
- background and foreground Identity refresh;
- partial-failure policy; and
- post-commit cache invalidations.

### GUI

Owns:

- the identity/signature management dialog;
- the shared editor presentation;
- compose identity and signature menus;
- live document-range tracking; and
- user-visible validation and errors.

The GUI reads identity state from SQLite and sends typed commands over IPC. It never writes Identity
rows or invokes JMAP directly.

## Protocol changes

Add bounded typed commands for:

- create Identity;
- update Identity;
- destroy Identity; and
- request Identity refresh.

Signature bodies need dedicated bounded fields rather than weakening the normal 4096-byte identifier
and label limit. Suggested limits:

- 128 KiB UTF-8 for each text or HTML signature representation;
- 256 KiB aggregate per Identity mutation;
- 256 identities per account; and
- the existing 1 MiB frame limit unchanged.

Add `ChangedDomain::SenderIdentities` so the GUI can refresh From menus and the management model without
pretending that message metadata changed.

## Error and offline behaviour

- Viewing and composing from already cached identities works offline.
- Creating, editing, or deleting an identity requires network access and is queued/admitted through
  the normal mutation path.
- A pending update is visible optimistically.
- A pending create cannot be selected for sending until it has a server Identity ID.
- A rejected or ambiguous mutation remains visible with a clear state and actionable error.
- No failure produces a local-only signature copy.

## Tests

### Method and parsing tests

- Identity create/update/destroy requests serialize exactly;
- immutable email is absent from update patches;
- SetResponses retain complete created/updated/destroyed and SetError detail;
- `Identity/changes` pagination and state parsing are correct; and
- signature size and invalid HTML inputs are rejected at the correct boundary.

### Repository and synchronization tests

- full Identity/get replacement stores objects and state atomically;
- incremental create/update/destroy commits state atomically;
- invalid or missing state falls back to full replacement;
- duplicate-email identities are preserved;
- cache deletion and resync restore server signatures;
- push Identity changes trigger bounded refresh; and
- cache invalidation follows commit.

### Optimistic consistency tests

For update, create, and destroy:

- projection;
- acceptance;
- rejection;
- ambiguous transport outcome;
- stale-refresh rebasing;
- process restart and retry recovery; and
- partial SetResponse failure.

Create tests must verify that pending rows never masquerade as confirmed JMAP identities.

### GUI tests

- duplicate-email identities remain separately visible and selectable;
- derived signature labels are deterministic and never persisted;
- duplicate creates a same-address Identity request;
- non-deletable identities disable Delete;
- rich and plain editor modes produce coherent signature pairs;
- image insertion and paste are unavailable;
- server errors do not offer local fallback; and
- management updates refresh open From menus without rewriting open message bodies.

### Compose tests

- exact placement for new, reply, reply-all, and forward;
- existing drafts and working copies never receive duplicate signatures;
- identity changes replace only unmodified tracked signatures;
- explicit removal is respected;
- same-address signature variant selection changes `identityId`;
- rich/plain conversion uses native variants for unmodified signatures;
- send and delayed-send submit the selected confirmed Identity ID; and
- sender Bcc and Reply-To behaviour remains correct.

## Implementation order

1. Add typed `Identity/changes` and `Identity/set` method support with structured SetErrors.
2. Extend the identity repository with state-token and incremental transactional operations.
3. Add Identity to account startup synchronization and push handling.
4. Add optimistic Identity mutation records and projections, including separate pending-create rows.
5. Add typed daemon commands, invalidations, and GUI read-model refresh.
6. Remove compose email-address deduplication and update identity display disambiguation.
7. Add the Sending Identities and Signatures dialog with the shared composer editor.
8. Add reply/forward insertion, the Signature menu, and safe live replacement tracking.
9. Run focused mutation, restart, cache-rebuild, compose, and full-suite regression verification.
