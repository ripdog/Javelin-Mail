# Dark Reader API bundle

`darkreader.js` is the generated UMD API bundle recorded in `VERSION`. The corresponding MIT
licence is in `LICENSE`.

To update it:

1. Update the clean source checkout under `references/darkreader` to the desired revision.
2. Run `scripts/update-darkreader.sh` from anywhere in the repository.
3. Build and smoke-test an HTML message in both original and dark modes.

The mail-profile patch prevents an inline proxy script from being inserted when both proxy
features are disabled. The update script applies this patch temporarily, installs the exact
lockfile dependencies, builds the API, normalizes its line endings, copies the runtime and
licence, records the version, commit, and checksum, then restores the source checkout. Only the
generated runtime and licence ship with Javelin Mail; the Dark Reader build dependencies do not.
