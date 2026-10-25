# Ovis Core

The S-expression language the phrases lower to: its types, forms, statements and expressions. Part of [architecture-spec.md](architecture-spec.md) §6, moved here to keep the spec under 500 lines.

Ovis Core is written as S-expressions; `;` starts a comment.

**Types:** `int` (i64), `float` (f64), numbers of a size `i8 u8 i16 u16 i32
u32 u64 f32` (also `byte`, `int32`…; [proposals/numbers.md](proposals/numbers.md)), `bool`, `str` (char*), `ptr` (void*),
records `(record NAME)` (a bare name would read as a variable in
`(let x y)`), `(maybe (record NAME))` for one that may be `null` (a
field of it is read only after a check like `(!= x null)` narrows it), and lists `int[] float[] bool[] str[]` (also `(list int)`,
`(list (record NAME))`). Any type can be left out or written `?`, and the
elaborator infers it.

**Top-level forms:**
- `(fn NAME (PARAM...) [RET] BODY...)`, where a PARAM is `x`, `(x)` or `(x TYPE)`
- `(extern NAME (CTYPE... [...]) CTYPE)`
- `(link "lib")`
- `(use-c "HEADER.h" ["LIBRARY"])` imports a C library (§8)
- `(record NAME (FIELD [TYPE])...)`: a record type, by name for the whole
  program (two declarations are an error). A field's type may be left out.
  `(packed-record NAME …)`: its fields with no gaps and no hidden tag (its
  memory is its fields); it can't implement a trait.
- `(trait NAME (METHOD (it PARAM...) [RET])... (fn METHOD (it PARAM...)
  BODY...)...)`: a trait. A `METHOD` list is a phrase every impl must
  write (a missing `RET` is nothing; other parameters need types); a `fn`
  is a default, whose `it` is a value of the trait. Methods are global
  names: a method may not share its name with another trait's, a function
  or a built-in.
- `(impl TRAIT RECORD (fn METHOD (it PARAM...) BODY...)...)`: one impl per
  trait and record (the type must be a record). Each `fn` becomes the
  function `METHOD__RECORD`; its `it` is the record, and unwritten types
  are the trait's (`pin_trait_types`). A parameter written `(b same)` in
  a trait (a method or a default's `fn`) is the same record as the first:
  an impl's is its record, and a dispatched call checks it. The checker reports missing
  methods (listed), methods the trait lacks, and types that differ.
- `(fn NAME (forall T (U BOUND...)...) (PARAM...) [RET] BODY...)`: a
  generic function ([proposals/generics.md](proposals/generics.md)); `(var
  T)` is its type variable in types (`(list (var item))`). Its body is
  checked once, a variable allowing only what its bounds do (`=` needs
  `equatable`, `<` and `sort` `comparable`, showing and `+` with text
  `showable`, arithmetic `number`, a user trait's phrases that trait).
  Each call gets fresh slots for the variables (the elaborator writes them
  in `expression::type_arguments`); the checker checks the bounds at the
  call and, with concrete types, calls a copy `NAME__T…` (`push__int`,
  `largest__book`, `pair__int__str`) made once from the generic and
  checked like any function; generics are removed before codegen. In a
  copy, a record compared or joined to text where its variable was uses
  the phrases its author wrote (`comes_before__book`, `is_same_as__book`,
  `text_of__book`). A copy's problem the generic already had is not said
  twice.
- `(record NAME (forall T...) (FIELD TYPE)...)`: a generic record; a use
  is `(record stack int)`, made by `(new (stack int) …)` or `(new stack …)`
  when the fields given decide its types (else "say what a stack holds").
  A field's type is its record's with the use's types. `(impl TRAIT NAME
  (forall (T BOUND...)...) (fn …)...)` is an impl for every use whose
  types meet the bounds; its phrases are generic functions. Once checked
  (`make_record_copies`), each concrete use is a record of its own
  (`stack__int`, `stack__stack__str`), with its tag, fields, impls and
  the copies of their phrases (`describe__stack__int`), and the generic
  records, impls and functions are removed. A copy prints as its use
  (`a stack of whole numbers with …`).
- A generic's type variable may be a field (`(forall r (field f))`): its
  calls are written `NAME:RECORD:FIELD` (a template's field slot) and each
  record and field gets a copy before elaboration
  (`prelude_total_by__book__pages`, `copy_field_generics`); `(field x f)`
  and the call names' parts (`prelude_sort_by:r:f`) take them.
- `(impl TRAIT int …)`: a number, text or truth value has a user trait
  statically (generics.md §4); its phrases are `METHOD__int`.
- any statement. Top-level statements run in order in `main`, and top-level
  `let`s are globals.

**Names across modules** (`resolve_modules`, before elaboration):
- A name defined by one module keeps its name. A name defined by several is
  renamed `module.name` in each.
- A reference resolves to its own module's definition first, then to the
  only module defining the name.
- When several other modules define it, the build reports the choices. Any
  tail of the module path picks one: `tax.add_tax` or `pricing.tax.add_tax`.
- Locals are never rewritten.

**Statements:** `let set put set-field print if(then/else) while
for(inclusive, up) for-down(inclusive, down) each break continue return pass`, plus calls.
`(set-field X FIELD VALUE)` sets a field of the record X.
`(for-down NAME FIRST LAST BODY...)` counts from FIRST down to LAST.
Memory ([proposals/memory.md](proposals/memory.md)): `(with-new-memory
BODY...)` makes a memory for the block and frees it however the block is
left; `(with-memory M BODY...)` makes things in the memory M (type
`memory`, from `(new-memory)`) for the block; `(clear-memory M)` frees what
M holds and keeps it; `(free X)` frees text, a list (not its items), a
record, a C pointer or a memory; `(keep X)` copies what the innermost `with`
made into the memory outside it, once for what is shared or circular, and
gives back X itself for what is already outside. Freeing text written in
the program does nothing.
`(defer STATEMENT...)` ("at the end", Zig's `defer`): the statements run
whenever control leaves the block it is in, last deferred first, if its
line was reached. The checker rejects `return`, and `break`/`continue`
aimed at a loop outside it, inside one; a helper file's top level may not
use it. The elaborator's implicit return skips trailing defers (the value
statement moves after them).
`(block [:LABEL] BODY...)` (also `(do …)`, `(begin …)`), Zig's bare block:
a scope of its own (names declared in it end with it) whose defers run
when it is left. `(leave [:LABEL] [VALUE])` leaves the innermost block, or
the one labelled, running the defers in between. `(let x [TYPE] (block
…))` and `(set x (block …))` give the block a value, Zig's `blk: { … break
:blk v; }`: the `let` reads as `(let x)` (its type inferred) and the block
sets x by `leave`; such a block must end by leaving (the checker's
`always_leaves`), and `leave` in it must give a value of x's type. A
deferred block can't leave a block around it. The Core reader still accepts the loose spellings a model used to write
(`let` as `set`, an `if` without `then`, operator aliases ...):
[dream/architecture-spec](dream/architecture-spec/INDEX.md).

**Expressions:**
- arithmetic: `+ - * / div %`. `/` is always real division; `div` is
  integer division. `+` with text joins the values as text.
- numbers (numbers.md §3): the same type in gives it out, worked out and
  overflow-checked at its size (unsigned with unsigned instructions);
  mixed sizes widen to the smaller type holding both (`common_number`; a
  `u64` with a signed type is an error); a number written in the program
  takes the type it meets if it fits, else is a build error (in a
  comparison it widens). A number goes where any number of a size does,
  checked when the program runs; a decimal never goes where a whole number
  does. `(as T X)` checks, `(wrap-to T X)` keeps the low bits,
  `(clamp-to T X)` the nearest value; `(wrapping E)` / `(saturating E)`:
  E's `+ - *` keep the low bits or the nearest value. Literals: `0x1F
  0b101 0o17 1_000`; one above the largest `int` is a `u64`.
- bits of whole numbers (result of the number's type, places of any):
  `bit-and bit-or bit-xor bit-not shift-left shift-right rotate-left
  rotate-right bit-test bit-set bit-clear bit-flip (bit-field X LOW HIGH)
  count-ones leading-zeros trailing-zeros byte-swap`; a place outside the
  number is a checked error. `(float-bits D)` / `(bits-float B)`;
  `(to-bytes X)` (lowest first; text's UTF-8), `(to-bytes-big X)`,
  `(from-bytes T BYTES I)`, `(from-bytes-big T BYTES I)`; `(to-hex X
  [DIGITS])`, `(to-binary X [DIGITS])`, `(from-hex TEXT)`.
- comparisons and logic: `= != < <= > >=` (text is ordered as by
  `strcmp`), short-circuit `and`/`or`, `not`
- conversions and maths: `int float sqrt pow floor abs min max`
- lists: `list array get len append remove-at sort reverse`
- text: `concat substring char_at chr to_str buffer` (a chain of `+` or
  `concat` is one allocation, numbers and truth values as text written
  into it), `(set-char TEXT INDEX CODE)` writes into text Ovis made (the
  prelude fills buffers with it)
- memory: `new-memory clear-memory free keep`
- input and randomness: `read_int read_float read_line random`
- calls to user functions, libc, externs and imported C functions
- records: `(new NAME (FIELD VALUE)...)` gives every field once, in any
  order (defaults are the surface's); `(field X FIELD)`. Records are by
  reference: copies, arguments and list items share one record. `=`
  compares identity unless the record implements `equatable`. A record is
  printed as written, joined to text only with `text of`, and sorted only
  with `comes before` (built-in traits, §4).
- **Traits as types:** a trait's name is a type, written like a record's:
  `(record shape)`. A value of it is any record that implements it
  (`can_assign`), also as an item (`append`, `put`, a list written out:
  its items may be of different records) but not a whole list (`list of
  circle` is not `list of shape`). It has no fields and can't be made.
  Calling a method (`(area x)`) on a record calls its impl directly (the
  checker renames the call `area__circle`, or leaves the default's name);
  on a value of the trait it is dispatched when the program runs: every
  record starts with a hidden number (its place among the records), and
  each trait method has a table of functions indexed by it
  (`ovis.dispatch.METHOD`: the impl's, else the default, else null); a
  method only one record answers is called directly after a tag check.
  A trait value is therefore the record pointer itself; the table and tag
  are the "pointer plus vtable". Printing a trait value prints the record
  it is. Where nothing says what an untyped parameter or variable is and
  only phrases of one trait are used on it, it is that trait (`hint`s in
  the elaborator); phrases of several traits are an error asking for its
  type. An impl's version of a default has the default's types.
- `(ref X)`: the address of a variable, a field or a list item, typed
  `(ptr T)` (a truth value in a list is a `u8`). Given straight to a C
  function, a variable goes through a temporary of the C type when sizes
  differ (`sqlite3_open(path, &db)`).
- **Pointers** (references.md §3, §5): `ptr` is C's `void *`; `(ptr T)`
  one value, `(many T)` several, `(cptr T)` what C gives. T is a C number
  (`i8 u8 i16 u16 i32 u32 u64 f32`, also `byte`, `int32`…; `int`/`i64`
  and `float`/`f64` are Ovis's 64-bit slots), an Ovis type's slot, or
  `(struct NAME)`. A typed pointer goes where `ptr` does, and the three
  kinds of the same T where each other does. `(load T P)` reads (a C
  number as a whole number or decimal; a struct gives the record at P),
  `(store T P V)` writes, `(offset P N [T])` moves by N of T (bytes for
  `u8`; P's own T without one), `(cast TYPE X)` changes a pointer, record
  or text into another, or to and from its address as a whole number,
  `(size-of T)`. `(alloc T N)` is room for N zeroed items of T in the
  current memory, a `(many T)`; `(alloc-more P T OLD NEW)` the same room
  grown, its items copied (and the old room freed in the program's
  memory); T may be a type variable (generics.md §5). Checked builds fail
  on reading or writing through null, past the end of room Ovis made, or
  into freed room; `keep`/`copy` refuse a record holding a `(many T)`.
- **C arrays** (references.md §4, Zig's slice as a list): `(list u8)` is a
  list whose items are kept as that C number (`value_type::pointee` on a
  list), read as whole numbers or decimals; `(array u8 N)` makes N zeroed,
  `(view T P N)` is N items over memory C has (it can't grow, and freeing
  it frees only its header). A list or array goes where C takes a pointer
  as its items (`ovis_list_data`; valid until it grows) when they are what
  C keeps there (`double *` a list of decimals, `char *` an array of
  bytes, `void *` any); `(cast ptr xs)` / `(cast str xs)` give them too.
  Its items are numbers of that size; they sort. A list of them keeps it
  (`(list (list i16))`, `value_type::pointee` through every level; only
  the innermost is a C array). A function taking `int[]`
  or `float[]` given one is called as a copy for it (`NAME__i16`, made by
  the checker).
- **Maps** (`(map K V)`; K is `int`, `float` (by value, `-0.0` is `0.0`),
  `str` or a number of a size (a stated one is pinned: keys meet at it); V
  any; a number of a size is widened in and narrowed out, and `map-keys`
  of sized keys is a list at their size): `(map-new K V)`,
  `(map-put M K V)`, `(map-get M K)` (a missing key is a runtime error),
  `(map-has M K)`, `(map-remove M K)`, `(map-keys M)` (a new list),
  `(len M)`; `(each k M …)` goes through the keys. Entries keep the order
  they were added in (printed `{"a": 1}`). The runtime's table holds
  8-byte keys and values (text keys compared by their characters, not
  copied); `free` is shallow, `keep`/`copy` follow text keys and values.
- **Phrases as values** (references.md §7, §7b): `(phrase (ARG…) RESULT)`
  in Ovis's convention, `(c-phrase (CTYPE…) CTYPE)` in C's (`i32 ptr str
  int float void`…); a pointer to a function, which goes where `ptr` does
  but only into a phrase type of its own convention and signature.
  `(phrase-of NAME)` is a phrase the program defines as a value (not a
  trait's). `(lambda ((x T)…) RESULT BODY…)` is a phrase written in place: the unit gets it as a function of its own, named from the unit's prefix, and it is `(phrase-of NAME)` there; it sees only its parameters. `(call F ARG…)` calls one (a C phrase's numbers converted as
  for C functions; null is a checked error). Where a C function takes a
  function pointer, `(phrase-of NAME)` is given through a C function Ovis
  writes for it (`ovis.c.NAME`), its types checked against the header's
  prototype; `null` may be given too. A header's function pointers (a
  parameter or result) are C phrases.
- **Generic C macros** (generics.md §9, c-libraries.md): a call `(MAX a
  b)` is renamed to the copy for its arguments' C types (`MAX__i64_f64`);
  a type argument is `(type T)`, a field one `(member NAME)`, a value the
  macro sets `(ref x)`.
- constants: `null EOF pi stdin stdout stderr RAND_MAX CLOCKS_PER_SEC`

The canonical list of built-ins is `builtin_names()` in `libc.cpp`.

