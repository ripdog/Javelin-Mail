# Local translation model manifest

`manifest-v1.json` is a maintainer-generated, reviewable snapshot of Firefox-compatible translation
model metadata. Javelin does not query Firefox Remote Settings at runtime and does not bundle model
packs.

When the local provider is selected, the GUI downloads only the model files named by this manifest,
verifies their compressed and decompressed sizes and SHA-256 hashes, and installs them below the
user's application-data directory. The native Bergamot engine and sentence-splitting resources are
part of the application package; downloaded language models remain user-managed application data.

Regenerate the manifest explicitly with:

```sh
python tools/update_translation_model_manifest.py \
    --output res/models/translations/manifest-v1.json
```

Review manifest changes together with the pinned Mozilla Translations source revision in
`cmake/Dependencies.cmake`. Model artifacts are distributed by Mozilla and carry the provenance and
licence metadata recorded in the manifest.
