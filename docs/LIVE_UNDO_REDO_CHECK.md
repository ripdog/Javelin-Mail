# Live undo/redo checker

`javelin-undo-live-check` runs persisted operation-history payloads through the production
undo/redo executors against the configured JMAP server. It does not move, delete, or rewrite
history entries.

Close Javelin Mail before running the checker. Each check temporarily changes the selected server
object, immediately applies the inverse operation, and repeats the cycle so both final-state
preconditions are verified. The starting server state is restored when both legs succeed.

Run autonomous, uniquely tagged fixtures against a specific configured connection:

```sh
out/build/debug/bin/javelin-undo-live-check \
  --autonomous \
  --connection CONNECTION_ID \
  --execute-live-mutations
```

The autonomous suite currently covers contact and contact-group create/edit/delete, group
membership, address-book create/edit/delete, calendar-event create/edit/delete, and draft
create/edit. Every action is applied through the production history executor and optimistic
mutation service. After each leg the suite verifies that no mutation-journal record belonging to
the fixture remains active, reads authoritative server state, and finally deletes any object in
the reserved `Javelin undo lab` fixture namespace.

List available history samples and missing command-kind coverage:

```sh
out/build/debug/bin/javelin-undo-live-check
```

Check one entry:

```sh
out/build/debug/bin/javelin-undo-live-check \
  --entry ENTRY_ID \
  --execute-live-mutations
```

Check all ready samples of one kind:

```sh
out/build/debug/bin/javelin-undo-live-check \
  --kind contact_card \
  --execute-live-mutations
```

Check the newest ready sample of every supported command kind:

```sh
out/build/debug/bin/javelin-undo-live-check \
  --all \
  --execute-live-mutations
```

The supported production executors are `mail_patch`, `draft`, `sieve`, `deferred_send`,
`calendar_event`, `calendar_preference`, `contact_card`, and `address_book`. The inventory reports
`MISSING` when the history database has no ready sample for a command kind. Perform that action
once in the application to provide a server-shaped sample, then rerun the checker. `--all` returns
failure while any supported kind is missing, so a partial run cannot be mistaken for complete
coverage.

If a return leg and its cleanup attempt both fail, the output is prefixed with `CLEANUP FAILED`.
Inspect the affected object before continuing with other live checks.
