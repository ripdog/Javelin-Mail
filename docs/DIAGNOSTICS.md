# Diagnostics

## UI Event Loop Profiling

Set `JAVELIN_UI_PROFILING=1` before launching Javelin to enable GUI-thread stall
diagnostics:

```sh
JAVELIN_UI_PROFILING=1 make run
```

This enables:

- A 50 ms heartbeat timer that logs `UI event loop stall:` when the GUI event
  loop cannot run timers for more than 200 ms.
- Slow Qt event logging from `QApplication::notify()` when an individual event
  handler takes more than 50 ms.

Leave this unset for normal runs. The instrumentation is off by default.
