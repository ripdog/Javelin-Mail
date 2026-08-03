# Database access

SQLite runs in WAL mode so independent thread-owned readers may proceed while a write is active.
`javelind` is the only process permitted to open the main cache for writing. It coordinates daemon
writes before asking SQLite for its single write lock; `javelin` opens only the read surface.

## Connection ownership

- A `DatabaseConnection` belongs to the thread on which it is opened.
- Daemon worker threads open their own connection with `ThreadConnectionFactory`; they never borrow
  another thread's connection.
- GUI connections use `ReadOnlyDatabaseConnection`, SQLite read-only mode, and
  `PRAGMA query_only=ON`. GUI code cannot run migrations or obtain writable repositories.
- Connections to the same normalized database path inside the daemon share one recursive write
  coordinator.
- Separate per-account search-index databases have independent coordinators keyed by their paths.
- Cache migration or replacement uses `CacheAccessBarrier`: the daemon requests suspension, the GUI
  closes every reader and acknowledges, and only then may the daemon replace or migrate the cache.

## Writes

- Multi-statement changes use `DatabaseTransaction`. It acquires the database's write coordinator
  before `BEGIN IMMEDIATE` and holds it through commit or rollback.
- An autocommit write uses `DatabaseWriteScope` for the smallest block containing that write.
- A write scope must end at the write or commit. It must never cover subsequent reads, window
  materialization, filesystem projection, signal delivery, or other follow-up work.
- Repository methods called from an existing transaction may take another `DatabaseWriteScope`;
  the coordinator is recursive for same-thread composition. They must not start a nested SQLite
  transaction.
- Filesystem, network, parsing, and other expensive preparation happen before the write scope.
  No coroutine may suspend while holding a write scope or transaction.
- New write paths must not use a bare `QSqlDatabase::transaction()` or execute an uncoordinated
  `INSERT`, `UPDATE`, or `DELETE`.

`busy_timeout` remains defense against another process accessing the file. It is not the in-process
scheduling mechanism. SQLite busy/locked failures at transaction acquisition are classified as
transient so refresh coordination can retry them without presenting a permanent storage error.

## Reads

Reads do not take the write coordinator. Finish or destroy `QSqlQuery` objects promptly, especially
before dispatching more work or crossing an asynchronous suspension point. Long-lived read
transactions prevent WAL checkpoint progress even though they do not block ordinary writers.
No `QSqlQuery` may remain active across `co_await`.

## Testing

Contention tests must use multiple real connections to the same temporary database. Mutex-only
tests do not prove that connection setup, transaction lifetime, and SQLite locking interact safely.
