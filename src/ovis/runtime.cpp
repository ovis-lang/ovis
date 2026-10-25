#include <ovis/codegen/runtime.hpp>

namespace ovis::codegen {

// Linked into every compiled program. Kept in C so the only tool needed at
// build time besides Ovis itself is the system C compiler used for linking.
auto runtime_source() -> string_view_type {
  return R"runtime(
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Checked builds (the default; `--fast` turns them off) know which source
   line is running, and keep a table of every value Ovis made, so a value
   used after it was freed is an error that names the lines. */
#ifdef OVIS_CHECKED
int64_t ovis_line;
#endif

static void ovis_fail(const char *message) {
  fflush(stdout);
#ifdef OVIS_CHECKED
  if (ovis_line > 0) {
    fprintf(stderr, "ovis: runtime error: %s (line %lld)\n", message,
            (long long)ovis_line);
    exit(1);
  }
#endif
  fprintf(stderr, "ovis: runtime error: %s\n", message);
  exit(1);
}

static void ovis_fail_with(const char *format, long long first, long long second) {
  char message[256];
  snprintf(message, sizeof message, format, first, second);
  ovis_fail(message);
}

/* The prelude's own errors (`the first item of xs where _ is even` when no
   item is): Ovis Core calls it as a C function. */
void ovis_runtime_error(const char *message) { ovis_fail(message); }

/* Memory (proposals/memory.md). The program's memory is malloc; a new memory
   is an arena of chunks that is freed or cleared all at once. Memories form
   a stack: what is made now goes in the top one. A null ovis_memory is the
   program's. Everything allocated is zeroed. */
typedef struct ovis_chunk {
  struct ovis_chunk *next;
  size_t size;
  size_t used;
} ovis_chunk;

typedef struct ovis_memory {
  ovis_chunk *chunks; /* newest first */
  size_t next_size;
  uint32_t id; /* for the checked table: values name the memory they are in */
  struct ovis_memory *next_arena; /* every live arena, for telling values apart */
} ovis_memory;

static ovis_memory *ovis_arenas;
static ovis_memory **ovis_stack;
static size_t ovis_stack_depth, ovis_stack_capacity;

static ovis_memory *ovis_top(void) {
  return ovis_stack_depth ? ovis_stack[ovis_stack_depth - 1] : NULL;
}

/* The memory current outside the innermost `with`. */
static ovis_memory *ovis_outer(void) {
  return ovis_stack_depth > 1 ? ovis_stack[ovis_stack_depth - 2] : NULL;
}

static void *ovis_alloc_raw_in(ovis_memory *memory, size_t size) {
  if (!memory) {
    void *result = calloc(1, size ? size : 1);
    if (!result)
      ovis_fail("out of memory");
    return result;
  }
  size = (size + 15) & ~(size_t)15;
  ovis_chunk *chunk = memory->chunks;
  if (!chunk || chunk->size - chunk->used < size) {
    size_t room = memory->next_size ? memory->next_size : 4096;
    while (room < size)
      room *= 2;
    memory->next_size = room * 2;
    chunk = calloc(1, sizeof(ovis_chunk) + 16 + room);
    if (!chunk)
      ovis_fail("out of memory");
    chunk->size = room;
    chunk->next = memory->chunks;
    memory->chunks = chunk;
  }
  /* The data starts 16 bytes after the header, so it is aligned for any C
     type. */
  void *result = (char *)(chunk + 1) + 16 + chunk->used;
  chunk->used += size;
#ifdef OVIS_CHECKED
  /* Cleared memory is poisoned (below); what is made from it starts empty. */
  memset(result, 0, size);
#endif
  return result;
}

/* What kind of value an allocation is, for messages. */
enum { OVIS_TEXT, OVIS_LIST, OVIS_RECORD, OVIS_OTHER, OVIS_MAP, OVIS_ROOM };

#ifdef OVIS_CHECKED
/* ---- the checked table: every value made, by its address ---- */
typedef struct {
  const void *start; /* NULL: empty; 1: a removed entry */
  size_t size;
  int64_t made_line, freed_line;
  uint32_t arena, generation;
  uint8_t kind;
  /* The variable it was last stored in, for messages; NULL if none. */
  const char *name;
} ovis_entry;

static ovis_entry *ovis_table;
static size_t ovis_table_capacity, ovis_table_used;

typedef struct {
  uint32_t generation;
  int alive;
  int64_t freed_line;
} ovis_arena_info;
static ovis_arena_info *ovis_arena_infos;
static uint32_t ovis_arena_count; /* ids start at 1; 0 is the program's memory */

static size_t ovis_slot(const void *start, size_t capacity) {
  return (size_t)(((uint64_t)(uintptr_t)start >> 4) * 0x9e3779b97f4a7c15ULL >> 24) &
         (capacity - 1);
}

static ovis_entry *ovis_find(const void *start) {
  if (!ovis_table_capacity)
    return NULL;
  for (size_t at = ovis_slot(start, ovis_table_capacity); ovis_table[at].start;
       at = (at + 1) & (ovis_table_capacity - 1))
    if (ovis_table[at].start == start)
      return &ovis_table[at];
  return NULL;
}

static void ovis_grow_table(void) {
  ovis_entry *old = ovis_table;
  size_t old_capacity = ovis_table_capacity;
  ovis_table_capacity = old_capacity ? old_capacity * 2 : 1024;
  ovis_table = calloc(ovis_table_capacity, sizeof(ovis_entry));
  if (!ovis_table)
    ovis_fail("out of memory");
  ovis_table_used = 0;
  for (size_t index = 0; index < old_capacity; ++index)
    if (old[index].start > (const void *)1) {
      size_t at = ovis_slot(old[index].start, ovis_table_capacity);
      while (ovis_table[at].start)
        at = (at + 1) & (ovis_table_capacity - 1);
      ovis_table[at] = old[index];
      ++ovis_table_used;
    }
  free(old);
}

static void ovis_register(const void *start, size_t size, uint32_t arena, int kind) {
  if ((ovis_table_used + 1) * 2 > ovis_table_capacity)
    ovis_grow_table();
  size_t at = ovis_slot(start, ovis_table_capacity);
  while (ovis_table[at].start > (const void *)1 && ovis_table[at].start != start)
    at = (at + 1) & (ovis_table_capacity - 1);
  if (ovis_table[at].start != start)
    ++ovis_table_used;
  ovis_table[at] = (ovis_entry){start, size, ovis_line, 0, arena,
                                arena ? ovis_arena_infos[arena].generation : 0,
                                (uint8_t)kind, NULL};
}

static const char *ovis_kind_name(int kind) {
  return kind == OVIS_TEXT ? "text" : kind == OVIS_LIST ? "list"
         : kind == OVIS_RECORD ? "record" : kind == OVIS_MAP ? "map"
         : kind == OVIS_ROOM ? "room for items" : "value";
}

/* "this text" or "the text in 'line'". */
static const char *ovis_described(const ovis_entry *entry) {
  static char text[96];
  if (entry->name)
    snprintf(text, sizeof text, "the %s in '%s'", ovis_kind_name(entry->kind),
             entry->name);
  else
    snprintf(text, sizeof text, "this %s", ovis_kind_name(entry->kind));
  return text;
}

/* Whether a value is still there; says why not, naming the lines. Values the
   table doesn't know (text written in the program, memory C made) are fine. */
static void ovis_check_live(const void *pointer) {
  const ovis_entry *entry = ovis_find(pointer);
  if (!entry)
    return;
  char message[256];
  if (entry->freed_line) {
    snprintf(message, sizeof message,
             "%s was freed on line %lld; it was made on line %lld",
             ovis_described(entry), (long long)entry->freed_line,
             (long long)entry->made_line);
    ovis_fail(message);
  }
  if (entry->arena) {
    const ovis_arena_info *info = &ovis_arena_infos[entry->arena];
    if (!info->alive || info->generation != entry->generation) {
      snprintf(message, sizeof message,
               "%s was made on line %lld in a memory that was freed or "
               "cleared on line %lld",
               ovis_described(entry), (long long)entry->made_line,
               (long long)info->freed_line);
      ovis_fail(message);
    }
  }
}

/* Freed values are poisoned and held back for a while before the real free,
   so a stale pointer is still recognised. */
#define OVIS_DELAY 512
static void *ovis_delayed[OVIS_DELAY];
static size_t ovis_delayed_next;

static void ovis_delay_free(void *pointer) {
  void *old = ovis_delayed[ovis_delayed_next];
  if (old) {
    ovis_entry *entry = ovis_find(old);
    if (entry)
      entry->start = (const void *)1; /* forgotten */
    free(old);
  }
  ovis_delayed[ovis_delayed_next] = pointer;
  ovis_delayed_next = (ovis_delayed_next + 1) % OVIS_DELAY;
}

/* Marks a value freed. Returns whether the table knew it. */
static int ovis_release(void *pointer, int kind) {
  ovis_entry *entry = ovis_find(pointer);
  if (!entry) {
    /* Not the start of anything Ovis made: inside one is a mistake. */
    for (size_t index = 0; index < ovis_table_capacity; ++index) {
      const ovis_entry *other = &ovis_table[index];
      if (other->start > (const void *)1 && !other->freed_line &&
          (const char *)pointer > (const char *)other->start &&
          (const char *)pointer < (const char *)other->start + other->size) {
        char message[200];
        snprintf(message, sizeof message,
                 "free was given a place inside a %s made on line %lld, not "
                 "the value itself",
                 ovis_kind_name(other->kind), (long long)other->made_line);
        ovis_fail(message);
      }
    }
    return 0;
  }
  if (entry->freed_line)
    ovis_fail_with("this was already freed on line %lld (it was made on line %lld)",
                   (long long)entry->freed_line, (long long)entry->made_line);
  (void)kind;
  entry->freed_line = ovis_line ? ovis_line : -1;
  memset(pointer, 0xdd, entry->size);
  if (!entry->arena)
    ovis_delay_free(pointer);
  return 1;
}

/* At exit: what the program's memory still holds, by the line that made it. */
static void ovis_report_leaks(void) {
  size_t lines = 0;
  struct {
    int64_t line;
    size_t count;
  } *report = NULL;
  for (size_t index = 0; index < ovis_table_capacity; ++index) {
    const ovis_entry *entry = &ovis_table[index];
    if (entry->start <= (const void *)1 || entry->arena || entry->freed_line)
      continue;
    size_t at = 0;
    while (at < lines && report[at].line != entry->made_line)
      ++at;
    if (at == lines) {
      report = realloc(report, (lines + 1) * sizeof *report);
      report[lines].line = entry->made_line;
      report[lines].count = 0;
      ++lines;
    }
    ++report[at].count;
  }
  for (size_t first = 0; first < lines; ++first)
    for (size_t second = first + 1; second < lines; ++second)
      if (report[second].line < report[first].line) {
        __typeof__(*report) swap = report[first];
        report[first] = report[second];
        report[second] = swap;
      }
  fflush(stdout);
  for (size_t index = 0; index < lines; ++index)
    fprintf(stderr, "ovis: line %lld made %zu value%s that %s never freed\n",
            (long long)report[index].line, report[index].count,
            report[index].count == 1 ? "" : "s",
            report[index].count == 1 ? "was" : "were");
  free(report);
}
#define OVIS_LIVE(pointer) ovis_check_live(pointer)
#else
#define OVIS_LIVE(pointer) ((void)0)
#endif

/* Allocates and, in a checked build, notes the value in the table. */
static void *ovis_alloc_in_kind(ovis_memory *memory, size_t size, int kind) {
  void *result = ovis_alloc_raw_in(memory, size);
#ifdef OVIS_CHECKED
  ovis_register(result, size ? size : 1, memory ? memory->id : 0, kind);
#else
  (void)kind;
#endif
  return result;
}

static void *ovis_alloc_in(ovis_memory *memory, size_t size) {
  return ovis_alloc_in_kind(memory, size, OVIS_OTHER);
}

void *ovis_alloc(int64_t size) { return ovis_alloc_in(ovis_top(), (size_t)size); }

/* Whether a pointer lies in the arena's chunks. */
static int ovis_in_arena(const ovis_memory *memory, const void *pointer) {
  for (const ovis_chunk *chunk = memory->chunks; chunk; chunk = chunk->next) {
    const char *begin = (const char *)(chunk + 1) + 16;
    if ((const char *)pointer >= begin && (const char *)pointer < begin + chunk->size)
      return 1;
  }
  return 0;
}

static int ovis_in_any_arena(const void *pointer) {
  for (const ovis_memory *memory = ovis_arenas; memory; memory = memory->next_arena)
    if (ovis_in_arena(memory, pointer))
      return 1;
  return 0;
}

void *ovis_memory_new(void) {
  ovis_memory *memory = calloc(1, sizeof(ovis_memory));
  if (!memory)
    ovis_fail("out of memory");
  memory->next_arena = ovis_arenas;
  ovis_arenas = memory;
#ifdef OVIS_CHECKED
  ovis_arena_infos = realloc(ovis_arena_infos,
                             (ovis_arena_count + 2) * sizeof(ovis_arena_info));
  memory->id = ++ovis_arena_count;
  ovis_arena_infos[memory->id] = (ovis_arena_info){1, 1, 0};
#endif
  return memory;
}

static void ovis_memory_release_chunks(ovis_memory *memory, int keep_one) {
  ovis_chunk *chunk = memory->chunks;
  memory->chunks = NULL;
  while (chunk) {
    ovis_chunk *next = chunk->next;
    if (keep_one && !memory->chunks) {
      /* The newest chunk is the biggest: keep it, empty, for reuse. */
      memset((char *)(chunk + 1) + 16,
#ifdef OVIS_CHECKED
             0xdd,
#else
             0,
#endif
             chunk->used);
      chunk->used = 0;
      chunk->next = NULL;
      memory->chunks = chunk;
    } else {
      free(chunk);
    }
    chunk = next;
  }
}

void ovis_memory_clear(void *pointer) {
  if (!pointer)
    ovis_fail("the memory was not made (`new memory`)");
#ifdef OVIS_CHECKED
  ovis_arena_info *info = &ovis_arena_infos[((ovis_memory *)pointer)->id];
  ++info->generation;
  info->freed_line = ovis_line;
#endif
  ovis_memory_release_chunks(pointer, 1);
}

void ovis_memory_free(void *pointer) {
  ovis_memory *memory = pointer;
  if (!memory)
    return;
  for (size_t index = 0; index < ovis_stack_depth; ++index)
    if (ovis_stack[index] == memory)
      ovis_fail("a memory was freed while it is the one in use");
#ifdef OVIS_CHECKED
  ovis_arena_infos[memory->id].alive = 0;
  ovis_arena_infos[memory->id].freed_line = ovis_line;
#endif
  ovis_memory_release_chunks(memory, 0);
  for (ovis_memory **link = &ovis_arenas; *link; link = &(*link)->next_arena)
    if (*link == memory) {
      *link = memory->next_arena;
      break;
    }
  free(memory);
}

void ovis_memory_push(void *memory) {
  if (ovis_stack_depth == ovis_stack_capacity) {
    ovis_stack_capacity = ovis_stack_capacity ? ovis_stack_capacity * 2 : 8;
    ovis_stack = realloc(ovis_stack, ovis_stack_capacity * sizeof(ovis_memory *));
    if (!ovis_stack)
      ovis_fail("out of memory");
  }
  ovis_stack[ovis_stack_depth++] = memory;
}

void ovis_memory_pop(void) {
  if (ovis_stack_depth)
    --ovis_stack_depth;
}

/* The program's string constants are in a section of their own, so `free`
   leaves them alone. */
#if defined(__APPLE__)
extern const char ovis_const_begin[] __asm("section$start$__TEXT$__ovis_const");
extern const char ovis_const_end[] __asm("section$end$__TEXT$__ovis_const");
#else
extern const char __start_ovis_const[] __attribute__((weak));
extern const char __stop_ovis_const[] __attribute__((weak));
#define ovis_const_begin __start_ovis_const
#define ovis_const_end __stop_ovis_const
#endif

static int ovis_is_constant(const void *pointer) {
  return (const char *)pointer >= ovis_const_begin &&
         (const char *)pointer < ovis_const_end;
}

/* Text is freed unless it is a constant or lives in a new memory (which
   frees it with everything else). Anything else was made by Ovis or C and
   goes to free. */
void ovis_free_text(char *text) {
  if (!text || ovis_is_constant(text))
    return;
#ifdef OVIS_CHECKED
  if (ovis_release(text, OVIS_TEXT))
    return;
#endif
  if (ovis_in_any_arena(text))
    return;
  free(text);
}

void ovis_free_record(void *record) {
  if (!record)
    return;
#ifdef OVIS_CHECKED
  if (ovis_release(record, OVIS_RECORD))
    return;
#endif
  if (ovis_in_any_arena(record))
    return;
  free(record);
}

typedef struct {
  int64_t length;
  int64_t capacity;
  char *data;
  ovis_memory *memory; /* where the list and its storage were made */
} ovis_list;

void ovis_free_pointer(void *pointer) {
#ifdef OVIS_CHECKED
  /* A pointer Ovis made is freed as the value it is; others are C's. */
  if (pointer && ovis_find(pointer)) {
    ovis_release(pointer, OVIS_OTHER);
    return;
  }
#endif
  free(pointer);
}

/* A list is freed whole, not its items. */
void ovis_free_list(void *pointer) {
  ovis_list *list = pointer;
  if (!list)
    return;
#ifdef OVIS_CHECKED
  /* The storage isn't in the table: it goes at once, the header is held. */
  const ovis_entry *entry = ovis_find(list);
  if (entry && entry->freed_line)
    ovis_release(list, OVIS_LIST); /* the double free is reported */
  /* A view's items are C's (capacity -1): only the header is Ovis's. */
  if (!list->memory && list->capacity >= 0) {
    free(list->data);
    list->data = NULL;
    list->length = 0;
  }
  if (ovis_release(list, OVIS_LIST))
    return;
#endif
  if (list->memory)
    return;
  if (list->capacity >= 0)
    free(list->data);
  free(list);
}

void *ovis_list_new(int64_t element_size, int64_t count);

/* ---- maps: keys (whole numbers, decimals or text) to values ----
   Entries are kept in the order they were added, so a map prints and goes
   through its keys the same way every time; an open-addressing index finds
   them. Every value is 8 bytes (a number, a decimal's bits, a pointer). */
typedef struct {
  int64_t count;    /* live entries */
  int64_t used;     /* entries used, removed ones too */
  int64_t capacity; /* entries room */
  int64_t slots;    /* index size, a power of two */
  int64_t key_kind; /* OVIS_KEY_NUMBER, OVIS_KEY_TEXT or OVIS_KEY_DECIMAL */
  uint64_t *keys;
  uint64_t *values;
  uint8_t *alive;
  int64_t *index; /* -1 empty, -2 removed, else an entry */
  ovis_memory *memory;
} ovis_map;

enum { OVIS_KEY_NUMBER, OVIS_KEY_TEXT, OVIS_KEY_DECIMAL };

static uint64_t ovis_key_hash(const ovis_map *map, uint64_t key) {
  uint64_t hash = 14695981039346656037ULL;
  if (map->key_kind == OVIS_KEY_TEXT) {
    for (const unsigned char *at = (const unsigned char *)(uintptr_t)key; at && *at; ++at)
      hash = (hash ^ *at) * 1099511628211ULL;
    return hash;
  }
  hash ^= key;
  hash *= 0x9e3779b97f4a7c15ULL;
  return hash ^ (hash >> 29);
}

static int ovis_key_equal(const ovis_map *map, uint64_t left, uint64_t right) {
  /* A decimal is its bits: the compiler makes -0.0 into 0.0. */
  if (map->key_kind != OVIS_KEY_TEXT)
    return left == right;
  const char *a = (const char *)(uintptr_t)left;
  const char *b = (const char *)(uintptr_t)right;
  return a == b || (a && b && strcmp(a, b) == 0);
}

static void *ovis_map_raw(ovis_map *map, size_t size) {
  return ovis_alloc_raw_in(map->memory, size);
}

static void ovis_map_release_raw(ovis_map *map, void *pointer) {
  if (!map->memory)
    free(pointer);
}

static void *ovis_map_new_in(ovis_memory *memory, int64_t key_kind) {
  ovis_map *map = ovis_alloc_in_kind(memory, sizeof(ovis_map), OVIS_MAP);
  map->memory = memory;
  map->key_kind = key_kind;
  map->capacity = 8;
  map->slots = 16;
  map->keys = ovis_map_raw(map, sizeof(uint64_t) * 8);
  map->values = ovis_map_raw(map, sizeof(uint64_t) * 8);
  map->alive = ovis_map_raw(map, 8);
  map->index = ovis_map_raw(map, sizeof(int64_t) * 16);
  for (int64_t at = 0; at < 16; ++at)
    map->index[at] = -1;
  return map;
}

void *ovis_map_new(int64_t key_kind) {
  return ovis_map_new_in(ovis_top(), key_kind);
}

/* Where the key's entry is in the index, or where it would go. */
static int64_t ovis_map_find(const ovis_map *map, uint64_t key, int *found) {
  int64_t free_slot = -1;
  for (int64_t at = (int64_t)(ovis_key_hash(map, key) & (uint64_t)(map->slots - 1));;
       at = (at + 1) & (map->slots - 1)) {
    const int64_t entry = map->index[at];
    if (entry == -1) {
      *found = 0;
      return free_slot >= 0 ? free_slot : at;
    }
    if (entry == -2) {
      if (free_slot < 0)
        free_slot = at;
      continue;
    }
    if (ovis_key_equal(map, map->keys[entry], key)) {
      *found = 1;
      return at;
    }
  }
}

/* Room for one more entry: the entries packed (removed ones dropped) and
   the index rebuilt when it fills. */
static void ovis_map_grow(ovis_map *map) {
  if (map->used < map->capacity && (map->used + 1) * 2 <= map->slots)
    return;
  int64_t capacity = map->count * 2 > 8 ? map->count * 2 : 8;
  uint64_t *keys = ovis_map_raw(map, sizeof(uint64_t) * (size_t)capacity);
  uint64_t *values = ovis_map_raw(map, sizeof(uint64_t) * (size_t)capacity);
  uint8_t *alive = ovis_map_raw(map, (size_t)capacity);
  int64_t next = 0;
  for (int64_t entry = 0; entry < map->used; ++entry)
    if (map->alive[entry]) {
      keys[next] = map->keys[entry];
      values[next] = map->values[entry];
      alive[next] = 1;
      ++next;
    }
  ovis_map_release_raw(map, map->keys);
  ovis_map_release_raw(map, map->values);
  ovis_map_release_raw(map, map->alive);
  map->keys = keys;
  map->values = values;
  map->alive = alive;
  map->capacity = capacity;
  map->used = next;
  int64_t slots = 16;
  while (slots < capacity * 2)
    slots *= 2;
  ovis_map_release_raw(map, map->index);
  map->index = ovis_map_raw(map, sizeof(int64_t) * (size_t)slots);
  map->slots = slots;
  for (int64_t at = 0; at < slots; ++at)
    map->index[at] = -1;
  for (int64_t entry = 0; entry < next; ++entry) {
    int found = 0;
    map->index[ovis_map_find(map, keys[entry], &found)] = entry;
  }
}

void ovis_map_put(void *pointer, uint64_t key, uint64_t value) {
  ovis_map *map = pointer;
  OVIS_LIVE(map);
  int found = 0;
  int64_t at = ovis_map_find(map, key, &found);
  if (found) {
    map->values[map->index[at]] = value;
    return;
  }
  ovis_map_grow(map);
  at = ovis_map_find(map, key, &found);
  const int64_t entry = map->used++;
  map->keys[entry] = key;
  map->values[entry] = value;
  map->alive[entry] = 1;
  map->index[at] = entry;
  ++map->count;
}

static void ovis_map_missing(const ovis_map *map, uint64_t key) {
  if (map->key_kind == OVIS_KEY_DECIMAL) {
    double decimal;
    memcpy(&decimal, &key, sizeof decimal);
    char message[200];
    snprintf(message, sizeof message, "the map has no entry for %g", decimal);
    ovis_fail(message);
  }
  if (map->key_kind == OVIS_KEY_TEXT) {
    char message[200];
    snprintf(message, sizeof message, "the map has no entry for \"%.150s\"",
             (const char *)(uintptr_t)key);
    ovis_fail(message);
  }
  ovis_fail_with("the map has no entry for %lld%.0lld", (long long)key, 0);
}

uint64_t ovis_map_get(void *pointer, uint64_t key) {
  ovis_map *map = pointer;
  OVIS_LIVE(map);
  int found = 0;
  const int64_t at = ovis_map_find(map, key, &found);
  if (!found)
    ovis_map_missing(map, key);
  return map->values[map->index[at]];
}

int32_t ovis_map_has(void *pointer, uint64_t key) {
  ovis_map *map = pointer;
  OVIS_LIVE(map);
  int found = 0;
  ovis_map_find(map, key, &found);
  return found;
}

void ovis_map_remove(void *pointer, uint64_t key) {
  ovis_map *map = pointer;
  OVIS_LIVE(map);
  int found = 0;
  const int64_t at = ovis_map_find(map, key, &found);
  if (!found)
    return;
  map->alive[map->index[at]] = 0;
  map->index[at] = -2;
  --map->count;
}

int64_t ovis_map_len(void *pointer) {
  OVIS_LIVE(pointer);
  return ((ovis_map *)pointer)->count;
}

/* Going through the entries in order: how many places, whether one is an
   entry, its key and value. */
int64_t ovis_map_places(void *pointer) {
  OVIS_LIVE(pointer);
  return ((ovis_map *)pointer)->used;
}
int32_t ovis_map_alive(void *pointer, int64_t at) {
  return ((ovis_map *)pointer)->alive[at];
}
uint64_t ovis_map_key_at(void *pointer, int64_t at) {
  return ((ovis_map *)pointer)->keys[at];
}
uint64_t ovis_map_value_at(void *pointer, int64_t at) {
  return ((ovis_map *)pointer)->values[at];
}
void ovis_map_set_value_at(void *pointer, int64_t at, uint64_t value) {
  ((ovis_map *)pointer)->values[at] = value;
}
void ovis_map_set_key_at(void *pointer, int64_t at, uint64_t key) {
  ((ovis_map *)pointer)->keys[at] = key;
}

/* The keys, in order, as a new list. */
void *ovis_map_keys(void *pointer) {
  ovis_map *map = pointer;
  OVIS_LIVE(map);
  ovis_list *list = ovis_list_new(8, map->count);
  int64_t next = 0;
  for (int64_t entry = 0; entry < map->used; ++entry)
    if (map->alive[entry])
      ((uint64_t *)list->data)[next++] = map->keys[entry];
  return list;
}

/* A copy of the map in the current memory (its keys and values as they
   are: `keep` and `a copy of` copy those). */
void *ovis_map_copy(void *pointer) {
  ovis_map *map = pointer;
  OVIS_LIVE(map);
  ovis_map *copy = ovis_map_new(map->key_kind);
  for (int64_t entry = 0; entry < map->used; ++entry)
    if (map->alive[entry])
      ovis_map_put(copy, map->keys[entry], map->values[entry]);
  return copy;
}

/* Freed whole, not its keys and values (they may be shared). */
void ovis_free_map(void *pointer) {
  ovis_map *map = pointer;
  if (!map)
    return;
#ifdef OVIS_CHECKED
  const ovis_entry *entry = ovis_find(map);
  if (entry && entry->freed_line) {
    ovis_release(map, OVIS_MAP); /* the double free is reported */
    return;
  }
#endif
  if (!map->memory) {
    free(map->keys);
    free(map->values);
    free(map->alive);
    free(map->index);
  }
#ifdef OVIS_CHECKED
  if (ovis_release(map, OVIS_MAP))
    return;
#endif
  if (!map->memory)
    free(map);
}

/* `the 100 int32s at p`: a C array over memory C has (references.md §4).
   Its items are checked against the count; it can't grow. */
void *ovis_list_view(void *data, int64_t count) {
  if (count < 0)
    ovis_fail("an array cannot have a negative size");
  if (!data && count > 0)
    ovis_fail("an array was asked for at a pointer that is nothing");
  ovis_list *list = ovis_alloc_in_kind(ovis_top(), sizeof(ovis_list), OVIS_LIST);
  list->memory = ovis_top();
  list->length = count;
  list->capacity = -1;
  list->data = data;
  return list;
}

/* The items, for C: valid until the list grows. */
void *ovis_list_data(void *pointer) {
  ovis_list *list = pointer;
  OVIS_LIVE(list);
  return list ? list->data : NULL;
}

/* splitmix64: a good 64-bit generator from one number, seeded from the clock
   and the process, so two runs in the same second differ. */
static uint64_t ovis_random_state;

static uint64_t ovis_next_random(void) {
  uint64_t z = (ovis_random_state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

void ovis_init(void) {
#if defined(OVIS_CHECKED) && defined(OVIS_REPORT_LEAKS)
  atexit(ovis_report_leaks);
#endif
  srand((unsigned)time(NULL));
  ovis_random_state = ((uint64_t)time(NULL) << 20) ^ (uint64_t)clock() ^
                      ((uint64_t)(uintptr_t)&ovis_random_state);
}

/* `keep`: copies of what the innermost `with` made, in the memory outside it
   (the program's, outside any `with`). Each value is copied once, so sharing
   and cycles survive: the map goes from what was copied to its copy. */
typedef struct {
  const void *from;
  void *to;
} ovis_pair;
static ovis_pair *ovis_pairs;
static size_t ovis_pair_capacity, ovis_pair_count;
static int ovis_keep_depth;

/* `a copy of`: the same walk, but of everything, into the current memory. */
static int ovis_copying;

static ovis_memory *ovis_keep_target(void) {
  return ovis_copying ? ovis_top() : ovis_outer();
}

void ovis_keep_begin(void) {
  if (ovis_keep_depth++ == 0) {
    memset(ovis_pairs, 0, ovis_pair_capacity * sizeof(ovis_pair));
    ovis_pair_count = 0;
  }
}

void ovis_keep_end(void) { --ovis_keep_depth; }

void ovis_copy_begin(void) {
  ovis_keep_begin();
  ovis_copying = 1;
}

void ovis_copy_end(void) {
  ovis_copying = 0;
  ovis_keep_end();
}

static size_t ovis_pair_slot(const void *from, size_t capacity) {
  return (size_t)(((uint64_t)(uintptr_t)from >> 4) * 0x9e3779b97f4a7c15ULL >> 20) &
         (capacity - 1);
}

void *ovis_keep_seen(const void *from) {
  if (!ovis_pair_capacity)
    return NULL;
  for (size_t at = ovis_pair_slot(from, ovis_pair_capacity);
       ovis_pairs[at].from; at = (at + 1) & (ovis_pair_capacity - 1))
    if (ovis_pairs[at].from == from)
      return ovis_pairs[at].to;
  return NULL;
}

static void ovis_keep_note(const void *from, void *to) {
  if ((ovis_pair_count + 1) * 2 > ovis_pair_capacity) {
    ovis_pair *old = ovis_pairs;
    size_t old_capacity = ovis_pair_capacity;
    ovis_pair_capacity = old_capacity ? old_capacity * 2 : 64;
    ovis_pairs = calloc(ovis_pair_capacity, sizeof(ovis_pair));
    if (!ovis_pairs)
      ovis_fail("out of memory");
    for (size_t index = 0; index < old_capacity; ++index)
      if (old[index].from) {
        size_t at = ovis_pair_slot(old[index].from, ovis_pair_capacity);
        while (ovis_pairs[at].from)
          at = (at + 1) & (ovis_pair_capacity - 1);
        ovis_pairs[at] = old[index];
      }
    free(old);
  }
  size_t at = ovis_pair_slot(from, ovis_pair_capacity);
  while (ovis_pairs[at].from)
    at = (at + 1) & (ovis_pair_capacity - 1);
  ovis_pairs[at].from = from;
  ovis_pairs[at].to = to;
  ++ovis_pair_count;
}

/* Whether a value was made in the memory `keep` leaves. */
int32_t ovis_keep_owned(const void *pointer) {
  if (!pointer)
    return 0;
  if (ovis_copying)
    return 1;
  ovis_memory *top = ovis_top();
  if (!top)
    return !ovis_is_constant(pointer);
  return ovis_in_arena(top, pointer);
}

char *ovis_keep_text(char *text) {
  if (!ovis_keep_owned(text))
    return text;
  char *seen = ovis_keep_seen(text);
  if (seen)
    return seen;
  size_t length = strlen(text);
  char *copy = ovis_alloc_in_kind(ovis_keep_target(), length + 1, OVIS_TEXT);
  memcpy(copy, text, length + 1);
  ovis_keep_note(text, copy);
  return copy;
}

void *ovis_keep_copy(void *record, int64_t size) {
  void *copy = ovis_alloc_in_kind(ovis_keep_target(), (size_t)size, OVIS_RECORD);
  memcpy(copy, record, (size_t)size);
  ovis_keep_note(record, copy);
  return copy;
}

void *ovis_keep_list(void *pointer, int64_t element_size) {
  ovis_list *list = pointer;
  if (!ovis_keep_owned(list))
    return list;
  ovis_list *seen = ovis_keep_seen(list);
  if (seen)
    return seen;
  ovis_memory *outer = ovis_keep_target();
  ovis_list *copy = ovis_alloc_in_kind(outer, sizeof(ovis_list), OVIS_LIST);
  copy->memory = outer;
  copy->length = list->length;
  copy->capacity = list->length > 4 ? list->length : 4;
  copy->data = ovis_alloc_raw_in(outer, (size_t)(copy->capacity * element_size));
  memcpy(copy->data, list->data, (size_t)(list->length * element_size));
  ovis_keep_note(list, copy);
  return copy;
}

/* A map, kept: its entries copied (their values are kept by the caller,
   which knows their type). */
void *ovis_keep_map(void *pointer) {
  ovis_map *map = pointer;
  if (!ovis_keep_owned(map))
    return map;
  ovis_map *seen = ovis_keep_seen(map);
  if (seen)
    return seen;
  ovis_map *copy = ovis_map_new_in(ovis_keep_target(), map->key_kind);
  for (int64_t entry = 0; entry < map->used; ++entry)
    if (map->alive[entry])
      ovis_map_put(copy, map->keys[entry], map->values[entry]);
  ovis_keep_note(map, copy);
  return copy;
}

/* The items of a list of text that was copied are kept too. */
void ovis_keep_list_text(void *original, void *copied) {
  if (original == copied)
    return;
  ovis_list *list = copied;
  for (int64_t index = 0; index < list->length; ++index)
    ((char **)list->data)[index] = ovis_keep_text(((char **)list->data)[index]);
}

void *ovis_list_new(int64_t element_size, int64_t count) {
  if (count < 0)
    ovis_fail("a list cannot have a negative size");
  ovis_memory *memory = ovis_top();
  ovis_list *list = ovis_alloc_in_kind(memory, sizeof(ovis_list), OVIS_LIST);
  list->memory = memory;
  list->length = count;
  list->capacity = count > 4 ? count : 4;
  list->data = ovis_alloc_raw_in(memory, (size_t)(list->capacity * element_size));
  return list;
}

int64_t ovis_list_len(void *pointer) {
  OVIS_LIVE(pointer);
  return ((ovis_list *)pointer)->length;
}

void *ovis_list_at(void *pointer, int64_t index, int64_t element_size) {
  ovis_list *list = pointer;
  OVIS_LIVE(list);
  if (index < 0 || index >= list->length)
    ovis_fail_with("index %lld is outside the list (it has %lld items, the "
                   "first one is at index 0)",
                   (long long)index, (long long)list->length);
  return list->data + index * element_size;
}

void *ovis_list_push(void *pointer, int64_t element_size) {
  ovis_list *list = pointer;
  OVIS_LIVE(list);
  if (list->capacity < 0)
    ovis_fail("an array over C's memory can't grow");
  if (list->length == list->capacity) {
    /* The storage grows in the memory the list was made in. */
    int64_t grown = list->capacity * 2;
    if (list->memory) {
      char *bigger = ovis_alloc_raw_in(list->memory, (size_t)(grown * element_size));
      memcpy(bigger, list->data, (size_t)(list->length * element_size));
      list->data = bigger;
    } else {
      list->data = realloc(list->data, (size_t)(grown * element_size));
      if (!list->data)
        ovis_fail("out of memory");
      /* realloc doesn't zero the new half. */
      memset(list->data + list->capacity * element_size, 0,
             (size_t)((grown - list->capacity) * element_size));
    }
    list->capacity = grown;
  }
  return list->data + list->length++ * element_size;
}

void ovis_overflow(const char *what) {
  char message[160];
  snprintf(message, sizeof message, "%s got too big (or too small) to keep",
           what);
  ovis_fail(message);
}

void ovis_check_divisor(int64_t divisor) {
  if (divisor == 0)
    ovis_fail("division by zero");
}

void ovis_print_int(int64_t value) { printf("%lld", (long long)value); }
void ovis_print_uint(uint64_t value) { printf("%llu", (unsigned long long)value); }

/* Numbers of a size (proposals/numbers.md). A value that doesn't fit the
   type it is put in: shown as a signed or unsigned whole number, or (2) a
   decimal's bits. */
void ovis_narrow_failed(int64_t value, int32_t kind, const char *type) {
  char message[256];
  if (kind == 2) {
    double real;
    memcpy(&real, &value, sizeof real);
    snprintf(message, sizeof message, "%.15g doesn't fit in %s", real, type);
  } else if (kind == 1) {
    snprintf(message, sizeof message, "%llu doesn't fit in %s",
             (unsigned long long)value, type);
  } else {
    snprintf(message, sizeof message, "%lld doesn't fit in %s",
             (long long)value, type);
  }
  ovis_fail(message);
}

void ovis_check_bit_place(int64_t place, int64_t bits) {
  if (place < 0 || place >= bits)
    ovis_fail_with("there is no bit %lld in a number of %lld bits", place, bits);
}

void ovis_check_bit_range(int64_t low, int64_t high, int64_t bits) {
  ovis_check_bit_place(low, bits);
  ovis_check_bit_place(high, bits);
  if (low > high)
    ovis_fail_with("bits %lld to %lld: the first is after the last", low, high);
}

/* A float32 as the fewest digits that read back as it. */
static void ovis_float32_text(char *buffer, size_t size, float value) {
  double wide = value;
  if (wide == floor(wide) && fabs(wide) < 1e15) {
    snprintf(buffer, size, "%.1f", wide);
    return;
  }
  for (int precision = 1; precision <= 9; ++precision) {
    snprintf(buffer, size, "%.*g", precision, wide);
    if ((float)strtod(buffer, NULL) == value)
      return;
  }
}

void ovis_print_float32(float value) {
  char buffer[48];
  ovis_float32_text(buffer, sizeof buffer, value);
  fputs(buffer, stdout);
}

void ovis_print_float(double value) {
  if (value == floor(value) && fabs(value) < 1e15)
    printf("%.1f", value);
  else
    printf("%.15g", value);
}

void ovis_print_bool(int32_t value) { fputs(value ? "true" : "false", stdout); }
void ovis_print_str(const char *value) {
  OVIS_LIVE(value);
  fputs(value ? value : "null", stdout);
}
void ovis_print_space(void) { putchar(' '); }
void ovis_print_newline(void) { putchar('\n'); }

void ovis_print_quoted(const char *value) {
  OVIS_LIVE(value);
  printf("\"%s\"", value ? value : "null");
}

void *ovis_record_new(int64_t size) {
  return ovis_alloc_in_kind(ovis_top(), (size_t)size, OVIS_RECORD);
}

/* Checked builds: a value given to a C function must still be there, and a
   value stored in a variable takes its name for messages. */
void ovis_live(void *pointer) {
  if (pointer)
    OVIS_LIVE(pointer);
}

void ovis_name(void *pointer, const char *name) {
#ifdef OVIS_CHECKED
  ovis_entry *entry = pointer ? ovis_find(pointer) : NULL;
  if (entry && !entry->freed_line)
    entry->name = name;
#else
  (void)pointer;
  (void)name;
#endif
}

/* Reading or writing through a pointer: not through null. */
void ovis_pointer_check(void *pointer) {
  if (!pointer)
    ovis_fail("a pointer that is nothing was read or written through");
  OVIS_LIVE(pointer);
}

#ifdef OVIS_CHECKED
/* The rooms made or used last, so checking a read is quick: a loop goes
   through one or two. */
#define OVIS_RECENT_ROOMS 8
static struct {
  const void *start;
  size_t size;
} ovis_recent_rooms[OVIS_RECENT_ROOMS];
static size_t ovis_recent_next;

static void ovis_remember_room(const void *start, size_t size) {
  ovis_recent_rooms[ovis_recent_next].start = start;
  ovis_recent_rooms[ovis_recent_next].size = size;
  ovis_recent_next = (ovis_recent_next + 1) % OVIS_RECENT_ROOMS;
}
#endif

/* Room for items (generics.md §5): `count` items of `size` bytes, zeroed, in
   the current memory. */
void *ovis_alloc_items(int64_t count, int64_t size) {
  if (count < 0)
    ovis_fail_with("room was asked for %lld items (of %lld bytes); there can't "
                   "be fewer than none", (long long)count, (long long)size);
  void *result = ovis_alloc_in_kind(ovis_top(), (size_t)(count * size), OVIS_ROOM);
#ifdef OVIS_CHECKED
  ovis_remember_room(result, (size_t)(count * size) ? (size_t)(count * size) : 1);
#endif
  return result;
}

/* The memory a value was made in: the arena holding it, or the program's. */
static ovis_memory *ovis_memory_of(const void *pointer) {
  for (ovis_memory *memory = ovis_arenas; memory; memory = memory->next_arena)
    if (ovis_in_arena(memory, pointer))
      return memory;
  return NULL;
}

/* The same room grown (or shrunk) to `new_count` items, in the memory it is
   in: its first items copied, the old room freed (an arena's stays until
   the arena goes). */
void *ovis_alloc_more(void *old, int64_t old_count, int64_t new_count,
                      int64_t size) {
  if (new_count < 0)
    ovis_fail_with("room was asked for %lld items (of %lld bytes); there can't "
                   "be fewer than none", (long long)new_count, (long long)size);
  ovis_memory *memory = old ? ovis_memory_of(old) : ovis_top();
#ifdef OVIS_CHECKED
  if (old) {
    const ovis_entry *entry = ovis_find(old);
    if (entry && entry->freed_line)
      ovis_fail_with("this room for items was freed on line %lld (it was made "
                     "on line %lld)",
                     (long long)entry->freed_line, (long long)entry->made_line);
    if (entry && (size_t)(old_count * size) > entry->size)
      ovis_fail_with("this room holds %lld bytes, not the %lld its old items "
                     "would take", (long long)entry->size,
                     (long long)(old_count * size));
  }
#endif
  void *result = ovis_alloc_in_kind(memory, (size_t)(new_count * size), OVIS_ROOM);
#ifdef OVIS_CHECKED
  ovis_remember_room(result,
                     (size_t)(new_count * size) ? (size_t)(new_count * size) : 1);
#endif
  if (old) {
    const int64_t kept = old_count < new_count ? old_count : new_count;
    memcpy(result, old, (size_t)(kept * size));
    if (!memory)
      ovis_free_pointer(old);
  }
  return result;
}

/* Reading or writing `size` bytes at a pointer: not through null and, in a
   checked build, inside room Ovis made that isn't freed. C's memory isn't
   checked: Ovis doesn't know its size. */
void ovis_room_check(void *pointer, int64_t size) {
  if (!pointer)
    ovis_fail("a pointer that is nothing was read or written through");
#ifdef OVIS_CHECKED
  const void *last_start = NULL;
  for (size_t index = 0; index < OVIS_RECENT_ROOMS; ++index) {
    const void *start = ovis_recent_rooms[index].start;
    if (start && (const char *)pointer >= (const char *)start &&
        (const char *)pointer < (const char *)start + ovis_recent_rooms[index].size) {
      last_start = start;
      break;
    }
  }
  const ovis_entry *entry = NULL;
  if (!last_start) {
    for (size_t index = 0; index < ovis_table_capacity; ++index) {
      const ovis_entry *other = &ovis_table[index];
      if (other->start > (const void *)1 &&
          (const char *)pointer >= (const char *)other->start &&
          (const char *)pointer < (const char *)other->start + other->size) {
        entry = other;
        break;
      }
    }
    if (!entry) {
      /* Just past the end of a room is still a mistake, not C's memory. */
      for (size_t index = 0; index < ovis_table_capacity; ++index) {
        const ovis_entry *other = &ovis_table[index];
        if (other->start > (const void *)1 && other->kind == OVIS_ROOM &&
            (const char *)pointer >= (const char *)other->start + other->size &&
            (const char *)pointer < (const char *)other->start + other->size + 64)
          ovis_fail_with("an item was read or written past the end of room "
                         "for %lld bytes made on line %lld",
                         (long long)other->size, (long long)other->made_line);
      }
      return;
    }
    last_start = entry->start;
    if (entry->kind == OVIS_ROOM)
      ovis_remember_room(entry->start, entry->size);
  }
  if (!entry)
    entry = ovis_find(last_start);
  if (!entry || entry->kind != OVIS_ROOM)
    return;
  if (entry->freed_line) {
    ovis_fail_with("this room for items was freed on line %lld (it was made on "
                   "line %lld)",
                   (long long)entry->freed_line, (long long)entry->made_line);
  }
  OVIS_LIVE(entry->start);
  if ((const char *)pointer + size > (const char *)entry->start + entry->size)
    ovis_fail_with("an item was read or written past the end of room for %lld "
                   "bytes made on line %lld",
                   (long long)entry->size, (long long)entry->made_line);
#else
  (void)size;
#endif
}

/* An array of bytes as text: up to its first zero byte (or its end). */
char *ovis_bytes_text(const char *bytes, int64_t length) {
  int64_t end = 0;
  while (end < length && bytes[end])
    ++end;
  char *text = ovis_alloc_in_kind(ovis_top(), (size_t)end + 1, OVIS_TEXT);
  memcpy(text, bytes, (size_t)end);
  text[end] = '\0';
  return text;
}

/* Text into a C struct's `char[N]`: at most N-1 bytes and a NUL. */
void ovis_c_text_set(char *field, int64_t size, const char *text) {
  OVIS_LIVE(text);
  if (size <= 0)
    return;
  size_t length = text ? strlen(text) : 0;
  if (length > (size_t)size - 1)
    length = (size_t)size - 1;
  if (length)
    memcpy(field, text, length);
  field[length] = '\0';
}

void ovis_record_check(void *record) {
  if (!record)
    ovis_fail("a record was used before it was given a value");
  OVIS_LIVE(record);
}

// A value typed as one trait may be asked for another trait's phrase (a
// list of records that are several traits): its table has no entry for a
// record that doesn't implement that trait.
void ovis_method_check(void *method) {
  if (!method)
    ovis_fail("this value does not implement the trait of the phrase "
              "used on it");
}

// `(a) comes before (b)` on two values of a trait: the impl the first picks
// takes the second as its own record. Every record starts with its tag.
void ovis_same_record_check(void *first, void *second) {
  ovis_record_check(second);
  if (*(int64_t *)first != *(int64_t *)second)
    ovis_fail("a phrase of a trait was given two different kinds of record; "
              "it needs two of the same kind");
}

void ovis_print_list(void *pointer, int32_t kind) {
  ovis_list *list = pointer;
  OVIS_LIVE(list);
  putchar('[');
  for (int64_t index = 0; index < list->length; ++index) {
    if (index != 0)
      fputs(", ", stdout);
    switch (kind) {
    case 0:
      ovis_print_int(((int64_t *)list->data)[index]);
      break;
    case 1:
      ovis_print_float(((double *)list->data)[index]);
      break;
    case 2:
      ovis_print_bool(((int8_t *)list->data)[index]);
      break;
    default:
      printf("\"%s\"", ((const char **)list->data)[index]);
    }
  }
  putchar(']');
}

/* After a number, the spaces and the line ending after it, so a line read
   next is the next line; a number further along the line stays. */
static void ovis_skip_line_end(void) {
  int character;
  while ((character = getchar()) == ' ' || character == '\t' ||
         character == '\r') {
  }
  if (character != '\n' && character != EOF)
    ungetc(character, stdin);
}

int64_t ovis_read_int(void) {
  long long value;
  fflush(stdout);
  if (scanf("%lld", &value) != 1)
    ovis_fail("expected a whole number as input");
  ovis_skip_line_end();
  return value;
}

double ovis_read_float(void) {
  double value;
  fflush(stdout);
  if (scanf("%lf", &value) != 1)
    ovis_fail("expected a number as input");
  ovis_skip_line_end();
  return value;
}

int64_t ovis_random(int64_t lowest, int64_t highest) {
  if (lowest > highest) {
    int64_t swap = lowest;
    lowest = highest;
    highest = swap;
  }
  /* The count of values, as unsigned so the whole range fits (0 means 2^64). */
  uint64_t span = (uint64_t)highest - (uint64_t)lowest + 1;
  uint64_t offset = ovis_next_random();
  if (span != 0) {
    /* Throw away the top few values so every result is equally likely. */
    uint64_t limit = UINT64_MAX - UINT64_MAX % span;
    while (offset >= limit)
      offset = ovis_next_random();
    offset %= span;
  }
  return (int64_t)((uint64_t)lowest + offset);
}

int32_t ovis_str_eq(const char *left, const char *right) {
  OVIS_LIVE(left);
  OVIS_LIVE(right);
  if (!left || !right)
    return left == right;
  return strcmp(left, right) == 0;
}

// Text in dictionary order, for `<`; nothing sorts before any text.
int32_t ovis_str_cmp(const char *left, const char *right) {
  OVIS_LIVE(left);
  OVIS_LIVE(right);
  if (!left || !right)
    return (left != 0) - (right != 0);
  const int result = strcmp(left, right);
  return (result > 0) - (result < 0);
}

static char *ovis_alloc_text(size_t length) {
  char *text = ovis_alloc_in_kind(ovis_top(), length + 1, OVIS_TEXT);
  text[length] = '\0';
  return text;
}

char *ovis_concat(const char *left, const char *right) {
  OVIS_LIVE(left);
  OVIS_LIVE(right);
  left = left ? left : "null";
  right = right ? right : "null";
  size_t left_length = strlen(left), right_length = strlen(right);
  char *text = ovis_alloc_text(left_length + right_length);
  memcpy(text, left, left_length);
  memcpy(text + left_length, right, right_length);
  return text;
}

void ovis_format_int(char *buffer, int64_t value) {
  snprintf(buffer, 32, "%lld", (long long)value);
}

void ovis_format_uint(char *buffer, uint64_t value) {
  snprintf(buffer, 32, "%llu", (unsigned long long)value);
}

void ovis_format_float32(char *buffer, float value) {
  ovis_float32_text(buffer, 32, value);
}

void ovis_format_float(char *buffer, double value) {
  if (value == floor(value) && fabs(value) < 1e15)
    snprintf(buffer, 32, "%.1f", value);
  else
    snprintf(buffer, 32, "%.15g", value);
}

/* A join of any number of parts is one text, made in one allocation. */
char *ovis_join(int64_t count, const char **parts) {
  size_t length = 0;
  for (int64_t index = 0; index < count; ++index) {
    OVIS_LIVE(parts[index]);
    if (!parts[index])
      parts[index] = "null";
    length += strlen(parts[index]);
  }
  char *text = ovis_alloc_text(length);
  size_t at = 0;
  for (int64_t index = 0; index < count; ++index) {
    size_t part_length = strlen(parts[index]);
    memcpy(text + at, parts[index], part_length);
    at += part_length;
  }
  return text;
}

char *ovis_int_to_str(int64_t value) {
  char *text = ovis_alloc_text(24);
  snprintf(text, 25, "%lld", (long long)value);
  return text;
}

char *ovis_uint_to_str(uint64_t value) {
  char *text = ovis_alloc_text(24);
  snprintf(text, 25, "%llu", (unsigned long long)value);
  return text;
}

char *ovis_float32_to_str(float value) {
  char *text = ovis_alloc_text(32);
  ovis_float32_text(text, 33, value);
  return text;
}

/* A whole number's bits in hex (4 bits a digit) or binary (1), at least
   `digits` of them. */
char *ovis_digits(uint64_t value, int64_t digits, int64_t bits_per_digit) {
  char reversed[65];
  int length = 0;
  do {
    reversed[length++] = "0123456789ABCDEF"[value & ((1u << bits_per_digit) - 1)];
    value >>= bits_per_digit;
  } while (value != 0);
  if (digits > 64)
    digits = 64;
  while (length < digits)
    reversed[length++] = '0';
  char *text = ovis_alloc_text((size_t)length);
  for (int index = 0; index < length; ++index)
    text[index] = reversed[length - 1 - index];
  text[length] = '\0';
  return text;
}

/* Text's bytes as a new list of bytes (the zero at its end left out). */
void *ovis_text_bytes(const char *text) {
  OVIS_LIVE(text);
  size_t length = text ? strlen(text) : 0;
  ovis_list *list = ovis_list_new(1, (int64_t)length);
  if (length > 0)
    memcpy(list->data, text, length);
  return list;
}

int64_t ovis_from_hex(const char *text) {
  OVIS_LIVE(text);
  if (!text || !*text)
    ovis_fail("there are no hex digits to read");
  const char *at = text;
  if (at[0] == '0' && (at[1] == 'x' || at[1] == 'X'))
    at += 2;
  uint64_t value = 0;
  int digits = 0;
  for (; *at; ++at) {
    char character = *at;
    int digit = character >= '0' && character <= '9'   ? character - '0'
                : character >= 'a' && character <= 'f' ? character - 'a' + 10
                : character >= 'A' && character <= 'F' ? character - 'A' + 10
                                                       : -1;
    if (character == '_')
      continue;
    if (digit < 0) {
      char message[160];
      snprintf(message, sizeof message, "\"%.100s\" is not a hex number", text);
      ovis_fail(message);
    }
    value = value * 16 + (uint64_t)digit;
    if (++digits > 16 || value > (uint64_t)INT64_MAX) {
      char message[160];
      snprintf(message, sizeof message,
               "\"%.100s\" is too big for a whole number", text);
      ovis_fail(message);
    }
  }
  if (digits == 0)
    ovis_fail("there are no hex digits to read");
  return (int64_t)value;
}

char *ovis_float_to_str(double value) {
  char *text = ovis_alloc_text(32);
  if (value == floor(value) && fabs(value) < 1e15)
    snprintf(text, 33, "%.1f", value);
  else
    snprintf(text, 33, "%.15g", value);
  return text;
}

char *ovis_bool_to_str(int32_t value) { return value ? "true" : "false"; }

char *ovis_substring(const char *text, int64_t start, int64_t count) {
  OVIS_LIVE(text);
  int64_t length = text ? (int64_t)strlen(text) : 0;
  if (start < 0)
    start = 0;
  if (start > length)
    start = length;
  if (count < 0)
    count = 0;
  if (count > length - start)
    count = length - start;
  char *result = ovis_alloc_text((size_t)count);
  if (count)
    memcpy(result, text + start, (size_t)count);
  return result;
}

int64_t ovis_char_at(const char *text, int64_t index) {
  OVIS_LIVE(text);
  int64_t length = text ? (int64_t)strlen(text) : 0;
  if (index < 0 || index >= length)
    ovis_fail_with("position %lld is outside the text (it has %lld "
                   "characters, the first one is at position 0)",
                   (long long)index, (long long)length);
  return (unsigned char)text[index];
}

/* Writes one character into text Ovis made (a buffer): the prelude builds
   text in place with it. Checked builds keep it inside the allocation. */
void ovis_set_char(char *text, int64_t index, int64_t code) {
  OVIS_LIVE(text);
#ifdef OVIS_CHECKED
  const ovis_entry *entry = ovis_find(text);
  if (index < 0 || (entry && (size_t)index >= entry->size))
    ovis_fail("a character was written outside its text");
#endif
  text[index] = (char)code;
}

char *ovis_chr(int64_t code) {
  char *text = ovis_alloc_text(1);
  text[0] = (char)code;
  return text;
}

char *ovis_buffer(int64_t size) {
  if (size < 1)
    ovis_fail("a buffer needs room for at least one character");
  return ovis_alloc_in_kind(ovis_top(), (size_t)size, OVIS_TEXT);
}

/* Reads one line without its line break, or returns null at the end. */
char *ovis_read_line(void) {
  fflush(stdout);
  size_t capacity = 64, length = 0;
  char *text = malloc(capacity);
  int character;
  while ((character = getchar()) != EOF && character != '\n') {
    if (length + 1 == capacity)
      text = realloc(text, capacity *= 2);
    text[length++] = (char)character;
  }
  if (character == EOF && length == 0) {
    free(text);
    return NULL;
  }
  if (length && text[length - 1] == '\r')
    --length;
  text[length] = '\0';
  /* Read into a buffer of its own; the text is made where the program is. */
  char *result = ovis_alloc_in_kind(ovis_top(), length + 1, OVIS_TEXT);
  memcpy(result, text, length + 1);
  free(text);
  return result;
}

static int ovis_compare_int(const void *left, const void *right) {
  int64_t a = *(const int64_t *)left, b = *(const int64_t *)right;
  return (a > b) - (a < b);
}
static int ovis_compare_float(const void *left, const void *right) {
  double a = *(const double *)left, b = *(const double *)right;
  return (a > b) - (a < b);
}
static int ovis_compare_str(const void *left, const void *right) {
  const char *a = *(const char *const *)left, *b = *(const char *const *)right;
  return strcmp(a ? a : "", b ? b : "");
}

/* Sorts from smallest to largest; kind is 0 int, 1 float, 3 str. */
void ovis_list_sort(void *pointer, int32_t kind) {
  ovis_list *list = pointer;
  OVIS_LIVE(list);
  if (kind == 0)
    qsort(list->data, (size_t)list->length, 8, ovis_compare_int);
  else if (kind == 1)
    qsort(list->data, (size_t)list->length, 8, ovis_compare_float);
  else
    qsort(list->data, (size_t)list->length, 8, ovis_compare_str);
}

/* Lists of numbers of a size (proposals/numbers.md): a comparison per C
   number type, by the codes codegen gives. */
#define OVIS_COMPARE(NAME, TYPE)                                              \
  static int NAME(const void *left, const void *right) {                      \
    TYPE a = *(const TYPE *)left, b = *(const TYPE *)right;                   \
    return (a > b) - (a < b);                                                 \
  }
OVIS_COMPARE(ovis_compare_i8, int8_t)
OVIS_COMPARE(ovis_compare_u8, uint8_t)
OVIS_COMPARE(ovis_compare_i16, int16_t)
OVIS_COMPARE(ovis_compare_u16, uint16_t)
OVIS_COMPARE(ovis_compare_i32, int32_t)
OVIS_COMPARE(ovis_compare_u32, uint32_t)
OVIS_COMPARE(ovis_compare_u64, uint64_t)
OVIS_COMPARE(ovis_compare_f32, float)

void ovis_list_sort_number(void *pointer, int32_t code) {
  static int (*const compare[])(const void *, const void *) = {
      ovis_compare_i8,  ovis_compare_u8,  ovis_compare_i16, ovis_compare_u16,
      ovis_compare_i32, ovis_compare_u32, ovis_compare_u64, ovis_compare_f32};
  static const size_t sizes[] = {1, 1, 2, 2, 4, 4, 8, 4};
  ovis_list *list = pointer;
  OVIS_LIVE(list);
  qsort(list->data, (size_t)list->length, sizes[code], compare[code]);
}

/* Takes out the item at index (counting from 0); later items move up. */
void ovis_list_remove(void *pointer, int64_t index, int64_t element_size) {
  ovis_list *list = pointer;
  char *item = ovis_list_at(pointer, index, element_size);
  memmove(item, item + element_size,
          (size_t)((list->length - index - 1) * element_size));
  list->length--;
}

void ovis_list_reverse(void *pointer, int64_t element_size) {
  ovis_list *list = pointer;
  OVIS_LIVE(list);
  char swap[8];
  for (int64_t low = 0, high = list->length - 1; low < high; ++low, --high) {
    memcpy(swap, list->data + low * element_size, (size_t)element_size);
    memcpy(list->data + low * element_size, list->data + high * element_size,
           (size_t)element_size);
    memcpy(list->data + high * element_size, swap, (size_t)element_size);
  }
}

void *ovis_stdin(void) { return stdin; }
void *ovis_stdout(void) { return stdout; }
void *ovis_stderr(void) { return stderr; }
int64_t ovis_rand_max(void) { return RAND_MAX; }
int64_t ovis_clocks_per_sec(void) { return CLOCKS_PER_SEC; }
)runtime";
}

} // namespace ovis::codegen
