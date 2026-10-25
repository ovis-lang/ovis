# Editor work dropped (2026-10-24)

The owner dropped the VS Code extension and all editor work from
[proposals/manual-and-editor.md](../../proposals/manual-and-editor.md):
the language is English matched to syntax, so the smart manual is the help.
The text below is §3 of that proposal as it stood, kept for history.

## 3. The editor

### 3.1 A line is checked when it is done

Agreed: a phrase reads as a whole, so a half-written line is noise, and
the model is too slow for each keystroke. But a line isn't read alone: the
names set above it, the program's `to …:` phrases and the records decide
how it reads. So "checking a line" means checking the program again and
taking what changed.

- **When a line is done.** LSP has no such event, so the extension sends
  one: `ovis/lineDone` with the line, when the cursor leaves a line that
  was edited, or Enter is pressed at its end (VS Code's
  `onDidChangeTextEditorSelection`). Other editors still get a check on
  save, and after 2 s with no typing.
- **Two speeds.**
  1. *Without the model*, at once: the program is read again with the
     editor's text (rules, ranking shortcut, edits, as on save today).
     The line being edited keeps its old problems until it is done, so
     nothing flickers under the cursor. First measure how long this takes
     for the examples and a large package (V1); if over ~150 ms, keep
     each file's readings keyed by the line's text and what it sees, and
     read again only the lines whose key changed.
  2. *With the model*, in the background, for lines step 1 left unclear:
     through the daemon (`ovis model serve`, already started by builds),
     one question at a time, the line done last first, cancelled when
     the line changes again. Answers are kept by the lockfile's key, so
     the build that follows asks nothing new.
- **What the model's answer does in the editor.** The line is marked
  (a hint, not an error) "Ovis reads this as: …", with a quick fix to
  write the `@[meaning: …]` now; nothing is written by itself (decision 3).
  The build writes it anyway, as today. A line the model can't settle is
  an error, as now, with its fixes and a link to the manual.

### 3.2 Highlighting from the parser

The TextMate grammar stays for what never changes (sigils, comments,
texts, numbers, the built-in forms), so a file is coloured at once.
On top, `ovis lsp` gives **semantic tokens** from each line's reading:

| Token | Colour role | Example |
| --- | --- | --- |
| a phrase's word | function | `in capitals`, `add … to` |
| a name | variable | `scores`, `fan speed` |
| a field | property | `b's title` |
| a record or trait | type | `book`, `shape` |
| a `to …:` header's words | function declaration | `to greet (p):` |
| a sigil's own line | dimmed (as now) | `@[meaning: …]` |
| the unclear span | underlined (a problem) | |

The parser knows which tokens a template's words matched and which
filled its slots; it needs to keep that with each reading (a list of
word roles per line, V3). This also makes a name of several words
(`fan speed`) read as one, which no grammar can.

### 3.3 From a problem to the manual

- Each problem of kind `no_phrase` or `unclear` gets a code action "Look
  it up in the manual", which runs `ovis manual --line` and shows the
  entries in a side panel; each has "Use this" which puts its example,
  with the line's names, in place of the line (the author's own edit,
  checked when the line is done).
- **Hover on a phrase's word** shows its manual entry: pattern, `about`,
  the first example, and the package it's from. **Hover on a name, field
  or record** shows its entry (type, where set or defined, fields), and
  *go to definition* jumps to where it's defined or first set
  (`textDocument/definition`), which the index already knows.
- "Phrases with these words" in reports stays, and says
  `ovis manual --line FILE:N` for more.

### 3.4 Not completion

Agreed, with one small exception left to the owner (decision 4): while
typing, nothing is offered. An entry goes into the file only when the
author picks it ("Use this", or `Alt+Enter` in the search, §3.5).

### 3.5 Coding and the manual side by side (VS Code)

Looking something up shouldn't mean leaving the code. Most useful first:

1. **A manual panel that follows the cursor.** A webview view in the
   secondary side bar, left open beside the code. With no search in it,
   it shows the line under the cursor: the phrases it reads with their
   entries, the names in scope and their types, and for an unclear line
   the closest entries. It updates when a line is done (§3.1), from the
   same check, so it costs nothing more. Referencing then needs no
   switch: a glance to the side.
2. **One key to search.** "Ovis: Search the manual", bound to a key,
   opens a quick pick: results as you type (lexical at once, embeddings
   after a short pause), over what this file sees. `Enter` opens the
   entry in the panel; `Alt+Enter` puts its example at the cursor as a
   snippet, its slots tab stops (`sort ${1:list}`); `Esc` is back where
   you were typing.
3. **From the code to the entry.**
   - Hover on a phrase's word, a name, a record or a C function: the
     short entry and an "Open in manual" link (a command link in the
     hover's markdown).
   - `Ctrl+click` / `F12` on a library phrase opens its entry; on the
     program's or a package's phrase, its definition.
   - Peek (`Alt+F12`): the entry inline under the line.

   Entries are read-only virtual documents (`ovis-manual:` URIs, from a
   `TextDocumentContentProvider` the extension fills from the server), so
   VS Code's own hover, definition, peek and back (`Alt+←`) work on them
   with no new UI, and a link in one entry goes to another.
4. **From a problem to the answer.** The "Look it up in the manual"
   quick fix (`Ctrl+.`, §3.3) fills the panel with the entries closest to
   the line; "Use this" puts one in place of the line with the line's
   names.
5. **Other editors.** Hover, definition and workspace symbols are
   standard LSP, so Neovim and the rest get 3 and 4 (a definition of a
   library phrase is a location in an `ovis-manual:` document the server
   serves through a custom `ovis/manual` request). The panel and the
   search box are the VS Code extension's, on the same request.

Optional, for authors who use VS Code's chat: an `@ovis` chat participant
that answers only with manual entries, under the same rule (it finds,
never writes). Not proposed: a query typed in the source (a line starting
with `?`), which would add syntax to the language for what the search box
does.

### 3.6 Installing the extension with ovisup

Today the extension is installed by hand from the source tree (`npm
install`, `npx @vscode/vsce package`, `code --install-extension`), and
neither ovisup nor the release packages know about it
([deployment.md](../deployment.md)). VS Code installs extensions from its
command line, so ovisup can do it:

1. **One `.vsix` per release.** The extension is JavaScript only, so one
   file serves every platform. `release.yml` packages it with `vsce` and
   attaches it to each release and to `nightly`, beside the toolchain
   packages; a toolchain package keeps a copy in `editors/`.
2. **ovisup installs it.**
   - `ovisup init` looks for `code` and, if found, asks "Install the
     Ovis extension for VS Code?" (yes with `-y`); `--with-vscode` and
     `--no-vscode` decide without asking, like `--with-model`.
   - `ovisup editor vscode` installs it at any time: `code
     --install-extension ~/.ovis/editors/ovis-X.Y.Z.vsix --force`.
   - It finds `code-insiders`, `codium` (VSCodium) and `cursor` too, which
     take the same command, and installs into each one present.
   - `ovisup update` installs it again when the default toolchain
     changes, so the extension and `ovis lsp` stay together.
   - `ovisup self uninstall` removes it (`code --uninstall-extension
     ovis.ovis`).
3. **The extension and the compiler stay matched.** The extension runs
   `ovis lsp` from `PATH`, which is ovisup's shim: the default
   toolchain, or the one a project's `ovis.toml` names. Newer features
   (the manual panel, `ovis/lineDone`) check the server's capabilities,
   so an older toolchain just doesn't show them.
4. **The other way round.** If the extension can't start `ovis`, it says
   so with the ovisup install command, instead of failing silently.
5. **The marketplaces** (decision 7). Publishing to the VS Code
   Marketplace and Open VSX (VSCodium, Cursor) lets authors find it in the
   editor and updates it by itself. It needs a publisher account and a
   token in the repository's secrets, the owner's to make; then ovisup
   installs it by its id. The `.vsix` in the release stays, for offline
   installs and pinned versions.

