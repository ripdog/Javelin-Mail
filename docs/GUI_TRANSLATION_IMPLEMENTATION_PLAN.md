# GUI-owned translation implementation plan

## Implementation status

Implemented in the main checkout on 5 August 2026. The production architecture now follows this
document: translation settings, language detection, provider execution, model management, and the
dedicated cache are GUI-owned; protocol major version 4 and settings schema version 3 contain no
translation fields or actions; main-database migration 39 drops the former daemon cache.

One upstream format assumption changed during implementation. Mozilla's current
`translations-models-v2` production records publish `application/zstd` attachments rather than gzip.
The committed manifest generator accepts the declared Zstandard format, and the installer performs
bounded streaming zstd decompression with compressed and decompressed size/SHA-256 verification. The
security and atomic-install requirements below are unchanged.

Manual end-to-end provider checks and performance measurements remain release validation work; the
implementation and deterministic automated coverage are complete.

## Authority and intent

This document is the authoritative implementation plan for message translation in Javelin Mail. It
supersedes every statement in `DAEMON_GUI_ARCHITECTURE.md`, `DAEMON_GUI_IMPLEMENTATION_PLAN.md`, and
`ARCHITECTURE.md` that assigns translation execution, translation preferences, translation caching,
or language detection to the daemon or to the main mail cache.

Translation is a presentation feature. It exists only to render the message currently being viewed,
and there is no useful translation work to perform after the GUI exits. The complete translation
subsystem therefore belongs to the `javelin` GUI process.

The final design has three user-selectable modes:

- **Disabled**: no language detection, translation offer, translation cache lookup, model loading, or
  translation network request occurs.
- **Google Translate**: the existing Google implementation remains available and is the default.
  Message text is sent to Google only for an explicit Translate action or an existing persisted
  sender/domain auto-translate rule.
- **Local translation**: Firefox-compatible Bergamot models run natively inside the GUI process.
  Message text never leaves the machine. Required models are downloaded on demand or explicitly from
  Preferences and remain installed until the user removes them.

There is deliberately no translation daemon, helper executable, hidden WebEngine worker, or separate
process. Local inference runs asynchronously in the GUI process. Model memory remains allocated while
the GUI is open and the local provider is active; choosing Google or Disabled releases loaded local
models.

This is a production-first implementation programme. Do not create a prototype executable, benchmark
harness, early memory experiment, or temporary provider path before implementing the real subsystem.
Implement the complete production path first. Add tests and performance verification after the
feature works end to end.

## Non-negotiable decisions

The implementation worker must not reopen these decisions:

1. Translation execution, language detection, model management, settings, and caching are GUI-owned.
2. The daemon and IPC protocol contain no translation action or translation setting.
3. Translation results are cached in a dedicated GUI-owned SQLite database, never the main mail
   database.
4. Google Translate is retained and remains the default provider after migration.
5. Local translation uses Mozilla's native Bergamot/Marian inference source and Firefox-compatible
   model artifacts, not the prebuilt Firefox WASM bundle.
6. Local inference runs in the GUI process on a serial worker pool. There is no helper process.
7. The local provider loads models lazily and keeps at most two model directions resident, enough for
   a direct route or a two-leg English pivot.
8. Disabling translation also disables language detection.
9. Generic automatic translation offers may use only cached results and already-installed models.
   An explicit Translate action or a persisted sender/domain auto-translate rule may perform an
   external fetch: a Google request for the Google provider or a model download for the local
   provider.
10. The existing main-database translation cache is disposable. Do not migrate its rows.
11. The implementation is split into buildable production commits, followed by tests. Do not keep two
    permanent translation paths.

## Current implementation surface

The current code has the right presentation behaviour but the wrong process ownership:

- `MessageViewContainer` performs language detection in the GUI, collects plain-text or HTML text
  chunks, guards responses with the current message/request token, applies translated chunks, and
  restores the original content.
- `RemoteTranslationPort` mirrors translation settings from the daemon settings snapshot and sends
  `RemoteActionKind::TranslationTranslate` over IPC.
- `TranslationService` runs in `javelind`, calls Google's `translateHtml` endpoint, and reads/writes
  `TranslationCacheRepository` in the main mail database.
- `SettingsRepository`, `ProcessBoundary`, and `SocketTransport` carry translation settings across the
  daemon boundary.
- `FastTextLanguageDetector` and `LanguageDetectionService` are compiled into GUI and JMAP targets even
  though their only production consumer is message presentation.
- main database migration 10 creates `translation_cache`.

The final migration must preserve the existing visible behaviour while removing all daemon ownership.
In particular, retain:

- the Translate / Show original control;
- sender and domain auto-translate rules;
- target-language selection;
- cached automatic translations that avoid external work;
- stale-response protection when the user navigates during translation;
- plain-text linkification after translation;
- HTML text-node extraction and reapplication; and
- the built-in Google credential plus optional API-key override.

## Final architecture

```text
javelin GUI process
│
├── MessageViewContainer
│   ├── requests GUI-local language detection
│   ├── collects TranslationChunks
│   ├── requests translation with ExternalFetchPolicy
│   └── applies/restores translated presentation
│
├── gui::translation::TranslationService
│   ├── owns current TranslationSettings
│   ├── performs cache lookup/merge/write
│   ├── selects Google or Bergamot backend
│   ├── resolves direct or English-pivot local routes
│   └── exposes model-download status to the UI
│
├── TranslationSettingsStore            QSettings, translation group only
├── TranslationCache                    dedicated GUI SQLite database
├── GoogleTranslationBackend            QNetworkAccessManager + QCoro
├── BergamotTranslationBackend          native C++, serial worker pool
├── TranslationModelManifest            committed immutable manifest
├── TranslationModelStore               download, verify, install, remove
└── LanguageDetectionService             fastText, GUI worker pool

javelind daemon process
└── no translation settings, service, cache repository, action, model, or detector
```

The main SQLite mail cache remains read-only from the GUI. The dedicated translation database is a
separate rebuildable presentation cache and is not governed by the daemon/main-cache writer rule.

## Production source layout

Create the following GUI-owned subsystem. Use namespace `javelin::gui::translation` throughout.

```text
src/gui/translation/
    TranslationTypes.h
    TranslationSettingsPolicy.cpp
    TranslationSettingsStore.h
    TranslationSettingsStore.cpp
    TranslationCache.h
    TranslationCache.cpp
    TranslationBackend.h
    TranslationService.h
    TranslationService.cpp
    GoogleTranslationBackend.h
    GoogleTranslationBackend.cpp
    BergamotTranslationBackend.h
    BergamotTranslationBackend.cpp
    TranslationModelManifest.h
    TranslationModelManifest.cpp
    TranslationModelStore.h
    TranslationModelStore.cpp
    LanguageDetection.h
    LanguageDetection.cpp
    FastTextLanguageDetector.h
    FastTextLanguageDetector.cpp
```

Add the maintainer-owned manifest generator and committed resources:

```text
tools/update_translation_model_manifest.py
res/models/translations/manifest-v1.json
res/models/translations/README.md
res/models/translations/LICENSES/
```

Remove these obsolete application/daemon files after the GUI cutover:

```text
src/app/TranslationApplicationPorts.h
src/app/TranslationService.h
src/app/TranslationService.cpp
src/app/TranslationSettings.cpp
src/app/TranslationSettingsPolicy.cpp
src/jmap/cache/TranslationCacheRepository.h
src/jmap/cache/TranslationCacheRepository.cpp
src/jmap/language/LanguageDetection.h
src/jmap/language/LanguageDetection.cpp
src/jmap/language/FastTextLanguageDetector.h
src/jmap/language/FastTextLanguageDetector.cpp
```

Do not leave forwarding headers or compatibility aliases. Update includes and namespaces directly.

## Core value types and contracts

Define the following production concepts in `TranslationTypes.h`. Names may follow the exact spelling
below; do not retain the old `enabled` boolean as a second source of truth.

```cpp
enum class TranslationProvider
{
    Disabled,
    Google,
    Local,
};

struct TranslationSettings
{
    TranslationProvider provider = TranslationProvider::Google;
    QString apiKeyOverride;
    QString targetLanguage = QStringLiteral("en");
    QStringList autoTranslateSenders;
    QStringList autoTranslateDomains;

    bool operator==(const TranslationSettings&) const = default;
};

enum class ExternalFetchPolicy
{
    InstalledAndCachedOnly,
    AllowExternalFetch,
};

using TranslationChunks = QVector<QStringList>;

struct TranslationUnavailable
{
    enum class Reason
    {
        Disabled,
        SourceLanguageUnknown,
        UnsupportedLanguageRoute,
        RequiredModelNotInstalled,
    };

    Reason reason;
};

enum class TranslationErrorCode
{
    CacheOpenFailed,
    CacheReadFailed,
    CacheWriteFailed,
    GoogleRequestFailed,
    GoogleResponseInvalid,
    ManifestInvalid,
    ModelDownloadFailed,
    ModelVerificationFailed,
    ModelLoadFailed,
    InferenceFailed,
};

struct TranslationError
{
    TranslationErrorCode code;
    QString message;
};

using TranslationResult =
    std::variant<TranslationChunks, TranslationUnavailable, TranslationError>;
```

`TranslationBackend` is provider-neutral and translates a flat, deduplicated list of non-empty text
items. It does not read settings or the cache.

```cpp
struct BackendRequest
{
    QString sourceLanguage;
    QString targetLanguage;
    QVector<QString> texts;
    ExternalFetchPolicy fetchPolicy;
};

struct BackendTranslation
{
    QVector<QString> texts;
    QString backendRevision;
};

using BackendResult =
    std::variant<BackendTranslation, TranslationUnavailable, TranslationError>;

class TranslationBackend
{
  public:
    virtual ~TranslationBackend() = default;
    [[nodiscard]] virtual QCoro::Task<BackendResult> translate(BackendRequest request) = 0;
};
```

The backend revision is part of the cache identity:

- Google uses a source constant, initially `google-translate-html-v1`. Bump it only when request or
  response semantics intentionally change.
- A direct Bergamot route uses
  `bergamot-v0.6.0:<manifest-revision>:<source>-<target>:<model-sha256>`.
- An English-pivot route includes both leg revisions in order.

## GUI-local settings ownership

### Storage

`TranslationSettingsStore` directly owns the `translation` QSettings group in the GUI process. It
must not use `GuiSettings`, `SettingsPort`, `SettingsSnapshot`, or daemon IPC.

Persist these keys:

```text
translation/provider                 disabled | google | local
translation/apiKeyOverride           QString
translation/targetLanguage           canonical language tag
translation/autoTranslateSenders     QStringList
translation/autoTranslateDomains     QStringList
translation/schemaVersion            1
```

### One-time live migration

Existing users already have `translation/enabled` and the other four settings. On the first read when
`translation/provider` is absent:

1. Read and normalize the old settings.
2. Map `enabled=false` to `provider=disabled`.
3. Map `enabled=true` or a missing key to `provider=google`.
4. Preserve API-key override, target language, sender rules, and domain rules.
5. Write the complete schema-version-1 shape and sync it.
6. Verify the new shape can be read back.
7. Remove `translation/enabled`.

This is the only accepted dual-key migration. Do not continue reading `enabled` after the migration
succeeds.

Normalize sender/domain lists exactly as the existing policy does: trim, lowercase where currently
expected, remove empty values and duplicates, and sort case-insensitively. Normalize language tags
through one shared language-tag helper.

### QSettings architecture guard

The existing CMake check rejects every production GUI source containing `QSettings`. Change it to
allow exactly `src/gui/translation/TranslationSettingsStore.cpp`, and continue rejecting `QSettings`
in every other production GUI source. Update the error text and architecture documentation to say
that daemon-owned settings remain canonical for operational/shared application state, while the
translation group is a GUI-owned presentation setting.

## Language tags and provider mappings

Use canonical internal tags throughout settings, cache keys, detection comparison, and UI state.
Canonicalize at least:

```text
zh-CN, zh-cn, zh          -> zh-Hans
zh-TW, zh-tw, zh_hant     -> zh-Hant
language_REGION           -> language-REGION
primary language          -> lowercase
script                    -> title case
region                    -> uppercase
```

Provider adapters perform their own mapping:

```text
internal zh-Hans -> Google zh-CN -> Mozilla zh
internal zh-Hant -> Google zh-TW -> Mozilla zh_hant
```

Do not leak Mozilla underscore identifiers or Google-specific aliases into settings or cache API
calls.

The Google target-language combo remains editable and keeps the existing curated entries. The local
provider target-language combo is non-editable and contains only target languages reachable through
the committed manifest. When switching to Local with an unsupported stored target, select English.
When switching back to Google, retain the current canonical target.

## Dedicated translation SQLite cache

### Location and ownership

Create the cache at:

```text
QStandardPaths::CacheLocation/translations/cache-v1.sqlite3
```

Create parent directories on demand. This file is opened only by the GUI process. It is unrelated to
the daemon cache instance ID, main-cache barrier, main database migrations, mail-vault lifecycle, or
cache invalidation IPC.

`TranslationCache` owns a unique `QSqlDatabase` connection created and destroyed on the GUI thread.
Do not reuse `jmap::cache::DatabaseConnection`, because that always runs the main mail schema and
would reintroduce the wrong dependency boundary.

Apply:

```text
PRAGMA journal_mode = WAL
PRAGMA synchronous = NORMAL
PRAGMA busy_timeout = 5000
PRAGMA foreign_keys = ON
```

### Schema

Use a private schema migration table and schema version 1:

```sql
CREATE TABLE schema_migrations (
    version INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    applied_at INTEGER NOT NULL
) STRICT;

CREATE TABLE translations (
    provider TEXT NOT NULL,
    source_language TEXT NOT NULL,
    target_language TEXT NOT NULL,
    backend_revision TEXT NOT NULL,
    input_hash TEXT NOT NULL,
    input_text TEXT NOT NULL,
    translated_text TEXT NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (
        provider,
        source_language,
        target_language,
        backend_revision,
        input_hash
    )
) STRICT;

CREATE INDEX translations_updated_at
ON translations(updated_at ASC);
```

Use SHA-256 of UTF-8 input text for `input_hash` and also compare `input_text` on lookup. A hash match
with different text is a cache miss.

### Cache behaviour

`TranslationService` performs cache lookup before invoking a backend. For one request:

1. Flatten chunk strings while recording their original chunk/text positions.
2. Preserve empty strings without sending them to a backend.
3. Deduplicate identical non-empty source strings.
4. Read all unique strings from the cache.
5. Invoke the selected backend only for misses.
6. Merge cached and new results back into exact original topology.
7. Upsert successful new translations in one transaction.

A cache read error is a visible translation error; do not silently send text externally because the
privacy/automatic-fetch decision may have relied on the cache. A cache write error is logged and
returned only after the translated content has been made available to the caller; display the
translation and show a non-blocking diagnostic rather than discarding successful work.

Bound the cache to 100,000 rows. Prune on open and after each 500 successful inserted rows. Delete the
oldest rows above the limit using `updated_at`. Do not add size-based vacuuming to the translation
path.

### Main-database cleanup

After the GUI cache is in production use:

- append a new main database migration named `drop_daemon_translation_cache` containing
  `DROP TABLE IF EXISTS translation_cache`;
- remove `TranslationCacheRepository` and its tests;
- remove `translation_cache` from main database tests and any generic writer-contention fixtures;
- do not rewrite or renumber historical migration 10; and
- do not copy old rows into the GUI cache.

## GUI-local service composition

`GuiServices` owns these objects in dependency order:

1. one GUI `QNetworkAccessManager`;
2. `TranslationSettingsStore`;
3. `TranslationCache`;
4. `TranslationModelManifest`;
5. `TranslationModelStore`;
6. `GoogleTranslationBackend`;
7. `BergamotTranslationBackend` when compiled in;
8. `LanguageDetectionService`; and
9. `TranslationService`.

Replace `std::unique_ptr<RemoteTranslationPort>` with
`std::unique_ptr<javelin::gui::translation::TranslationService>`. `GuiServices::translationService()`
returns the concrete GUI-local service. Update `MainWindow` and `MessageViewContainer` constructors to
accept that service.

The service owns settings state and reloads it after Preferences saves. It provides:

```cpp
[[nodiscard]] const TranslationSettings& settings() const;
[[nodiscard]] bool isEnabled() const;
[[nodiscard]] QString targetLanguage() const;
[[nodiscard]] bool shouldAutoTranslate(QStringView sender, QStringView domain) const;
void setAutoTranslateSender(QString sender, bool enabled);
void setAutoTranslateDomain(QString domain, bool enabled);
[[nodiscard]] QFuture<std::optional<LanguageDetectionResult>>
detectLanguage(QString text);
[[nodiscard]] QCoro::Task<TranslationResult>
translate(TranslationChunks chunks, QString sourceLanguage,
          ExternalFetchPolicy fetchPolicy);
void reloadSettings();
void releaseLocalModels();
```

`setAutoTranslateSender` and `setAutoTranslateDomain` persist through
`TranslationSettingsStore` immediately, preserving the current message-view menu behaviour. A failed
settings write restores the in-memory previous value and returns a visible error to the view.

When settings change:

- switching to Disabled clears current translation UI state, stops future detection, and releases
  local models;
- switching from Local to Google releases local models;
- changing target language invalidates the current translation presentation and reruns offer policy;
- changing API key affects only future Google requests; and
- changing provider does not delete cached results or downloaded models.

## Message-view integration

Keep the current extraction/application code and stale-request token. Change only service ownership
and request semantics.

### Detection

Move `LanguageDetectionResult`, `shouldOfferTranslation`, `LanguageDetectionService`, and
`FastTextLanguageDetector` to the GUI translation namespace and source directory.

`MessageViewContainer` must no longer construct a detector per message. It sends the prepared message
text to `TranslationService::detectLanguage()`. The service owns a lazy fastText detector on a
single-worker `QThreadPool`, so the model is loaded once per GUI process. Disabled mode returns a
completed `std::nullopt` result without starting the worker or loading fastText.

Retain the existing minimum-text and confidence policy. For Local mode, also require that the
manifest contains a direct or English-pivot route from the detected source to the selected target.
Unsupported local routes do not show an actionable Translate button.

### Fetch policy

Replace the ambiguous boolean `allowNetwork` with `ExternalFetchPolicy`:

- ordinary language detection / automatic cache restore:
  `InstalledAndCachedOnly`;
- explicit Translate button:
  `AllowExternalFetch`;
- persisted sender/domain auto-translate rule:
  `AllowExternalFetch`.

For Google, `InstalledAndCachedOnly` means cache only. For Local, it means cache plus already-installed
models; local inference is allowed because it performs no external fetch.

### Status and errors

While a local model downloads, the message banner shows:

```text
Downloading Japanese → English translation model… 42%
```

Use indeterminate progress when total bytes are unknown. Once installation completes, translation
continues automatically. Navigation invalidates only presentation application; the model download may
finish and install because it is useful for later requests.

Map failures to concise UI text:

- unknown source: `Could not determine the message language.`
- unsupported route: `Local translation is not available for <source> → <target>.`
- model download/verification/load failure: state the failing language direction and error;
- Google errors: retain the current provider/network error detail; and
- disabled: hide translation controls rather than showing an error.

Do not expose file paths, hashes, or Bergamot internals in normal UI errors. Log those details.

## Google backend

Move the current Google-specific implementation into `GoogleTranslationBackend` without changing its
wire contract during the ownership migration:

- endpoint: `https://translate-pa.googleapis.com/v1/translateHtml`;
- built-in API key behaviour;
- optional API-key override;
- 30-second transfer timeout;
- current 800-character request batching;
- current HTML escaping/group marker transformation; and
- current response validation.

This first move must be behavioural, not a Google API rewrite. Later cleanup is permitted only after
both providers work and tests exist.

`GoogleTranslationBackend` receives the current API key through its request/configuration and has no
access to QSettings or SQLite.

## Mozilla/Bergamot source integration

### Upstream pin

Use Mozilla's maintained `mozilla/translations` repository:

```text
repository: https://github.com/mozilla/translations.git
commit:     4732dc947bc952abb019aabfe5582006d4fc3337
Bergamot:   v0.6.0
license:    MPL-2.0
```

Pin the exact commit in `cmake/Dependencies.cmake`. Do not follow `main` at configure time. Fetch
recursive submodules because the native inference tree requires Marian, SentencePiece, intgemm,
simd helpers, and `ssplit-cpp`.

Configure the upstream `inference` subdirectory with:

```text
COMPILE_WASM=OFF
USE_WASM_COMPATIBLE_SOURCE=OFF
COMPILE_TESTS=OFF
COMPILE_UNIT_TESTS=OFF
COMPILE_PYTHON=OFF
USE_STATIC_LIBS=ON
BUILD_ARCH=x86-64
```

Never use upstream's `BUILD_ARCH=native` default in release packages. Set a portable x86-64 baseline
and rely on the engine's supported runtime paths. Initially expose local translation only on x86-64.
Google and Disabled remain available everywhere.

Add:

```cmake
option(JAVELIN_ENABLE_BERGAMOT_TRANSLATION
       "Build native local message translation" <ON only for x86-64>)
```

When disabled, do not compile Bergamot, do not show Local in Preferences, and do not fetch Mozilla
sources. Do not create a runtime stub pretending Local is available.

Link the upstream `bergamot-translator-source` target into a new static `javelin_translation` target.
Do not link Bergamot into `javelind`, `javelin_protocol`, `javelin_jmap`, or `javelin_cache_read`.

Raise Javelin's CMake minimum to 3.24 so the pinned upstream native CMake and recursive FetchContent
configuration have one supported baseline.

### Translation target

Create `javelin_translation` with the complete GUI-local translation subsystem. Its direct public
surface is `TranslationTypes.h`, `TranslationService.h`, and language-detection result types. Link:

```text
Qt::Core
Qt::Concurrent
Qt::Network
Qt::Sql
QCoro6::Core
QCoro6::Network
fasttext_javelin
bergamot-translator-source   only when enabled
ZLIB::ZLIB                   only when enabled
```

`javelin_gui` may link `javelin_translation`; the `javelin` executable obtains it transitively. Remove
all direct fastText linkage and language-model compile definitions from `javelin_jmap` and the
`javelin` executable.

Install the fastText language model because both Google and Local modes use it. Its ownership changes
to the GUI translation target, but its package path may remain
`share/javelinmail/models/fasttext/lid.176.ftz`.

## Committed Firefox model manifest

Do not query Firefox Remote Settings at application runtime. Runtime behaviour must be reproducible
and compatible with the pinned engine.

`tools/update_translation_model_manifest.py` is a maintainer tool that reads Mozilla's
`translations-models-v2` collection:

```text
https://firefox.settings.services.mozilla.com/v1/buckets/main/collections/
translations-models-v2/records
```

It emits `manifest-v1.json` from records compatible with engine major version 4 / model major version
3. Select, for each language direction:

1. the highest stable numeric model version;
2. `base-memory` architecture when complete;
3. otherwise `base` when complete;
4. never `tiny` for production; and
5. only a complete set of `model`, `lex`, and either shared `vocab` or both `srcvocab` and `trgvocab`.

The generated manifest records:

```json
{
  "schemaVersion": 1,
  "manifestRevision": "mozilla-remote-settings-<collection-last-modified>",
  "engine": {
    "name": "bergamot",
    "version": "v0.6.0",
    "sourceCommit": "4732dc947bc952abb019aabfe5582006d4fc3337"
  },
  "directions": [
    {
      "source": "ja",
      "target": "en",
      "mozillaSource": "ja",
      "mozillaTarget": "en",
      "modelVersion": "3.1",
      "architecture": "base-memory",
      "files": [
        {
          "type": "model",
          "url": "https://firefox-settings-attachments.cdn.mozilla.net/...",
          "compressedSize": 31973916,
          "compressedSha256": "...",
          "decompressedSize": 43977787,
          "decompressedSha256": "...",
          "installedName": "model.bin"
        }
      ],
      "licenseFiles": ["MPL-2.0.txt"]
    }
  ]
}
```

Use the record's attachment hash/size and decompressed hash/size. Sort directions and files
stably so regeneration produces reviewable diffs. Validate duplicate directions, incomplete file
sets, unknown file types, invalid hashes, and non-HTTPS URLs before writing.

Commit the generated manifest. Updating it is an explicit source change reviewed alongside engine
compatibility; it is not an automatic application update.

## Model storage and download

### Location

Store installed local models under:

```text
QStandardPaths::AppLocalDataLocation/translations/models/v1/
    <source>-<target>/<model-version>-<architecture>/
        model.bin
        lex.bin
        vocab.spm
        # or srcvocab.spm + trgvocab.spm
        installed.json
```

Models are user-installed application data, not disposable cache data. Do not put them inside the
translation SQLite directory or main mail cache.

### Installation algorithm

`TranslationModelStore` performs every install as follows:

1. Resolve the exact manifest direction and destination.
2. Reject a URL not present in the committed manifest.
3. Create a temporary installation directory beside the final directory.
4. Download each attachment with `QNetworkAccessManager` and QCoro into `*.part` files.
5. Enforce the manifest compressed-size limit while streaming.
6. Verify compressed SHA-256 and size.
7. Decompress Zstandard with libzstd into a temporary output file while enforcing the
   decompressed-size limit.
8. Verify decompressed SHA-256 and size.
9. Write `installed.json` containing manifest revision, direction, model version, architecture, file
   hashes, and installation time.
10. `fsync`/close files and atomically rename the complete directory into place.
11. Remove the prior installation for that direction only after the new directory is complete.

A cancellation or failure removes the temporary directory and leaves any previous installed model
usable. Startup removes stale temporary installation directories.

Before loading a model, validate `installed.json`, every required filename, size, and SHA-256. A
corrupt installation is not loaded; report it and offer redownload.

Model downloads may continue after message navigation, but GUI shutdown cancels network transfers and
removes incomplete temporary files on the next start.

### Route selection

`TranslationModelManifest::route(source, target)` returns:

- no legs when source equals target;
- one direct leg when available;
- otherwise exactly two legs `source -> en` and `en -> target` when both exist; or
- unsupported.

Never pivot through anything other than English and never use more than two legs.

`InstalledAndCachedOnly` succeeds only when every required leg is installed. `AllowExternalFetch`
installs all missing legs in route order, then translates.

## Native Bergamot backend

Use `marian::bergamot::BlockingService` because Javelin already provides job-level asynchrony. Run all
Bergamot construction, model loading, and inference on a private `QThreadPool` with
`maxThreadCount(1)`. The GUI thread must never call Bergamot directly.

The worker owns:

- one `BlockingService`;
- an LRU map of at most two `TranslationModel` instances; and
- all native model/config objects.

A direct route loads one model. A pivot route loads both legs and calls `pivotMultiple`. Evict the
least-recently-used model only when loading a third direction. `releaseLocalModels()` schedules a
worker action that clears all model instances after current inference completes.

Generate one Marian/Bergamot config centrally for each manifest direction. Use:

```yaml
beam-size: 1
normalize: 1.0
word-penalty: 0
max-length-break: 128
mini-batch-words: 1024
workspace: 128
max-length-factor: 2.0
skip-cost: true
cpu-threads: 0
quiet: true
quiet-translation: true
gemm-precision: int8shiftAlphaAll
```

Add the exact installed model, vocabulary, shortlist, and sentence-split prefix paths. Bundle the
required nonbreaking-prefix files from upstream `ssplit-cpp` with the application and select the
source-language file, falling back to the English prefix file only where Bergamot upstream does so.
Do not invent a model config in multiple call sites.

Translate the flattened vector in one `translateMultiple` call per direct leg. Preserve item order.
For a pivot route, use `pivotMultiple` so response composition follows Bergamot's own mapping logic.
Disable quality scores and alignments; Javelin only needs translated text.

Catch C++ exceptions at the worker boundary and convert them into `TranslationError`. No Bergamot
exception may unwind into Qt event handling.

At GUI shutdown, clear queued translation work and wait for the active worker task to complete before
destroying model objects. Blocking at final process teardown is acceptable; blocking during normal
GUI interaction is not.

## Preferences design

Replace the Translation page checkbox with a provider combo labelled `Translation provider`:

```text
Disabled
Google Translate
Local (Firefox models)       only when compiled in
```

Google remains selected for migrated enabled installations and new installations.

Common controls:

- Target language
- Auto-Translate Entries list
- Remove selected auto-translate entry

Google panel:

- copy explaining that translated message text is sent to Google;
- API-key override field; and
- existing built-in-key placeholder.

Local panel:

- copy explaining that translation runs on-device and models are downloaded from Mozilla-hosted
  artifacts;
- required route for the selected target/source when one is known;
- installed-model list showing direction, model version, architecture, and disk size;
- `Download models…` button;
- `Remove selected models` button; and
- current download/install progress.

Disabled mode disables/hides target, provider-specific, and auto-translate controls. It must make
clear that language detection is also disabled.

The model list is populated from `TranslationModelStore`, not by scanning arbitrary files in the UI.
Removing a model first calls `releaseLocalModels()` for affected directions, then deletes the
validated installed directory. Never recursively delete a path not derived from the model root plus a
manifest direction.

### Saving mixed settings

`PreferencesDialog` currently saves one daemon settings update. Split translation out:

1. Build and validate daemon/shared settings without any translation field.
2. Submit the daemon settings update.
3. If it succeeds, save normalized GUI translation settings through `TranslationSettingsStore`.
4. If local saving fails, keep the dialog open, show the error, and update the base daemon revision so
   retrying does not submit a stale shared-settings revision.
5. On success, call `TranslationService::reloadSettings()` and notify the current message view.

Do not write local translation settings into `GuiSettings` merely to preserve one dialog transaction.
These stores have intentionally different owners.

## IPC and daemon removal

Perform the GUI Google cutover and daemon removal in the same production phase. Delete:

- `RemoteActionKind::TranslationTranslate`;
- protocol `TranslationSettings`;
- `SettingsUpdate::translation`;
- `SettingsSnapshot::translation`;
- all translation validation and size accounting in `ProcessBoundary.cpp`;
- all translation encoding/decoding in `SocketTransport.cpp`;
- `RemoteTranslationPort` declaration and implementation;
- translation dispatch in `DaemonRemoteActionDispatcher`;
- translation service/cache members and accessors in `DaemonServices`;
- translation settings application in `DaemonProcess`;
- translation constants/read/normalize/write logic in `SettingsRepository`; and
- daemon translation sources and test dependencies in CMake.

Removing fields changes the framed settings snapshot and action enumeration. Bump:

```text
ProtocolVersion 3.0 -> 4.0
SettingsSnapshot schemaVersion 2 -> 3
```

Update protocol conformance, boundary validation, socket round-trip, daemon-process, and settings
repository fixtures. Both executables ship together; do not implement compatibility decoding for
protocol version 3.

After removal, `rg` for `TranslationTranslate`, `RemoteTranslationPort`, and
`translationService()` must return no production matches.

## CMake and packaging

### Build graph

The final graph is:

```text
javelind
└── javelin_daemon_core
    └── no translation dependencies

javelin
├── javelin_gui
│   └── javelin_translation
│       ├── Qt Network/Sql/Concurrent
│       ├── QCoro Network
│       ├── fastText
│       └── optional native Bergamot
└── read-only main-cache targets
```

Update GUI boundary checks to permit the dedicated translation target while continuing to forbid
`javelin_jmap`, daemon services, main write-capable cache APIs, and JMAP transport.

### Arch package

Ensure the package fetches or builds the pinned Mozilla source recursively, installs MPL and model
notices, installs sentence-split prefix resources, and retains the fastText model. Models themselves
are not bundled.

### Flatpak

Add a pinned `type: git` source for `mozilla/translations` at the audited commit with submodules
enabled, place it under `third-party/mozilla-translations`, and pass:

```text
-DFETCHCONTENT_SOURCE_DIR_MOZILLA_TRANSLATIONS=/run/build/javelin-mail/third-party/mozilla-translations
```

Retain network permission because JMAP, Google translation, and optional model downloads require it.
Model downloads go to the Flatpak persistent application-data directory through
`QStandardPaths::AppLocalDataLocation`.

### AppImage

Compile Bergamot into the GUI binary/static dependency closure and install required prefix/license
resources into the AppDir. Do not bundle model packs. Confirm AppImage runtime paths use the mounted
share directory for bundled prefix/fastText assets and writable XDG locations for model/cache data.

### Notices

Install:

- Mozilla translations MPL-2.0 source notice;
- notices required by native Bergamot/Marian dependencies;
- fastText/model notices already present; and
- model provenance/license files referenced by the committed manifest.

Do not expose or distribute fetched source archives as runtime assets.

## Ordered implementation programme

Every phase below implements production code. There is no prototype phase.

### Phase 1: cut Google translation over to the GUI and remove daemon ownership

Implement in one logical, buildable cutover:

1. Create GUI translation types, settings store, dedicated cache, Google backend, language detector,
   and translation service.
2. Wire them into `GuiServices`, `MainWindow`, and `MessageViewContainer`.
3. Preserve the current Google endpoint, batching, key, extraction, auto-rule, cache-only, and stale
   response behaviour.
4. Redesign Preferences storage plumbing enough to save translation locally while leaving visible
   controls otherwise unchanged.
5. Remove the remote translation port, daemon service, IPC action, settings snapshot field, main-cache
   repository, and daemon settings handling.
6. Bump protocol/settings schema versions.
7. Append the main database migration that drops `translation_cache`.
8. Update architecture guardrails and remove obsolete source files.
9. Configure and build with the Debug preset; fix compilation across both executables before ending
   the phase.

Acceptance:

- Google translation works identically with the daemon running or restarting.
- Translation works entirely within the GUI process.
- closing the GUI terminates all translation work while the daemon remains unaffected;
- the daemon starts and operates with no translation dependencies;
- `javelin` never writes the main mail database;
- translations are present in the dedicated cache database; and
- existing users retain enabled/disabled state, target, API key, and auto rules after migration.

Suggested commit:

```text
Move message translation ownership into GUI
```

### Phase 2: introduce provider-aware preferences and contracts

1. Replace `enabled` with `TranslationProvider` everywhere.
2. Add Disabled / Google / conditional Local provider UI.
3. Add canonical language-tag mapping and provider-specific mappings.
4. Replace `allowNetwork` with `ExternalFetchPolicy`.
5. Add provider/backend revision to cache identity.
6. Make Disabled suppress language detection and release local resources.
7. Add richer unavailable/error results and UI mapping.

At this point Local may be hidden because the build option/backend is not yet present; do not expose a
nonfunctional option.

Acceptance:

- Disabled performs no detection or translation work;
- Google remains default and fully functional;
- provider changes persist locally and never cross IPC; and
- cached Google results are isolated by backend revision.

Suggested commit:

```text
Add GUI-local translation provider settings
```

### Phase 3: integrate pinned native Bergamot and the model manifest

1. Add the pinned recursive FetchContent dependency and optional build flag.
2. Create `javelin_translation` and move fastText ownership into it.
3. Add libzstd and sentence-split runtime resources.
4. Implement the manifest generator and commit `manifest-v1.json`.
5. Implement typed manifest parsing, validation, language catalog, and route resolution.
6. Update Arch, Flatpak, AppImage, and notice packaging.
7. Build the real `javelin` executable with Bergamot linked; do not create a separate sample binary.

Acceptance:

- a clean Debug build produces the normal GUI with native Bergamot linked on x86-64;
- Local remains hidden when the build option is off;
- manifest parsing succeeds from installed resources; and
- packaged builds do not use `-march=native` or download source during an offline Flatpak build.

Suggested commit:

```text
Integrate native Firefox translation engine
```

### Phase 4: implement production model installation and local inference

1. Implement model store paths, installed metadata, validation, download, Zstandard decompression,
   verification, atomic installation, replacement, and removal.
2. Implement direct and English-pivot route installation.
3. Implement serial worker-owned BlockingService/model LRU.
4. Implement direct `translateMultiple` and pivot `pivotMultiple` paths.
5. Flatten/deduplicate/rebuild chunks through TranslationService.
6. Continue explicit/auto-rule translation after an on-demand model download.
7. Emit progress and actionable errors to MessageViewContainer.
8. Release loaded models when provider changes away from Local or translation becomes Disabled.

Acceptance:

- selecting Local and explicitly translating a supported uncached message downloads verified models,
  translates, caches, and renders it;
- a second translation uses installed models without network access;
- ordinary automatic offers never download models;
- an auto-translate sender/domain rule may download its required model once;
- direct and English-pivot routes both work;
- corrupt or partial model files are rejected and recoverable; and
- Google continues to work independently.

Suggested commit:

```text
Implement local Bergamot message translation
```

### Phase 5: complete Preferences model management and UX

1. Implement installed-model list, disk sizes, route display, progress, explicit pre-download, and
   removal.
2. Finish provider-specific explanatory copy and control visibility.
3. Handle model deletion while loaded by releasing affected native models first.
4. Make current-message controls refresh immediately after provider/target changes.
5. Ensure local unsupported languages do not present a dead Translate action.
6. Update all user-facing translations/TS extraction as needed.

Acceptance:

- users can understand the privacy/resource trade-off and choose a provider;
- users can preinstall and remove model directions without opening a matching message;
- no UI control offers an operation the selected provider cannot perform; and
- settings/model failures keep Preferences open with actionable errors.

Suggested commit:

```text
Add local translation model management UI
```

### Phase 6: cleanup and documentation

1. Remove every obsolete daemon/IPC/main-cache translation reference and test fixture.
2. Update `ARCHITECTURE.md` to describe GUI ownership and the dedicated cache.
3. Update `DAEMON_GUI_ARCHITECTURE.md` and `DAEMON_GUI_IMPLEMENTATION_PLAN.md` with the explicit
   presentation-only translation exception.
4. Update `DEVELOPMENT.md` with the Bergamot build option, pinned dependency, manifest update command,
   packaging resources, and model storage paths.
5. Update README feature copy to distinguish cloud and private local translation without making
   unsupported quality claims.
6. Run `git diff --check` and architecture/dependency checks.

Acceptance:

- repository documentation no longer claims translation is daemon-owned;
- no translation source remains under `src/jmap` or daemon-only application services;
- the GUI's only writable SQLite surface is the explicitly named dedicated translation cache; and
- all package formats include code/notices/resources but no language model packs.

Suggested commit:

```text
Document GUI-owned translation architecture
```

### Phase 7: add tests after the production path works

Do not start this phase until phases 1–6 are functionally working.

Add deterministic tests for:

- old `enabled` settings migration to provider enum;
- settings normalization and persistence;
- dedicated cache schema, lookup, collision guard, provider/revision isolation, partial hits,
  transaction upsert, and pruning;
- Google request batching/response parsing through a scripted network manager;
- canonical language tags and provider mappings;
- manifest parsing, stable route selection, incomplete-direction rejection, and English pivot;
- model download size/hash/decompressed-hash checks and atomic replacement using local fixtures;
- local backend config generation and chunk topology;
- disabled mode suppressing detection;
- automatic fetch-policy semantics for Google and Local;
- stale response ignored after navigation;
- protocol/settings round trips after translation removal;
- main database migration dropping the old table; and
- Preferences provider/model-control state.

Move existing language detection tests into `tests/gui/translation`. Remove daemon
`TranslationServiceTest` and main `TranslationCacheRepositoryTest`; replace their useful behavioural
coverage with GUI subsystem tests.

The native inference test uses one small committed or test-downloaded fixture only if its license and
repository size are acceptable. Do not make the ordinary test suite depend on Mozilla network access.
If no appropriately small model can be committed, test the backend wrapper with a fake native-engine
adapter and reserve real-model verification for the release check below.

Run the full Debug test suite only after these tests are implemented.

Suggested commit:

```text
Test GUI translation providers and model lifecycle
```

### Phase 8: release verification

After implementation and deterministic tests pass:

1. Run a clean Debug configure/build/test using the required XDG runtime directory.
2. Build Arch, Flatpak, and AppImage artifacts.
3. Manually verify Google and Local translation for plain text and HTML.
4. Verify at least Japanese -> English direct translation and one non-English -> non-English English
   pivot.
5. Verify explicit download progress, offline reuse, model removal, corrupt-install recovery, and
   Disabled mode.
6. Verify daemon notifications/synchronization continue while the GUI is closed and have no Bergamot
   or fastText mappings in daemon memory.
7. Measure cold local model load, warm translation time, peak GUI RSS, and model disk use now that the
   production implementation exists. Record results in the release report; do not redesign solely to
   meet an arbitrary preimplementation estimate.
8. Run `ldd`, Flatpak inspection, and AppImage extraction checks to ensure all native dependencies and
   notices are present.

No separate benchmark executable is required.

## Final completion definition

The work is complete when all of the following are true:

- `javelind` contains no translation service, setting, action, cache, detector, model, or provider
  dependency;
- translation preferences are GUI-owned and absent from the IPC settings snapshot;
- Google translation remains available and is the default;
- Disabled suppresses both translation and language detection;
- Local uses pinned native Bergamot in the GUI process with Firefox-compatible verified model packs;
- local direct and English-pivot translation work without a helper process;
- translation results are isolated in the dedicated GUI SQLite cache by provider and backend/model
  revision;
- the GUI never writes the main mail database;
- models can be downloaded, validated, listed, reused offline, replaced, and removed;
- navigating during translation never applies text to the wrong message;
- Arch, Flatpak, and AppImage builds include the engine/resources/notices and exclude model packs;
- deterministic tests pass after the production implementation is complete; and
- all architecture and development documentation reflects the final ownership boundary.
