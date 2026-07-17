# Configured JMAP query tool

`jmap-query` sends a complete JMAP request envelope through Javelin's JMAP
transport using one of the accounts already configured in the application. It
is intended for protocol diagnostics and does not print authentication tokens.

Build it with the debug preset:

```sh
cmake --build --preset debug --target jmap-query
```

List the configured accounts and their cached JMAP account IDs:

```sh
out/build/debug/bin/jmap-query --list-accounts
```

Send a request from a file:

```sh
out/build/debug/bin/jmap-query --account Stalwart --request request.json
```

The request may instead be supplied with `--json` or on standard input. Add
`--verbose` to print the selected account, API endpoint, and request envelope
to standard error. The parsed JMAP response is printed as formatted JSON on
standard output.

The input must be a complete RFC 8620 request envelope, including `using` and
`methodCalls`. The response can contain private account data, so inspect it
before attaching it to a public bug report.
