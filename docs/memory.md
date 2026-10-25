# Memory at run time

How a program's memory is made, freed and checked, as built: moved out of
[architecture-spec.md](architecture-spec.md) §8 (code generation) when that
file neared its 500 lines. The design is in
[proposals/memory.md](proposals/memory.md).

- **Memory** ([proposals/memory.md](proposals/memory.md)). Every
  allocation goes through the runtime's current memory: the program's
  (`calloc`) or the top of a stack of arenas (`with-memory`, `new-memory`;
  chunks of 4 KB doubling, zeroed). A list's storage grows in the memory
  the list was made in. `with-memory` pushes in its block and pops in the
  block's exit frame (the same frames as `defer`; `exit_action`), freeing
  the memory when the block made it. `free` is one Core form; codegen picks
  `ovis_free_text/list/record/pointer` or `ovis_memory_free` from the type.
  Text written in the program is emitted into a section `ovis_const`,
  which `free` recognises and ignores. A chain of `+` is `ovis_join`: one
  allocation. `keep` is per-type copy functions (`record_keeper`) over the
  runtime's pointer map.
- **Checked builds** (the default; `--fast` and CMake's `OVIS_FAST` turn
  them off): the runtime is compiled with `-DOVIS_CHECKED` and codegen
  stores the source line (`unit::source_lines`, set by the front end) in
  `ovis_line` before each statement. A table by address notes every value
  made, with its line and memory; `free` poisons a value and holds it back
  for 512 frees; lists, records and text operations check their values, so
  a value used after it was freed, freed twice, or kept in a freed memory
  is an error naming the lines and the variable it was last stored in
  (`ovis_name`, outside the prelude); text, lists and records given to a C
  function are checked first (`ovis_live`); a read or write through a
  pointer into room Ovis made (`new room for 4 items`) is checked against
  the room's end and freed state (`ovis_room_check`, which remembers the
  rooms used last). `--report-leaks` lists at exit the values
  of the program's memory never freed, by the line that made them. Checked
  mode costs 3–6× the time and memory on programs that make many values
  (state.md). Unless `--lax-memory`, `ovis` also runs `check_memory`
  (`memory_check.cpp`): a forward pass that follows where a variable's value
  was made, and reports new values made where nothing can free them, a
  variable given a new value while it may hold an unfreed one, a value
  stored outside or read after the `with new memory:` that made it (or
  after `clear`/`free` of the named memory a `with M:` made it in; an
  item read from a list whose items were made there too), and a
  phrase that gives back both new and existing values. A block's `at the
  end` lines are walked where they run, when the block is left (in a
  loop's body, before the next pass), so `at the end, free xs` frees what
  the pass made; in a function's body or at the top level they may not
  have run yet.
