# Diagnostics

## Split-process performance profiling

Set `JAVELIN_UI_PROFILING=1` before launching Javelin to enable opt-in performance
metrics in both processes. The daemon inherits the variable when the GUI starts it;
set it before starting an already-running daemon as well if daemon metrics are needed.

```sh
JAVELIN_UI_PROFILING=1 make run 2>javelin-performance.log
```

Each line is a machine-readable metric written through the normal Qt logging path:

```text
metric process=gui operation=remote_action_e2e outcome=completed duration_us=1842 details="kind=mail_queue_mark_read payload_bytes=96"
metric process=daemon operation=remote_action_execution outcome=completed duration_us=1321 details="kind=mail_queue_mark_read result_bytes=1"
metric process=daemon operation=process_resources outcome=sample details="rss_kib=... max_rss_kib=... user_cpu_us=... system_cpu_us=... wal_bytes=..."
```

The `process` field is important: GUI timings cover cache reads, navigation,
event-loop stalls, IPC admission, and end-to-end command completion, while daemon
timings cover handshake, command admission, network/materialization completion,
SQLite WAL size, CPU/RSS samples, and work-scheduler contention. Remote actions use
stable protocol names and never log account IDs, mailbox IDs, message content, or
credentials.

The resource sampler runs every 30 seconds only while profiling is enabled and
records cumulative CPU time, current/high-water RSS, WAL size, and scheduler
counters. `duration_us` is monotonic elapsed time; compare adjacent CPU samples to
estimate idle CPU use. The event-loop heartbeat runs every 50 ms and records stalls
over 200 ms, while individual Qt event handlers over 50 ms are recorded separately.
No profiling metric is persisted in SQLite.

Leave the variable unset for normal runs. The instrumentation, sampler timer, and
diagnostic logging are off by default.
