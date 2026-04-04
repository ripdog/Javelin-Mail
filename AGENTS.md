# AGENTS.md

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
