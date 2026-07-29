# Message Rendering

HTML message bodies are hostile input. `HtmlMessageDocumentBuilder` creates a derived document
that removes sender scripts and event handlers, rewrites inline resources, reserves application
attributes, and applies the message content-security policy. Canonical message content is never
modified for presentation.

`HtmlMessageView` owns document-side application behaviour. It restores explicitly permitted
remote content, applies translations, and adapts message colours without reloading or modifying
the canonical body.

## Dark appearance

HTML message colours can follow the application, remain original, or always use a dark
appearance. Dark rendering uses the pinned Dark Reader API bundle in
`res/vendor/darkreader/darkreader.js`.

The mail configuration deliberately differs from a browser extension:

- Dark Reader executes in Qt WebEngine's isolated application world.
- Its stylesheet and custom-element proxies are disabled because sender JavaScript is removed.
- Image analysis is disabled so message images and logos retain their pixels.
- No Dark Reader fetch method is installed. It cannot bypass the remote-content policy.
- A temporary static background is included in the derived view document to prevent a white
  flash, then removed after the dynamic theme is active.
- Sender-provided Dark Reader markers are cleared before the engine starts.

The context menu can temporarily switch the current message between its original colours and the
dark appearance. This override is discarded when another message is loaded.

The vendored bundle is updated with `scripts/update-darkreader.sh`; its source revision and
checksum are recorded in `res/vendor/darkreader/VERSION`.
