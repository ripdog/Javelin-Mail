# Future Refactor Notes

These are deferred architectural improvements. They are intentionally not prerequisites for finishing core mail functionality.

## Highest-value targets

### Split `MainWindow`

`src/gui/shell/MainWindow.cpp` is now the largest implementation file by a wide margin. It appears to own several separable concerns:

- mailbox and account navigation
- message-list loading and selection restoration
- message actions such as move, copy, archive, delete, and read state
- search lifecycle
- compose-tab lifecycle
- refresh/progress/status presentation
- window and tray behaviour

Prefer extracting focused controllers or coordinators without changing the visible UI. Good first candidates are message actions, mailbox/message-list coordination, and search coordination. Keep models and domain operations out of the window class.

### Narrow `JmapCore`

`JmapCore` is becoming a broad facade for refresh, mailbox paging, search, downloads, pending mutations, and mutation submission. Preserve it as a temporary compatibility facade if useful, but move behaviour toward narrower services such as:

- mailbox sync/query service
- message-content service
- message-mutation service
- attachment/source download service
- search service

The GUI should consume domain operations rather than a single all-purpose core object.

### Split large feature implementations

The next-largest concentration points are:

- `MessageViewContainer`
- `ComposeService`
- `MailMethods`
- `ComposeTabWidget`
- `ContactsManagerWidget`
- `MailboxRefreshExecutor`

Split only when a feature boundary is clear. Avoid mechanical file splitting that leaves the same class responsibilities intact.

## Boundary cleanup

### Remove WebEngine from `javelin_jmap`

The development plan says the internal JMAP library must not depend on WebEngine, but `javelin_jmap` currently links publicly against `Qt::WebEngineCore`. Move presentation-specific HTML/WebEngine integration into the GUI or app layer. Keep reusable HTML sanitisation and document transformation independent of WebEngine where practical.

### Keep `ProcessServices` as the composition root

The explicit ownership in `ProcessServices` is a useful foundation. Continue constructing process-wide services there rather than introducing hidden globals or service locators.

## Build and documentation cleanup

- Decide whether the project standard is C++20 or C++23, then align `AGENTS.md`, the development plan, and CMake.
- Remove duplicate `MessageListDelegate.cpp/.h` entries from `JAVELIN_FORMAT_FILES`.
- Remove or restore the stale `LongPollMailboxObserverTest.cpp` formatting-list entry.
- Update `DEVELOPMENT_PLAN.md` where the intended repository layout has diverged from the current implementation.

## Suggested order

1. Finish essential receive, compose, mutation, notification, and account-management behaviour.
2. Add regression tests around any production bugs encountered during that work.
3. Extract message actions and mailbox/message-list coordination from `MainWindow`.
4. Split `JmapCore` behind its existing typed API.
5. Remove the WebEngine dependency from `javelin_jmap`.
6. Tackle compose and message-view decomposition based on profiling and change frequency.

The goal is incremental reduction of coupling while keeping every intermediate revision buildable and usable.
