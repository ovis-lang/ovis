# History and measurements: the manual's L4–L6

Archived from state.md → "History" and "Manual measured" on 2026-10-25 (dream at b68e55d).

| Commit | Summary |
| --- | --- |
| (P2) | **L5, the model choosing in `ovis manual`**: `manual_choice_probabilities` asks the chat model (one `choose`) which of the first 5 entries found (pattern and `about`) does what the query asks, or "none of these"; `manual_choose` moves its pick first only when sure (60%, 30 points ahead), else the order stands; `ovis manual` uses it with the daemon's model when there are words, not `--offline` or `--html`. `ovis_eval manual --choose-model FILE` measures it: first 28 of 34 against 25, held out 16 of 30 against 12, no right first lost, ~2.9 s a query. `manual_test`; test cases 256 (253 pass, 3 skipped), 3544 assertions |
| (P2) | **The manual's held-out queries and model decided**: `eval/manual-held-out.txt` (30 queries written after the weight was chosen): words 1 of 30 first, EmbeddingGemma 12 joined, 15 alone, Qwen3-Embedding 15 and 19; the weight stays ½ (partly fitted, above); EmbeddingGemma ships (decisions); no code changed |
| (P2) | **L4 with a model made for search** (what blocked L5): `builtin_model::embed` pools as the GGUF says (`ARCH.pooling_type`) with its special tokens, so BERT-like and Gemma/Qwen3 embedding models work (a chat model still by its last token); `ovis model pull embedding` (EmbeddingGemma-300M, `default_embedding_model`), which `ovis manual` uses when found (`find_embedding_model`, `--embedding-model`, `--embeddings` fails without it), never the chat model; query and entry prefixes (`manual_embeddings`); the words count half (`lexical_weight`). Fixed: `manual-embeddings.bin` written with one vector size for vectors of two models, after which every read failed and the search fell back to the words. `ovis_eval manual` reports embeddings alone too, `--lexical-weight`, `--query-prefix`, `--entry-prefix`, `--no-prefixes`. Measured (above): first 25 of 34 against 13. `manual_test`; test cases 255 (252 pass, 3 skipped), 3539 assertions |
| (P2) | **L6, `ovis manual --html DIR`**: `write_manual_html` writes `index.html` (every entry under its topic) and one page per topic (per package and topic), an anchor per entry with its sentence, other ways of saying it and examples; with words, only the entries found. The links are checked in the files written (a page and an id must be there), problems go to stderr and exit 1; `manual_test`. Test cases 254 (251 pass, 3 skipped) |

## Manual measured

`ovis_eval manual eval/manual-queries.txt` (34 queries far from the
phrases' words), 4 CPUs, 2026-10-25: where the entry meant comes, first /
in the first 5, and the time a query once the entries are cached (each
model embeds the 171 once, in 1–20 s).

| Embeddings | Joined, words 3× | Alone | Per query |
| --- | --- | --- | --- |
| None (the words) | 13 / 21 | — | — |
| 3B (Qwen2.5-Coder, last token; L4) | 13 / 21 | — | 0.5 s |
| bge-small-en-v1.5 (37 MB) | 17 / 22 | 18 / 24 | 56 ms |
| nomic-embed-text-v1.5 (146 MB) | 16 / 23 | 15 / 26 | 0.2 s |
| EmbeddingGemma-300M (334 MB) | 19 / 24 | 23 / 33 | 50 ms |
| Qwen3-Embedding-0.6B (639 MB) | 18 / 24 | 24 / 31 | ~0.2 s |

Each with the prefixes its authors give (Qwen3 an instruction).
EmbeddingGemma joined with the words counting 0.25 or 0.5: **25 / 33**
(46 ms); at 1, 19 / 31. A model made for search reaches what the words
miss (`trimmed`, `reversed`, `rounded down`, `a copy of`, `in hex`); 3B's
vectors did not. So the words now count half and `ovis manual` uses
EmbeddingGemma when it is pulled. The weight was picked on these 34
queries.

Held out (`eval/manual-held-out.txt`, 30 queries written after the weight
was chosen, about entries the first set doesn't cover), first / in the
first 5, no prefixes but EmbeddingGemma's:

| Embeddings | Joined, words ½ | Alone |
| --- | --- | --- |
| None (the words) | 1 / 10 | — |
| bge-small-en-v1.5 | 10 / 19 | 13 / 20 |
| nomic-embed-text-v1.5 | 9 / 21 | 12 / 21 |
| EmbeddingGemma-300M | 12 / 24 (words ¼: 13 / 24; 1: 10 / 19; 3: 5 / 15) | 15 / 24 |
| Qwen3-Embedding-0.6B | 15 / 24 | 19 / 25 |

So embeddings hold up far from the words, and the weight was partly fitted:
here the words cost 3 firsts, on the first set they gave 2. It stays ½
(over both sets ½ gives 37 of 64 first, ¼ and alone 38): not tuned on
these. Qwen3 reads better but is twice the size (639 MB) and ~0.2 s a
query on 4 CPUs once cached (EmbeddingGemma ~60 ms; the first search
embeds the 171 entries, 20 s against 6 s). Each model drops another's
vectors from the cache file (one size to a file). Decided (owner,
2026-10-25): EmbeddingGemma ships; Qwen3 is the switch if its licence or
reading matters more than 3× the time.

L5, the chat model choosing among the first 5 (`ovis_eval manual
--choose-model`, 3B on 4 CPUs, after EmbeddingGemma joined with the words),
first / in the first 5:

| | 34 queries | Held out (30) |
| --- | --- | --- |
| The search (EmbeddingGemma and words) | 25 / 33 | 12 / 24 |
| Its pick always first | 21 / 33 | 12 / 24 |
| Its pick first only when sure (60%, 30 points ahead) | **28 / 33** | **16 / 24** |

3B is seldom sure (most picks 25–50%), and its unsure picks undo the
search's right firsts as often as they fix them. Its sure picks (7 of 64,
64–90%) were all right and none moved a right first entry down: `in
capitals`, `average of`, `as text`, `squared`, `last item of`, `remove
item`, `number of entries in`. "None" was likeliest once. ~2.9 s a query
once loaded (the daemon keeps it; ~6 s with the start). So `ovis manual`
asks the model when there is one and moves only a sure pick.
