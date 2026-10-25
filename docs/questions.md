# Questions

How an unclear line becomes a question, how it is settled, and the
model's rewrites. Part of [architecture-spec.md](architecture-spec.md) §5,
moved here to keep the spec under 500 lines.

**Questions** (`question_of`, `questions.cpp`). The unclear span is the
shortest one nothing reads, in a reading of the rest. Kinds and their rule:
- `ambiguous_parse`: several readings; candidates are the readings.
- `unknown_name`: one word, no name: candidates are the names in scope with
  which the line reads, most recently set first; rule: the one close
  spelling (`core::closest_name`). In a longer unclear span a word that is
  no phrase word and a close spelling of a name is fixed first.
- `reference`: `it`/`them`, same candidates; rule: only one fits. Also a
  record's name (`the book`, with its article): the variables holding one.
- A word where a field's name goes that no field is called (a hole with
  its record, `cost of b`, `b's titl`, `total cost of books`): an
  `unknown_name` whose candidates are the record's fields with which the
  line reads; rule: the one close spelling.
- `unknown_phrase`: templates whose slots take the span's arguments (a
  name of several words is one, the longest there; in any order the types allow), ranked lexically against its other words
  (same word 3, a shared 4-letter start 2, trigrams 1, function words ×0.2;
  aliases count as the template's words). Also, ranked 1 lower, with one
  recent name as a left-out argument of a fixed slot (`missing_argument`),
  kept only if it ranks above every reading of the arguments as written.
  A span starting with a record's name also gets `a NAME with F V, …` (each
  value a field its type fits, written order; record words 3 each).
  Only readings with which the whole line still reads are offered (the
  span replaced by the reading). A name that is also one of the span's
  words (`how many items in items`) may be part of the verb instead, if the
  template has a word like it. Never offered: a reading without a field
  word of the span, or one filling a variadic slot (`show`) with several
  values that shares no word with the span (`drops_meaning`), or a
  dependency's phrase sharing no word of meaning with the span (same word
  or a 4-letter start; phrase-scope.md S3: the library's and the
  program's own are offered by type, a package that wants a synonym
  reached adds an alias). The 9 best are kept, and any as good as the 9th (then a run-off).
- **Edits** (`edit_candidates`, put first by `with_edits`): the span
  edited a little and read by the parser, so a candidate keeps the line's
  own values, fields and structure and any built-in form (`with`, `'s`,
  `set … to`, comparisons) or type slot can be one, which the readings
  above can't. One edit: a word put in (from phrases sharing a word with
  the span, the forms' words, `of`/`is`/`to`/`,`/`'s`…; a joining word is
  cheaper), up to three joining words taken out (`of`, `the`, `does`,
  `which`…; never a word with meaning: `w's age in months` is not `w's
  age`), or a word replaced by one spelt like it (`decrement` →
  `decrease`; `median` → `average` is the model's choice, not an edit).
  Two edits, only for a whole statement: a built-in update's verb put
  first and the one or two joining words before the value made `to`
  (`set`) or `by` (`increase`…): `w's age is now 13` is `set w's age to
  13` (cheaper with `set`).
  Names, values, fields and records are never taken out or replaced.
  Cheapest first, at most 9. An edit leaving only values goes last (and
  is dropped when the span is the whole line). Edits never make the
  ranking shortcut. When nothing at all is offered for the unclear part,
  edits of the whole line are tried (`c's count not 0`, `b title`).
  Measured by `eval/wording-questions.txt` (near misses; CI keeps its
  floor): 3B right on 14 of 28 before, 22 after, every confident pick right;
  the other sets unchanged.

Resolution: the lockfile (below); else the rule; else the ranking shortcut
(a reading covering every content word of the span and adding at most one
of its own, besides function words and the arguments (`first item of
names` for `first of names`, never `count the smallest mean of numbers`
for `mean of numbers`); best score ≥ 3 and 3 above the next); else `choose` over at most 9 readings plus "none of
these" (`0` when there are 9; run-offs for more, a group of one goes on
unasked; a run-off's final makes "none" less likely, so it is avoided),
accepted at p ≥ 0.6 and margin ≥ 0.3 (with `ask_both_orders`, asked
again reversed and averaged: off, twice the time), within
`--model-budget`; else, for an `unknown_phrase` (none offered, or none
taken), a **rewrite**; else a report listing the options and its fixes:
after the model was asked, only the readings it put above "none of these",
likeliest first (`question_asker::not_rejected`; a run-off's losers out),
and if none, no reading, only "write it in other words" and phrases to try:
one it found no better than none is no good fix. All but the report
change the line's meaning. `front_end_timing` counts how each was settled,
and `front_end_result::decisions` lists this build's (`ovis_eval` reads it).

**Rewrite** (`rewritten`, `rewrite_request_of`): the model writes the line's
whole span (`p_shape.begin..end`) again with `chat`, once. The system
message (the built-in forms, two examples, the library's phrases with slots
as `NUMBER`, `LIST`, `FIELD`...) is the same for every program, so its state
is cached; the user message has the records, the program's phrases, the
names in scope and the line. A GBNF grammar allows only the template words,
the forms' words, records, fields, names in scope, the span's own texts,
numbers, operators, `'s` and brackets, so it can't copy an example or invent
a name. A span that names nothing (no name, value, field or record:
`has_anchor`) isn't rewritten, since nothing would check it (`show fan
speed` before `fan speed` was set became `show a blank line`). It is kept
only if it differs from the span, keeps every name (a name of several words
whole),
number, text, field and record word of the span (`rewrite_loss`; a record
word may become a variable holding that record), and the line reads with
it. Its lockfile entry's key is the line's with the one reading
`(rewrite)`, and is used only while it still passes. Name questions are
never rewritten (a rewrite can't invent a name). `front_end_options::rewrite`
turns it off (the questions eval does, to measure choosing alone).

**Diagnostics and quick fixes** ([proposals/quick-fix.md](proposals/quick-fix.md)).
Every problem is a `diagnostic` (file, line, columns of the unclear
words, kind: `unclear`, `no_phrase`, `unknown_name`, `no_field`,
`syntax`, `error`; message; span; fixes); `problems` are their text. A
fix of an unclear line is each reading offered, with `meaning`: the whole
physical line with it put in (`describe_problem`), what `@[meaning: …]`
would hold. `ovis check [--json]` reports without the model or writing
(lines as read). `ovis fix` asks about each problem with words in doubt
(a reading, a meaning written by hand, skip) and writes the taken one as
`@[meaning: …]` above the line at its indentation (replacing one there),
then builds again; `--line [FILE:]N --apply K | --meaning TEXT` takes one
without asking, by the lines `check` cites. `ovis run`/`build` do the
same as `fix` when stdin and stderr are terminals (`--no-prompt` not),
for up to 3 rounds.

**Phrases to try** (`closest_templates`). A report of a line no reading
was taken for ("unclear", "no phrase reads") ends with "Phrases with these
words:": up to 4 phrases whose words are like the span's words of meaning
(not names, of one word or several, values, fields; the same word or a shared 4-letter start),
spoken with slots as their kind (`TEXT in capitals`), by whichever way of
saying it is closest. `ovis phrases WORDS` searches the built-in phrases
the same way (12 best); with no words it lists them all by file, each
with its `about:` sentence and first example.

**`ovis manual`** (`manual_search`, `manual_matches`; L3): the lexical step
of the manual ([proposals/manual-and-editor.md](proposals/manual-and-editor.md)).
Every word of every way of saying a phrase counts in full, every word of its
`about:` sentence by half, so "letter" finds `TEXT in capitals`; best first,
6 shown with topic (the `.phr` file), `about`, examples and the other ways of
saying it, none of it written by a model. `--in FILE` adds the `phrases/`
folder beside FILE and the project's `[build] phrases`; `--line N` adds the
words of line N of FILE (not sigils, comments, texts or numbers); no words
lists every phrase by topic. The program's and its packages' `to …:` phrases are entries too (L7, `program_phrase_templates`):
the `#` lines above a header are its `about`, up to two lines that read to it its examples, the package
its origin. What a file's `use "x.h"` lines import are entries too (L10, `c_header_templates`): functions, function-like and generic macros, constants and structs, found by the name split into words and the parameters' names, titled with the C signature in Ovis's types (`phrase_template::title`), the header's comment sentence, and the program's uses of the name as examples (those it calls first). Records (`a book (record)`: fields, the traits it is), traits (`a shape (trait)`: needed and default phrases, who implements it) and a package's top-level constants (`max_sides = 12`) are entries too (L7 rest, same scan, `#` lines above as `about`, the program's lines that mention the name as examples); a package's entries end with `From NAME 1.4.0.` (`phrase_file::package_version`).

*Embeddings (L4, `manual.cpp`), by a model made for search.* `ovis model
pull embedding` fetches EmbeddingGemma-300M (`default_embedding_model`, 334
MB); when it is found (`find_embedding_model`: `--embedding-model PATH`,
beside the executable, `../share/ovis`, the cache directory) and not
`--offline`, `ovis manual` uses it, never the chat model (`--embeddings`
fails without it). The query and each entry's text (every way of saying it,
then `about`; not the examples) are embedded after the model's prefixes
(`task: search result | query: `, `title: none | text: `; another model's
are left out), with its own pooling and special tokens (`builtin_model::embed`
takes them from the GGUF's `ARCH.pooling_type`; a chat model's is its last
token). The lexical order and the order by similarity are joined by
reciprocal rank (`1/(10+place)`, the lexical place counting half,
`manual_embeddings::lexical_weight`), ties to the lexical order, so "shout"
finds `in capitals` and a shared word still counts. Entry vectors are kept
in `manual-embeddings.bin` in the model cache directory, keyed by the model
and the entry's text, one vector size to a file (another size is dropped
when the file is written): after an edit only the changed entries are
embedded, a package's or the program's too. A model that can't embed leaves
the search lexical. Measured by `ovis_eval manual eval/manual-queries.txt`
(state.md). CI keeps the words' floor with `--no-model`.

*The model choosing (L5, `manual_choose`).* After the search, when there
are words and not `--offline` or `--html`, the chat model (`make_model`,
the daemon's) is shown the query and the first 5 entries, each its pattern
and `about`, and "none of these" (`manual_choice_probabilities`, one
`choose`). Its pick moves first only when it is sure, as a question's
answer is taken (60%, 30 points ahead of the next, "none" included); the
rest keep the search's order, and no model or a failing one leaves the
order as it was. It moves real entries and never writes a word (decision
1). Measured with `ovis_eval manual --choose-model FILE` (state.md):
always taking its pick loses to the search; only a sure one wins.

**`ovis fmt`** (`formatted_source`, `format.cpp`): text only, no build.
Each code line's sigils go above it at its indentation, spaced as builds
write them; old `Read as:` lines under it become one `@[meaning: …]` (the
decisions applied to the line, whole words), `Compiler:` lines `@[note:
…]`; a meaning the same as its line goes. Stranded sigils are left for
the build to report. `--check` only says which files would change (CI
checks the examples).

`ovis_eval questions` and `ovis_eval programs` measure ranking, the model and
whole programs; their options (`--orders`, `--embeddings-only`, `--min-first`,
`--min-shown`, `--with`) are in
[dream/architecture-spec](dream/architecture-spec/INDEX.md).

## Wording offered

A phrase with aliases is offered in the wording the line is closest to:
`number timez 2` gets `number times 2`, `number multiply 2` gets `number
multiply by 2`, `the grand total of scores` gets `total of scores`
(`unknown_candidates` scores each alternative pattern against the line's
words and builds the reading with the best; ties go to the first). The
reading is the text the sigil and the lockfile keep, so any wording is
read back the same way.
