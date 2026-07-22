# Database access

SQLite runs in WAL mode so reads from independent thread-owned connections may proceed while a
write is active. SQLite still permits only one writer. Javelin therefore coordinates writes in the
process before asking SQLite for its write lock.

## Connection ownership

- A `DatabaseConnection` belongs to the thread on which it is opened.
- Worker threads open their own connection with `ThreadConnectionFactory`; they never borrow the
  GUI connection.
- Connections to the same normalized database path share one recursive write coordinator.
- Separate per-account search-index databases have independent coordinators keyed by their paths.

## Writes

- Multi-statement changes use `DatabaseTransaction`. It acquires the database's write coordinator
  before `BEGIN IMMEDIATE` and holds it through commit or rollback.
- An autocommit write uses `DatabaseWriteScope` for the smallest block containing that write.
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

## Testing

Contention tests must use multiple real connections to the same temporary database. Mutex-only
tests do not prove that connection setup, transaction lifetime, and SQLite locking interact safely.
