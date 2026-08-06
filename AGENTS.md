# AGENTS.md

## Working Rules

- Do not run Git commands in parallel or run concurrent configure, build, or test commands against the same build directory.
- Use the Qt 6/C++23 codebase directly. Do not add legacy fallbacks or accept old and new data shapes unless an explicit migration requires it.
- Read `docs/ARCHITECTURE.md`, `docs/DEVELOPMENT.md`, and any relevant subsystem document before changing an architectural boundary.
- Commit meaningful changes at the end of the task.

## Source of Truth

- JMAP behavior follows `specs/rfc8620.txt`, `specs/rfc8621.txt`, and applicable extension specifications. The specification wins over implementation convenience.
- `docs/ARCHITECTURE.md` defines process and component ownership.
- `docs/OPTIMISTIC_CONSISTENCY.md` defines persistent mutation and refresh behavior.
- Unsupported capabilities must fail clearly rather than silently selecting an alternate protocol path.

## Architecture

- The daemon owns JMAP transports, writable repositories, canonical settings, background work, authentication, and stateful application commands.
- The GUI renders cache state and sends typed commands over IPC. It must not perform JMAP operations or writable mail-cache access directly.
- The internal JMAP library owns protocol validity, typed wire/domain data, cache primitives, sync primitives, and exact policy-neutral mutations.
- The application coordination layer interprets user intent, expands selections, orchestrates multi-object work, and owns partial-failure policy.
- Keep transport and raw JMAP JSON out of GUI and product-policy code. Use Glaze for JMAP wire JSON and typed structures across boundaries.
- Keep protocol, cache, sync, and application policy testable without starting Qt Widgets.

## State, Memory, and Async Work

- SQLite is the immediate local data plane. Do not create a second in-memory source of truth or optimistic object store in the GUI.
- Keep long-lived models lightweight; retain IDs and summaries and load full objects only for active views.
- Offline-selected mailboxes are complete mirrors. Raw MIME and attachments belong in the filesystem vault, not SQLite BLOBs; other mailboxes remain bounded online-first working sets.
- Do not block the GUI thread for I/O, database work, HTML processing, translation, or networking.
- Use Qt asynchronous transports with QCoro where appropriate. Do not use nested callback pyramids, local event loops, or ad hoc polling state machines.
- Use model/view for large datasets rather than duplicating them in item widgets.

## C++ and Qt Conventions

- Prefer strong types, value semantics, explicit ownership, narrow APIs, and small focused classes.
- Use stack ownership first, `std::unique_ptr` for exclusive heap ownership, and QObject parent ownership only for natural QObject hierarchies. Avoid shared ownership unless the design requires it.
- Use typed signal/slot connections and the Qt macro keywords required by `QT_NO_KEYWORDS`.
- Follow the compiler-enforced Qt string rules: use `QStringLiteral`, `QString::fromStdString`, and explicit Latin-1 forms rather than implicit ASCII conversions.
- Keep headers minimally coupled and avoid global mutable state outside explicit bootstrap-owned services.

## Optimistic Consistency

- Every persistent JMAP mutation must use the optimistic-consistency subsystem; do not add direct `/set` or `/copy` cache-writing paths around it.
- Persist the mutation and materialize its projection atomically. Reconciliation, cache updates, state tokens, and consistency generations must also commit atomically.
- Treat ambiguous transport outcomes as `unknown`, not success or rejection. Rebase active projections over refreshed confirmed state so stale snapshots never flash old state back into the UI.
- Use exact RFC 8620 PatchObject paths for changed map entries rather than replacing whole maps.
- New mutations require deterministic coverage for projection, success, rejection, ambiguity, stale-refresh rebasing, and crash/retry recovery.

## Change Safety

For any non-trivial change:

1. Identify the invariant being changed before editing.
2. Search for every reader, writer, serializer, validator, test, and persisted representation of that invariant.
3. Prefer changing the type or data model over using empty values, sentinels, or special cases.
4. Test through the real production path, including restart, failure, ordering, or concurrency where relevant.
5. After implementation, perform a separate regression review: assume the change is wrong and look for distant consequences.
6. Run focused tests, the affected production build, and the full normal test suite before committing.

Do not treat the named file or visible symptom as the full scope of the task.

## Build and Verification

- In the shared checkout, use `scripts/check-debug.sh` so configuration, compilation, and tests are serialized.
- Before building or testing, create `/tmp/javelin-mail-xdg-runtime` with mode `0700` and set `XDG_RUNTIME_DIR` to it.
- During implementation, build the narrowest relevant target and run focused tests. Use `scripts/check-debug.sh --full` for final verification.
- Prefer deterministic fixtures and scripted transports over live network tests.
- Keep changes warning-free and `clang-format` clean. Do not run clang-format on non-C++ files.
- Use the sanitizer and static-analysis workflows described in `docs/DEVELOPMENT.md` when the risk profile warrants them; CI runs the normal sanitizer suite.
- Do not terminate Ninja merely because compilation is slow. Let interrupted processes exit before starting another build.

## User-Facing and Diagnostic Messages

- Reserve UI status messages for actionable progress, errors, and meaningful outcomes; do not narrate internal housekeeping.
- Diagnostics should describe useful state or outcomes and include relevant identifiers when they aid debugging.
- Never turn an implementation instruction into product copy or a status label.
