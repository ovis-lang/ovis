# Proposal: quick fixes for unclear lines

Status: **accepted** (decisions at the end); plan Q1–Q7 below. It
answers the frustration the near-miss work left: about 5 in 28 lines a
word or so off Ovis's wording still end in a report, and a report today
is a dead end the author fixes by guessing.

## Why

A build that can't settle a line prints something like:

```text
main.ovis, line 23: "fan speed = 100" is unclear. Did you mean one of these? Write it in the source:
    show 100
```

Three things make that frustrating:

1. **Acting on it is manual.** To take a suggestion, the author must work
   out where it goes and type it: as the line, or as a `@[meaning: …]`
   above it, in exactly the right form.
2. **No suggestion means no path.** "No phrase Ovis knows reads …" says
   nothing about what Ovis does read near those words.
3. **Nothing reaches the editor.** Problems are lines of text on stderr;
   an editor can't underline the span, offer the candidates, or show
   what a sigil means.

Ovis already has the hard parts. Each question knows its span, its kind,
and its candidates: readings the whole line reads and type-checks with
(`question_of`, edits included). The meaning sigil already makes an
author's choice authoritative and reproducible. What's missing is getting
those candidates out of the build in a form tools can act on.

## Design

### 1. Structured diagnostics (the foundation)

Every problem becomes a `diagnostic` instead of a string:

```text
diagnostic {
  file, line, column_begin, column_end   // the span, in the file on disk
  kind        // unclear | no_phrase | unknown_name | no_field | error
  message     // what is printed today
  span        // the words in doubt
  fixes: [ {
    title     // "Read as: a random number from 1 to 15"
    meaning   // the whole line as it would read
    edit      // the text change: insert this sigil line above line N
  } ]
}
```

- `front_end_result::problems` stays (it is the text of each diagnostic),
  so nothing printing it changes.
- The candidates come from where `settle()` builds its report now. A
  fix's `meaning` is the line with the reading put in, the same text a
  `@[meaning: …]` would hold, so taking it is exactly what a model's
  choice would have done.
- Unknown names and fields get fixes too (the names and fields that fit),
  and a misspelling's one close name is the first.
- `ovis check FILE --json` prints the diagnostics and builds nothing.
  `check` without `--json` is a build with no code generation: fast, for
  editors and scripts.

### 2. `ovis fix`: taking a fix from the terminal

```text
$ ovis fix main.ovis
main.ovis, line 23: "fan speed = 100" is unclear.
  1. Read as: show 100
  2. Write it yourself
  3. Skip
Choose [1-3]:
```

- Picking a reading writes the `@[meaning: …]` above the line. It counts
  as the author's (no lockfile entry), so the next build uses it with no
  model.
- "Write it yourself" opens the line in `$EDITOR`, or takes a line typed
  at the prompt. It is checked at once: if it still doesn't read, the
  prompt says why and asks again.
- `--apply N`, without prompts, for scripts and editors:
  `ovis fix main.ovis --line 23 --apply 1`.
- It runs after the model, so it only asks about what the model couldn't
  settle; with `--offline`, about every unclear line.

### 3. Something to try when nothing fits

When there are no candidates ("No phrase Ovis knows reads …"), the report
lists the phrases closest to the line's words, with their slots. These are
the same lexical scores the ranking already computes, without the "fits
the line" filter:

```text
main.ovis, line 9: No phrase Ovis knows reads "winnie's age in months".
Phrases with these words:
    X in capitals                  (text)
    X with N decimals              (number)
```

`ovis phrases [words…]` searches the built-in phrases the same way (as
built: without examples, which live in the test data, not the binary).
That's a reference an author can use before a build fails.

### 4. Editor support: `ovis lsp`

A Language Server Protocol server over the same diagnostics:

- **Diagnostics:** the unclear span underlined, with the message.
- **Code actions:** each fix is a quick fix ("Read as: …"); taking one
  inserts the sigil above the line.
- **Hover** on a line with a meaning shows how the compiler reads it; on
  a sigil, which build or which author wrote it (from the lockfile).
- **Dimming:** `@[…]` lines are faint, so the source reads as written.
  This answers the earlier wish to hide the additions, without hiding
  them from the file or the diff.
- Re-checks on save, not on every keystroke. The model isn't asked while
  editing: fixes come from rules, ranking and edits, and the model only
  runs on a build.

(The VS Code extension this proposal planned was built, then dropped on 2026-10-24.) `ovis lsp` itself was removed on 2026-10-24.

### 5. `ovis fmt`

Already a task: re-render sigils, migrate old `Read as:` lines, and
remove meanings that no longer differ from their line, without building.
It shares the code of `fix --apply`.

## Plan

| Step | What | Test | Size |
| --- | --- | --- | --- |
| Q1 | `diagnostic` in the front end; problems become its text; fixes from candidates, names and fields | `diagnostic` fields for every kind of problem in `question_test` | M |
| Q2 | `ovis check [--json]` | the JSON of a known program, stable | S |
| Q3 | `ovis fix` (prompt, `--apply`), and `run`/`build` offering fixes in a terminal | apply a fix, rebuild with no model, nothing asked | M |
| Q4 | Closest phrases for "no phrase reads"; `ovis phrases` | the lists for the near-miss cases | S |
| Q5 | `ovis fmt` | migrate and re-render the examples, byte for byte | S |
| Q6 | `ovis lsp` (diagnostics, code actions, hover, folding) | an LSP session driven by a script | L |
| Q7 | VS Code extension (start the server, highlighting) | manual, on the owner's machine | S |

Q1 is the base for everything else. Order: Q1–Q3, then Q6–Q7 (VS
Code), then Q4–Q5. **Done: Q1–Q7** (`test/fix_test.cpp`); Q1 also made
reports cite the file as written, which the spec promised but nothing
did.

## Decisions (owner, 2026-10-19)

1. **Sigil.** Taking a fix writes `@[meaning: …]` above the author's line,
   keeping their words. There is no option to replace the line.
2. **`ovis run` and `ovis build` offer the fixes** when they run in a
   terminal (stdin and stderr are terminals), then build again. In a
   script, a pipe or CI they never prompt, and report as now. `ovis fix`
   does the same on demand, and `--apply` without prompts.
3. **VS Code first**: Q6–Q7 come before Q4–Q5.
4. **Sigils fold by default** in the editor. VS Code can't fold a single
   line away (a fold hides the lines after its first), so the owner then
   chose **dimming**: sigil lines are faint and italic, and hover tells
   how a line is read.
