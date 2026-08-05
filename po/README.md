# Javelin Mail translations

Javelin Mail uses KDE's KI18n framework with the translation domain `javelinmail`.
Following the KI18n/Gettext model, source-language strings passed to `i18n()`,
`i18nc()`, and `i18np()` are the message identifiers. The extracted template is
`javelinmail.pot`; it is not itself a translation.

Regenerate the template after changing user-visible text:

```sh
cmake --build --preset debug --target extract-translations
```

Future translations belong in locale subdirectories using KI18n's standard
layout, for example `po/de/javelinmail.po`. `ki18n_install(po)` compiles and
installs any such catalogs during the normal CMake install step.
