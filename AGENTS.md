# AGENTS.md

## General instructions
Do not run git commands in parallel - you will hit git lock file contention.
When building, use the cmake debug preset always.
Tokens are precious - do not do wasteful things like re-reading files after clang-format, run rebuilds after formatting. Do not read files already in your context - edit boldly. The edit tool will stop you making invalid edits.
If you have made meaningful changes during your turn, always commit at the end.

This repository is for a Qt Widgets JMAP email client. Treat it as a modern-only codebase:

- Target Qt 6 and C++20 directly.
- Do not add fallbacks for older Qt, older compilers, or alternate protocol paths.
- Do not preserve legacy patterns "just in case".
- Prefer removing obsolete code over abstracting around it.
- In this greenfield codebase, "no fallbacks" also means no dual config keys, no alternate database readers, no compatibility dictionary lookups, and no code that accepts both old and new shapes unless a live migration step explicitly requires it.

## Source Of Truth

- JMAP behavior must follow the specifications in [`specs/rfc8620.txt`](/home/ripdog/CLionProjects/Javelin-Mail/specs/rfc8620.txt) and [`specs/rfc8621.txt`](/home/ripdog/CLionProjects/Javelin-Mail/specs/rfc8621.txt).
- If implementation convenience conflicts with the spec, the spec wins.
- Keep protocol logic inside the internal JMAP library. The GUI must not construct raw JMAP JSON or understand transport details.
- JMAP capabilities must be negotiated explicitly. Unsupported server features should fail clearly and early rather than degrading into alternate code paths.

## Programming Preferences

- Use modern C++20 idioms aggressively: RAII, value semantics, move semantics, `enum class`, `std::optional`, `std::variant`, `std::span`, `std::chrono`, designated helper structs where they improve clarity.
- Prefer explicit ownership. Use stack values first, `std::unique_ptr` for exclusive heap ownership, and Qt parent ownership only where QObject lifetime is naturally hierarchical.
- Avoid shared ownership unless it is required by the design and documented at the point of use.
- Use typed signal/slot connections only. Do not use string-based `SIGNAL` or `SLOT`.
- Prefer narrow, strongly typed APIs over loosely structured data bags.
- Keep headers clean. Minimize transitive includes and prefer forward declarations where valid.
- Prefer free functions and small focused classes over monolithic managers.
- Avoid global mutable state. If singleton-like process services are required, keep them explicit in the bootstrap layer.
- Do not block the GUI thread for I/O, database access, HTML processing, translation, or network calls.

## Qt And Async Conventions

- Networking must use `QNetworkAccessManager` with QCoro coroutines.
- Do not write callback pyramids, nested lambdas, ad hoc state machines, or local event-loop hacks for async work.
- Parse JSON with `stephenberry/glaze` into typed protocol/domain structures. Do not pass `QJsonObject` through the application as a de facto domain model.
- Use Qt model/view properly. Avoid item-based widgets for large mail datasets.
- Prefer `QAbstractItemModel`-based models backed by the cache rather than in-memory duplication.
- Use `QProperty`/bindable properties where they simplify state propagation cleanly.

### Qt String and Keyword Conventions

KDE cmake settings enable `QT_NO_CAST_FROM_ASCII` and `QT_NO_KEYWORDS`. All code must comply:

- **Never pass raw string literals (`"..."`) to Qt APIs.** Use `QStringLiteral("...")` for compile-time string literals passed to `QString`-accepting parameters (e.g., `query.prepare()`, `query.bindValue()`, `query.exec()`, `QDir::filePath()`, `QDate::toString()`).
- **For `QString::replace()` with single characters**, use `QLatin1String` for the search argument: `str.replace(QLatin1String("&"), QStringLiteral("&amp;"))`.
- **For `std::string` → `QString` conversion**, use `QString::fromStdString()`.
- **Use macro-based Qt keywords**, not the keyword forms: `Q_SIGNALS:` not `signals:`, `Q_SLOTS:` not `slots:`, `Q_EMIT` not `emit`.
- **In test assertions**, compare `QString` against `QStringLiteral(...)`, not raw string literals: `CHECK(value == QStringLiteral("expected"))`.

## Memory And Cache Policy

- The application is expected to run continuously for notifications, so memory pressure matters at all times.
- `QSqlDatabase` is the local system of record for synced state, not a secondary convenience cache.
- Persist enough server state locally to minimize redundant JMAP round-trips and to support fast startup.
- This is an online-first client, not a full-offline client. Do not expand the cache toward a full local mirror unless there is a strong, measured reason.
- Keep only active UI state and short-lived working sets in memory.
- Message bodies, large MIME sections, rendered HTML artifacts, and attachment metadata should be loaded on demand and released promptly.
- Favor IDs and lightweight summaries in long-lived models. Fetch full objects only when a view needs them.

## Library Boundary

- JMAP code lives in an internal library with no dependency on widgets or WebEngine.
- The GUI layer may consume typed entities, view models, commands, and service interfaces from the JMAP library, but it must not know raw protocol method names or JSON wire layouts.
- Keep transport, sync, cache, and domain logic testable without starting a GUI.
- Authentication, token refresh, and secret storage policy belong to the non-GUI service layers, not to ad hoc dialog code.

### Application Coordination Boundary

- Keep application and product logic out of the internal JMAP library. The library owns JMAP
  protocol mechanics, typed protocol/domain data, capability handling, cache primitives, sync
  primitives, and exact mutations requested by its callers.
- The application coordination layer owns interpretation of user intent and UI context. This
  includes workflow policy, multi-object orchestration, selection expansion, role-based actions,
  batching decisions, partial-failure policy, refresh coordination, and deciding which exact JMAP
  or cache mutations implement an application command.
- Prefer typed, policy-neutral library APIs. For example, the library may apply an explicit email
  mailbox patch containing mailbox IDs to add and remove; it must not decide what "move from a
  search tab" or another UI-specific command means.
- GUI code should raise typed application commands and render their outcomes. It must not bypass
  the coordination layer to assemble protocol operations or embed cross-service workflow policy.
- When responsibility is ambiguous, place protocol validity and transactional cache integrity in
  the JMAP library, application semantics and orchestration in the coordination layer, and visual
  interaction only in the GUI.

### Optimistic Consistency Foundation

All stateful JMAP actions must build on the architecture in
[`docs/OPTIMISTIC_CONSISTENCY.md`](/home/ripdog/CLionProjects/Javelin-Mail/docs/OPTIMISTIC_CONSISTENCY.md).
Do not add a direct `/set` or `/copy` path that writes the cache outside this subsystem.

- SQLite renders the effective state: confirmed server state plus active mutation projections.
  The GUI must not keep a second optimistic object store.
- Persist every mutation before dispatch with a typed service adapter and the generic lifecycle:
  `pending`, `in_flight`, `accepted`, `rejected`, or `unknown`.
- Append a mutation and materialize its projection atomically with
  `MutationProjectionTransaction`. Accept/reject reconciliation, cache changes, state tokens, and
  consistency-generation changes must likewise be one transaction.
- Treat transport ambiguity as `unknown`, never success or rejection. Startup recovery converts
  leftover `in_flight` records to `unknown`.
- A refresh must capture a per-account, per-data-type generation fence. It may commit only if still
  causally current, unless its typed adapter rebases every active projection into the same cache
  transaction.
- Stale server snapshots must never make an optimistic object flash back to its old state. Rebase
  `pending`, `in_flight`, and `unknown` projections over refreshed confirmed state.
- Retire an unknown mutation only when a server snapshot proves the requested outcome. If a lost
  create response cannot be correlated safely, preserve uncertainty and block duplicate submission
  of the same logical command.
- Definitive per-object JMAP failures restore the confirmed state immediately. Partial successes
  and failures reconcile independently.
- Use exact RFC 8620 PatchObject paths for changed map entries. Do not replace whole collection
  properties when the user changed only one membership or keyword.
- Cross-account or cross-type workflows are application-owned operation groups with explicit
  dependencies and partial-failure policy; typed JMAP adapters remain policy-neutral.
- Procedural operations such as uploads, downloads, validation, and reads do not need optimistic
  records unless they mutate persistent JMAP object state.

Every new mutation requires deterministic tests for projection, success, rejection, ambiguous
transport outcome, stale refresh rebasing, and crash/retry safety.

## Static Analysis And Quality Gates

- Keep the codebase `clang-format` clean. Do NOT run clang-format on non-code, such as CMakeLists.txt
- Run `clang-tidy` regularly and fix warnings instead of normalizing them.
- Run clazy for Qt-specific issues and treat findings seriously.
- Use AddressSanitizer in debug/test workflows by default where supported.
- Add narrowly scoped suppressions only when the warning is understood and the reason is documented.
- New code should compile warning-free under the project warning policy.

## Testing Expectations

- Unit-test the internal JMAP library with Catch2.
- Prefer deterministic tests over network-dependent tests.
- Protocol parsing, state transitions, cache reconciliation, query windows, and notification flows should all be testable from canned data.
- Regressions around sync state, cache eviction, and background notification behavior require tests.
- Add a fake or scripted JMAP test harness once the transport layer exists so sync and long-poll behavior can be tested beyond static fixtures.

## Build And Dependency Preferences

- Use CMake as the single build entrypoint.
- Prefer explicit targets with clear dependency boundaries.
- External dependencies should be introduced deliberately and kept minimal.
- Use `FetchContent` for project-managed third-party dependencies when reproducibility matters.
- Keep static analysis, sanitizer, and formatting support wired into the build or documented scripts early, not as an afterthought.

## Change Discipline

- Make focused changes with a clear architectural reason.
- Do not sneak protocol, storage, and UI concerns into the same class.
- When adding a new subsystem, create the intended boundary first, then implement within it.
- Update documentation when architecture or workflow expectations change.

## User-Facing And Diagnostic Messages

- Do not emit routine internal lifecycle or procedural messages merely to narrate what the
  application just did (for example, connection restarts, cache updates, or task completion).
- Console diagnostics should explain a useful state or outcome, with relevant identifiers or
  values when they help debugging. Prefer messages such as `Update watched mailboxes to Inbox,
  Sent` over implementation narration such as `Updated watched mailboxes without restarting the
  state-change source`.
- UI status messages should be reserved for user-actionable progress, errors, and meaningful
  outcomes. Do not surface background autosave or other internal housekeeping notifications.
- Never turn an implementation instruction into product copy or a status label. User requests are
  design input for the change, not text that should appear in the application.
