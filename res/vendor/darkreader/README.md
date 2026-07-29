# Dark Reader API bundle

`darkreader.js` is the generated UMD API bundle from Dark Reader 4.9.129, commit
`6fc3e2c40648a48af0f43c819e60926d13b9dd21`. The corresponding MIT licence is in
`LICENSE`.

To update it:

1. Update the source checkout under `references/darkreader`.
2. Apply `mail-profile.patch` to that checkout.
3. Run `npm ci` and `npm run api` there.
4. Copy the generated `darkreader.js` and `LICENSE` into this directory.

The mail-profile patch prevents an inline proxy script from being inserted when both proxy
features are disabled. Only the generated runtime and licence ship with Javelin Mail; the Dark
Reader build dependencies do not.
