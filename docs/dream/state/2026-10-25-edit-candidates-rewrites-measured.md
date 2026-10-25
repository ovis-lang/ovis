# Edit candidates and rewrites measured

Archived from state.md → "Edit candidates measured" and "Rewrites measured" on 2026-10-25 (dream at 45455c9).

## Edit candidates measured

`eval/wording-questions.txt`: 28 near misses (a word left out, an extra
joining word, a word spelt like the phrase's, built-in forms, fields).
3B, one order, before → after edit candidates: right 14 → 22 of 28 (every
confident pick right); `questions.txt` 24 → 24 of 26; `hard-questions.txt`
12 → 12 of 20. Ranking (`--no-model`): near misses first 13 → 21, shown
15 → 23. Tried and dropped: replacing a word by any phrase word (`median`
became `average` confidently), a single question of 9 (cut right readings
the run-off kept), dropping words with meaning (`w's age in months` read
as `w's age` at 86%). Two edits (a verb put first, the joining words
before the value made `to`/`by`): wording ranking first 21 → 23, shown
23 → 25 of 28 (`w's age is now 13`, `goes to 14`); the other sets
unchanged; CI floor raised. Left to rewrites: word order, `sort out`,
`the order of`.

## Rewrites measured

`ovis run` through the daemon, 4 CPUs, 3B (7B in brackets). A rewrite
takes 2.2–3.1 s (5.3–7 s); the first read of its system message (~1500
tokens, the phrase list) takes ~20 s (47 s) once, then its state is
loaded from the prompt cache (32 MB). Lines no reading fitted (the
records probes): 3B rewrote right `winnie's age becomes 13`, `make
winnie's age 14`, `double winnie's age`, `the_car's owner turns 13` (set
its owner's age), `give the car 2 more wheels`; refused, so reported,
`remove winnie from people`, `in months`, `is a teenager`, `names of
people`, `the oldest person`. 7B also got `is a teenager` (age between 13
and 19), `the oldest person` and `the number of wheels the_car has`; its
one wrong rewrite (`turns 13` as `increase … by 1`) was refused. No wrong
rewrite was kept by either. Asked again with the refusal's reason, 3B
wrote the same in 4 of 5, so there is one try.

