# The manual's finished tasks (L4, held-out queries, L5) and R1

Archived from remaining-tasks.md → "A manual and checking a line when it is done" and "Deployment" on 2026-10-25 (dream at 45455c9).

- [x] L5 (state.md, "manual measured"): the chat model chooses among the
  first 5 found, and its pick moves first only when it is sure: first 28 of
  34 against 25, held out 16 of 30 against 12, ~2.9 s a query on the CPU.
  L1–L10 and R1 are all done, so the manual's plan is finished.
- [x] L4 with a real embedding model (state.md, "manual measured"):
  EmbeddingGemma-300M beats the words (first 25 of 34 against 13, in the
  first 5 33 against 21), so `ovis manual` uses it when it is pulled. L5 is
  unblocked: the model's `choose` now has to beat 25 of 34 first, among
  the first 5 it puts up (33 of 34 have the entry), for ~2 s a query on
  the CPU.
- [x] Held-out queries (`eval/manual-held-out.txt`, 30) and the shipped
  model decided (EmbeddingGemma): state.md, "manual measured".

- [x] Version 0.1.0 (R1 in
  [proposals/manual-and-editor.md](proposals/manual-and-editor.md)):
  CMakeLists.txt and ovisup say 0.1.0, kept one by a test.
