# Compose And Send Plan

## Status

This is the original compose/submission implementation plan, retained as design history. The current
product no longer follows several proposed presentation details: compose is tab-hosted, daemon-owned
services perform draft/save/send work across typed IPC, attachments are copied into immutable daemon
staging, and delayed send continues after the GUI exits. The implemented runtime boundaries are
defined in [ARCHITECTURE.md](ARCHITECTURE.md),
[DAEMON_GUI_ARCHITECTURE.md](DAEMON_GUI_ARCHITECTURE.md), and [UNDO_REDO.md](UNDO_REDO.md).

The protocol, normalization, MIME, reply/forward, and testing discussions below still provide useful
rationale, but future work should use [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) as the roadmap rather
than treating every proposed class or window shape in this document as current.

## Goal

Make composing and sending feel like a first-class desktop mail workflow, while keeping the implementation aligned with the repository's existing architecture:

- typed JMAP logic in `src/jmap/`
- widgets-only UI in `src/gui/`
- durable local state in SQLite
- QCoro-based async boundaries
- no raw JMAP JSON in the GUI
- no protocol fallbacks when the account cannot support the feature correctly

This feature should cover:

- new message
- reply
- reply all
- forward
- reopen/edit saved draft
- autosave
- attachment upload
- send through `EmailSubmission/set`
- a rich-text editor with a standard toolbar
- a deliberate fallback to raw HTML for advanced users

This feature should not require:

- a browser-based editor
- callback-heavy async state machines
- the GUI knowing `Email/set` or `EmailSubmission/set`
- lossy conversions hidden from the user

## Product Direction

### Interaction model

The compose experience should be built around a reusable central widget and hosted in a dedicated top-level compose window.

Preferred structure:

- `ComposeEditorPanel`
- `ComposeWindow`

Why this shape:

- the editor stays reusable if we later want a tab-hosted compose surface
- composing stays independent from mailbox refresh churn in `MainWindow`
- users can browse mail while keeping one or more drafts open

### Compose entry points

We should support the following launch intents:

- `NewMessage`
- `Reply`
- `ReplyAll`
- `Forward`
- `EditDraft`

Each intent becomes a typed request into the non-GUI compose service. The GUI should only provide the source ids and desired mode.

## Editor Choice

### Primary WYSIWYG editor

Use `QTextEdit` with `QTextDocument` as the primary rich-text editor.

This is the right default for v1 because:

- it is native to Qt Widgets
- it fits the existing desktop application style
- it avoids shipping a second HTML editing stack inside WebEngine
- formatting commands map cleanly to `QTextCursor` and `QTextCharFormat`
- it keeps the editor responsive and testable without a browser runtime

We should not use a `QWebEngineView`-based editor as the primary compose surface. That would add a separate DOM editing stack, JavaScript bridge complexity, and much harder state synchronization for draft autosave.

### Standard formatting controls

The rich editor should expose a normal mail-toolbar set:

- bold
- italic
- underline
- strikethrough
- text color
- highlight color
- clear formatting
- bulleted list
- numbered list
- increase indent
- decrease indent
- block quote
- insert/edit link
- remove link
- align left
- align center
- align right
- undo
- redo

We should intentionally avoid font-family pickers and arbitrary HTML widgets in the first cut. Rich outbound mail should stay semantically simple and predictable.

### Raw HTML fallback

Provide a raw HTML mode beside the rich editor.

Preferred widget stack:

- `QStackedWidget`
- rich page: `RichTextComposeEditor`
- HTML page: `QPlainTextEdit`
- preview page: reuse `HtmlMessageView`

Mode policy:

- rich mode is the default
- switching from rich mode to HTML mode shows the normalized generated HTML
- once the user manually edits raw HTML, the session is marked as `rawHtmlAuthoring = true`
- switching back to rich mode must be explicit, because the conversion can be lossy

That policy matters. We should never silently destroy custom HTML that `QTextDocument` cannot round-trip.

### Recipient and attachment widgets

Use purpose-built widgets, not a giant form of plain `QLineEdit`s:

- `RecipientField`
- `RecipientChipModel`
- `AttachmentListModel`
- `AttachmentStrip`

Preferred UI behavior:

- token/chip style recipients for To/Cc/Bcc
- keyboard-friendly recipient editing
- later-ready for `QCompleter` when contacts exist
- drag-and-drop attachments
- paste files into the editor to attach them
- per-attachment progress and retry state

## Outbound Message Model

The current `domain::Email` type is read-oriented and too small for compose. Compose needs a separate draft model.

Preferred types under `src/jmap/submission/`:

```cpp
enum class ComposeMode
{
    NewMessage,
    Reply,
    ReplyAll,
    Forward,
    EditDraft,
};

enum class BodyEditorMode
{
    RichText,
    RawHtml,
};

struct DraftAttachment
{
    QString localFilePath;
    QString displayName;
    std::string mediaType;
    std::uint64_t size = 0;
    std::optional<std::string> blobId;
    bool inlineDisposition = false;
    std::optional<QString> contentId;
};

struct ThreadingContext
{
    std::optional<std::vector<std::string>> messageId;
    std::optional<std::vector<std::string>> inReplyTo;
    std::optional<std::vector<std::string>> references;
};

struct DraftSnapshot
{
    std::string composeSessionId;
    std::string accountId;
    std::optional<std::string> draftEmailId;
    ComposeMode mode = ComposeMode::NewMessage;
    BodyEditorMode editorMode = BodyEditorMode::RichText;
    std::string identityId;
    std::vector<javelin::jmap::domain::EmailAddress> to;
    std::vector<javelin::jmap::domain::EmailAddress> cc;
    std::vector<javelin::jmap::domain::EmailAddress> bcc;
    std::optional<std::string> subject;
    std::string plainTextBody;
    std::string htmlBody;
    ThreadingContext threading;
    std::vector<DraftAttachment> attachments;
};
```

Design rules:

- `DraftSnapshot` is the GUI-facing value object
- rich mode still stores both `plainTextBody` and `htmlBody`
- `htmlBody` is the source of truth for rich authoring
- `plainTextBody` is always regenerated before save/send from the same snapshot
- attachments carry upload state through `blobId`

## Required Domain Additions

Reply and forward need more metadata than we currently cache for `Email`.

We should extend the mail domain and caching layers to include:

- `messageId`
- `inReplyTo`
- `references`
- enough body/header access to build quoted replies cleanly

This should be fetched through typed JMAP properties, not raw header dictionaries in the GUI.

## High-Level Service API

The GUI should talk to a compose service, not directly to repositories or JMAP methods.

Preferred service split:

- `ComposeService`
- `SubmissionService`

`ComposeService` owns session lifecycle and draft persistence.
`SubmissionService` owns the server-facing save/send pipeline and can be reused by future non-GUI workflows.

Indicative API:

```cpp
struct ComposeError
{
    QString message;
};

template <typename T>
using ComposeResult = std::variant<T, ComposeError>;

struct OpenComposeRequest
{
    std::string accountId;
    ComposeMode mode = ComposeMode::NewMessage;
    std::optional<std::string> referenceEmailId;
    std::optional<std::string> draftEmailId;
};

struct DraftSaveSummary
{
    std::string composeSessionId;
    std::string accountId;
    std::string draftEmailId;
};

struct SendSummary
{
    std::string composeSessionId;
    std::string accountId;
    std::string draftEmailId;
    std::optional<std::string> submissionId;
};

class ComposeService
{
  public:
    [[nodiscard]] virtual QCoro::Task<ComposeResult<DraftSnapshot>>
    open(OpenComposeRequest request) = 0;

    [[nodiscard]] virtual QCoro::Task<ComposeResult<DraftSaveSummary>>
    saveDraft(const DraftSnapshot& snapshot) = 0;

    [[nodiscard]] virtual QCoro::Task<ComposeResult<SendSummary>>
    send(const DraftSnapshot& snapshot) = 0;

    [[nodiscard]] virtual ComposeResult<void>
    discard(std::string_view composeSessionId) = 0;
};
```

Why this API shape is attractive:

- it is built on immutable snapshots, which keeps GUI state and tests straightforward
- it hides transport details
- it composes naturally with autosave
- it keeps send as a first-class action instead of a side effect of saving
- it remains useful if we later add scheduled send or undo send

## Internal JMAP Submission Pipeline

Sending should be draft-first.

### Draft save pipeline

1. Validate the account session for mail and submission capability.
2. Load identities and mailbox roles from cache.
3. Normalize the outgoing body:
   - canonical HTML
   - plaintext alternative
4. Upload any local-only attachments to `uploadUrl`.
5. Materialize the snapshot into a typed `Email/set` create or update.
6. Persist returned draft ids into the local compose session.

### Send pipeline

1. Save the latest snapshot as a draft.
2. Create an `EmailSubmission/set` request referencing that draft email id.
3. Use `onSuccessUpdateEmail` to:
   - remove the drafts mailbox
   - add the sent mailbox
   - clear `$draft`
4. Cache the resulting submission metadata.
5. Close the compose session on success.
6. Leave the draft editable on failure.

The send path should not skip the draft save. That keeps attachment uploads, autosave, and send all converging on a single canonical email object.

### Why `onSuccessUpdateEmail`

RFC 8621 explicitly models send as `EmailSubmission/set` plus an implicit `Email/set` patch when `onSuccessUpdateEmail` is supplied. That is the cleanest v1 behavior for this client because it gives us deterministic sent/draft mailbox transitions without inventing alternate post-send policies.

## Low-Level API Work In `src/jmap/api/`

The current mail method layer only supports the narrow update form of `Email/set`. Compose needs the full typed API surface.

Add typed support for:

- `Identity/get`
- full `Email/set` create/update/destroy
- `EmailSubmission/set`
- upload endpoint handling

Preferred new types:

- `IdentityGetResponse`
- `EmailCreate`
- `EmailSetRequest`
- `EmailSetResponse`
- `EmailSubmissionCreate`
- `EmailSubmissionSetRequest`
- `EmailSubmissionSetResponse`
- `UploadResult`

Important implementation detail:

`EmailSubmission/set` with `onSuccessUpdateEmail` produces both an `EmailSubmission/set` response and an implicit `Email/set` response with the same method call id. The current `ResponseReader` only returns the first response for a call id. It must be extended to support:

- all responses for a call id
- filtering by expected method name
- reading more than one typed response from the same call

Without that change, send will be wired on a shaky parsing path.

## Repository And Cache Design

### Existing tables to use

Keep using the existing tables for server-backed state:

- `identities`
- `emails`
- `email_body_values`
- `email_parts`
- `submissions`

### New local working-copy table

Add a dedicated local compose table:

- `compose_sessions`

Preferred columns:

- `compose_session_id`
- `account_id`
- `draft_email_id`
- `mode`
- `editor_mode`
- `snapshot_json`
- `last_saved_at`
- `updated_at`

Why a local compose table is worth it:

- unsaved work survives process restarts
- autosave debounce stays cheap
- raw HTML edits do not need to be projected immediately into server state
- attachment upload retry state can survive transient failures

The JSON payload here is acceptable because compose sessions are loaded individually, not queried as list models.

### New repositories

Add:

- `IdentityRepository`
- `ComposeSessionRepository`
- `SubmissionRepository`

`SubmissionRepository` should wrap the already-present `submissions` table rather than letting `JmapCore` or the GUI read it directly.

## Body Normalization

We should treat outbound body construction as its own subsystem, not as ad hoc editor glue.

Add:

- `HtmlComposeNormalizer`
- `PlainTextAlternativeBuilder`
- `ReplyQuoteBuilder`

Responsibilities:

- remove obviously unsafe or unsupported HTML such as scripts and event handlers
- normalize inline styles to a conservative subset
- preserve block structure and links
- generate plaintext from HTML consistently
- build reply and forward quotes for both HTML and plaintext

This is especially important because the rich editor and raw HTML editor both feed the same outbound pipeline.

## Reply, Reply All, And Forward Semantics

### Reply

- default recipients come from `replyTo`, otherwise `from`
- subject is normalized to `Re: <subject>` if needed
- `inReplyTo` is set from the referenced message's `messageId`
- `references` appends the referenced message id to the referenced message's existing `references`

### Reply all

- start from reply semantics
- add original `to` and `cc` recipients except:
  - the chosen identity address
  - duplicates

### Forward

- subject is normalized to `Fwd: <subject>` if needed
- no reply threading headers are set
- include a quoted header block and original body in both HTML and plaintext

## Attachment Flow

Attachment handling should be asynchronous and eager.

Preferred behavior:

- selecting or dropping a file immediately creates a `DraftAttachment`
- upload begins in the background
- send is disabled while any attachment is still uploading
- failed uploads stay attached with retry affordances
- successful uploads store `blobId` and no longer require local file access for send

For v1, file attachments are enough. Inline-image authoring can be a follow-up once the base pipeline is stable.

## Mailbox And Identity Requirements

Compose should fail clearly and early if the account cannot support the feature correctly.

For v1, require:

- session mail capability
- session submission capability
- account mail capability
- account submission capability
- at least one identity
- a mailbox with role `drafts`
- a mailbox with role `sent`

This is intentionally strict. It is better than silently inventing alternate mailbox policies.

## GUI Components

Preferred widget breakdown under `src/gui/compose/`:

- `ComposeWindow`
- `ComposeEditorPanel`
- `ComposeHeaderWidget`
- `RecipientField`
- `FormattingToolBar`
- `RichTextComposeEditor`
- `RawHtmlEditor`
- `AttachmentStrip`
- `AttachmentListModel`
- `SendStatusBar`

Suggested layout:

- top row: From, To, Cc, Bcc, Subject
- second row: formatting toolbar
- center: editor stack
- bottom: attachment strip and send/save status

Expected actions:

- `Ctrl+Enter` send
- `Ctrl+S` save draft
- `Ctrl+Shift+H` switch to raw HTML mode
- `Esc` close with unsaved-work check

## Integration Points

### `DaemonServices`

Construct and expose:

- `ComposeService`
- `IdentityRepository`
- `SubmissionRepository`

### `MainWindow`

Add actions for:

- new message
- reply
- reply all
- forward
- edit draft

`MainWindow` should launch compose windows but should not own compose business logic.

## Testing Strategy

### Unit tests

Add deterministic tests for:

- `Identity/get` parsing
- full `Email/set` create/update request serialization
- `EmailSubmission/set` serialization and response parsing
- multi-response `ResponseReader` behavior for implicit `Email/set`
- reply threading header generation
- HTML normalization
- plaintext alternative generation
- attachment upload state transitions
- compose session repository round-trips

### Service tests

With a fake transport and canned session:

- open new compose session
- open reply session
- save draft create
- save draft update
- send success with `onSuccessUpdateEmail`
- send failure preserving draft state

### GUI tests

Keep these focused:

- toolbar actions mutate editor state
- switching to raw HTML shows normalized HTML
- raw HTML mode blocks silent rich-mode round-trip
- attachment strip reflects upload progress state

## Delivery Phases

### Phase 1

- full typed JMAP API for identities, draft create/update, submission create
- repositories for identities and compose sessions
- compose service without UI polish

### Phase 2

- dedicated compose window
- rich editor
- raw HTML mode
- autosave
- send flow

### Phase 3

- attachment upload
- reply/reply all/forward quoting polish
- draft restore after restart

### Phase 4

- submission status display from cached `submissions`
- optional undo-send support if the server exposes `undoStatus = pending`
- optional inline image authoring

## Recommended First Implementation Slice

The best first vertical slice is:

1. `Identity/get` plus `IdentityRepository`
2. full typed `Email/set` create/update support
3. full typed `EmailSubmission/set` support
4. `ResponseReader` multi-response support
5. `ComposeSessionRepository`
6. `ComposeService` with `open`, `saveDraft`, and `send`
7. minimal `ComposeWindow` using:
   - recipient fields
   - subject field
   - `QTextEdit`
   - formatting toolbar
   - raw HTML page

That slice gives us end-to-end value quickly without painting the architecture into a corner.
