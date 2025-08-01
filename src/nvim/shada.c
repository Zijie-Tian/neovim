#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <uv.h>

#include "auto/config.h"
#include "klib/kvec.h"
#include "mpack/mpack_core.h"
#include "nvim/api/keysets_defs.h"
#include "nvim/api/private/defs.h"
#include "nvim/api/private/dispatch.h"
#include "nvim/api/private/helpers.h"
#include "nvim/ascii_defs.h"
#include "nvim/buffer.h"
#include "nvim/buffer_defs.h"
#include "nvim/cmdhist.h"
#include "nvim/eval.h"
#include "nvim/eval/decode.h"
#include "nvim/eval/encode.h"
#include "nvim/eval/typval.h"
#include "nvim/eval/typval_defs.h"
#include "nvim/ex_cmds.h"
#include "nvim/ex_cmds_defs.h"
#include "nvim/ex_docmd.h"
#include "nvim/fileio.h"
#include "nvim/gettext_defs.h"
#include "nvim/globals.h"
#include "nvim/hashtab.h"
#include "nvim/hashtab_defs.h"
#include "nvim/macros_defs.h"
#include "nvim/map_defs.h"
#include "nvim/mark.h"
#include "nvim/mark_defs.h"
#include "nvim/mbyte.h"
#include "nvim/memory.h"
#include "nvim/memory_defs.h"
#include "nvim/message.h"
#include "nvim/msgpack_rpc/packer.h"
#include "nvim/msgpack_rpc/packer_defs.h"
#include "nvim/msgpack_rpc/unpacker.h"
#include "nvim/normal_defs.h"
#include "nvim/ops.h"
#include "nvim/option.h"
#include "nvim/option_vars.h"
#include "nvim/os/fileio.h"
#include "nvim/os/fileio_defs.h"
#include "nvim/os/fs.h"
#include "nvim/os/fs_defs.h"
#include "nvim/os/os.h"
#include "nvim/os/os_defs.h"
#include "nvim/os/time.h"
#include "nvim/os/time_defs.h"
#include "nvim/path.h"
#include "nvim/pos_defs.h"
#include "nvim/regexp.h"
#include "nvim/search.h"
#include "nvim/shada.h"
#include "nvim/strings.h"
#include "nvim/types_defs.h"
#include "nvim/version.h"
#include "nvim/vim_defs.h"

#ifdef HAVE_BE64TOH
# define _BSD_SOURCE 1  // NOLINT(bugprone-reserved-identifier)
# define _DEFAULT_SOURCE 1  // NOLINT(bugprone-reserved-identifier)
# include ENDIAN_INCLUDE_FILE
#endif

#define SEARCH_KEY_MAGIC sm
#define SEARCH_KEY_SMARTCASE sc
#define SEARCH_KEY_HAS_LINE_OFFSET sl
#define SEARCH_KEY_PLACE_CURSOR_AT_END se
#define SEARCH_KEY_IS_LAST_USED su
#define SEARCH_KEY_IS_SUBSTITUTE_PATTERN ss
#define SEARCH_KEY_HIGHLIGHTED sh
#define SEARCH_KEY_OFFSET so
#define SEARCH_KEY_PAT sp
#define SEARCH_KEY_BACKWARD sb

#define REG_KEY_TYPE rt
#define REG_KEY_WIDTH rw
#define REG_KEY_CONTENTS rc
#define REG_KEY_UNNAMED ru

#define KEY_LNUM l
#define KEY_COL c
#define KEY_FILE f
#define KEY_NAME_CHAR n

// Error messages formerly used by viminfo code:
//   E136: viminfo: Too many errors, skipping rest of file
//   E137: Viminfo file is not writable: %s
//   E138: Can't write viminfo file %s!
//   E195: Cannot open ShaDa file for reading
//   E574: Unknown register type %d
//   E575: Illegal starting char
//   E576: Missing '>'
//   E577: Illegal register name
//   E886: Can't rename viminfo file to %s!
//   E929: Too many viminfo temp files, like %s!
// Now only six of them are used:
//   E137: ShaDa file is not writeable (for pre-open checks)
//   E929: All %s.tmp.X files exist, cannot write ShaDa file!
//   RCERR (E576) for critical read errors.
//   RNERR (E136) for various errors when renaming.
//   RERR (E575) for various errors inside read ShaDa file.
//   SERR (E886) for various “system” errors (always contains output of
//   strerror)
//   WERR (E574) for various ignorable write errors

/// Common prefix for all errors inside ShaDa file
///
/// I.e. errors occurred while parsing, but not system errors occurred while
/// reading.
#define RERR "E575: "

/// Common prefix for critical read errors
///
/// I.e. errors that make shada_read_next_item return kSDReadStatusNotShaDa.
#define RCERR "E576: "

/// Common prefix for all “system” errors
#define SERR "E886: "

/// Common prefix for all “rename” errors
#define RNERR "E136: "

/// Common prefix for all ignorable “write” errors
#define WERR "E574: "

/// Callback function for add_search_pattern
typedef void (*SearchPatternGetter)(SearchPattern *);

/// Possible ShaDa entry types
///
/// @warning Enum values are part of the API and must not be altered.
///
/// All values that are not in enum are ignored.
typedef enum {
  kSDItemUnknown = -1,       ///< Unknown item.
  kSDItemMissing = 0,        ///< Missing value. Should never appear in a file.
  kSDItemHeader = 1,         ///< Header. Present for debugging purposes.
  kSDItemSearchPattern = 2,  ///< Last search pattern (*not* history item).
                             ///< Comes from user searches (e.g. when typing
                             ///< "/pat") or :substitute command calls.
  kSDItemSubString = 3,      ///< Last substitute replacement string.
  kSDItemHistoryEntry = 4,   ///< History item.
  kSDItemRegister = 5,       ///< Register.
  kSDItemVariable = 6,       ///< Global variable.
  kSDItemGlobalMark = 7,     ///< Global mark definition.
  kSDItemJump = 8,           ///< Item from jump list.
  kSDItemBufferList = 9,     ///< Buffer list.
  kSDItemLocalMark = 10,     ///< Buffer-local mark.
  kSDItemChange = 11,        ///< Item from buffer change list.
} ShadaEntryType;
#define SHADA_LAST_ENTRY ((uint64_t)kSDItemChange)

/// Possible results when reading ShaDa file
typedef enum {
  kSDReadStatusSuccess,    ///< Reading was successful.
  kSDReadStatusFinished,   ///< Nothing more to read.
  kSDReadStatusReadError,  ///< Failed to read from file.
  kSDReadStatusNotShaDa,   ///< Input is most likely not a ShaDa file.
  kSDReadStatusMalformed,  ///< Error in the currently read item.
} ShaDaReadResult;

/// Possible results of shada_write function.
typedef enum {
  kSDWriteSuccessful,    ///< Writing was successful.
  kSDWriteReadNotShada,  ///< Writing was successful, but when reading it
                         ///< attempted to read file that did not look like
                         ///< a ShaDa file.
  kSDWriteFailed,        ///< Writing was not successful (e.g. because there
                         ///< was no space left on device).
  kSDWriteIgnError,      ///< Writing resulted in a error which can be ignored
                         ///< (e.g. when trying to dump a function reference or
                         ///< self-referencing container in a variable).
} ShaDaWriteResult;

/// Flags for shada_read_next_item
enum SRNIFlags {
  kSDReadHeader = (1 << kSDItemHeader),  ///< Determines whether header should
                                         ///< be read (it is usually ignored).
  kSDReadUndisableableData = (
                              (1 << kSDItemSearchPattern)
                              | (1 << kSDItemSubString)
                              | (1 << kSDItemJump)),  ///< Data reading which cannot be disabled by
                                                      ///< &shada or other options except for disabling
                                                      ///< reading ShaDa as a whole.
  kSDReadRegisters = (1 << kSDItemRegister),  ///< Determines whether registers
                                              ///< should be read (may only be
                                              ///< disabled when writing, but
                                              ///< not when reading).
  kSDReadHistory = (1 << kSDItemHistoryEntry),  ///< Determines whether history
                                                ///< should be read (can only be
                                                ///< disabled by &history).
  kSDReadVariables = (1 << kSDItemVariable),  ///< Determines whether variables
                                              ///< should be read (disabled by
                                              ///< removing ! from &shada).
  kSDReadBufferList = (1 << kSDItemBufferList),  ///< Determines whether buffer
                                                 ///< list should be read
                                                 ///< (disabled by removing
                                                 ///< % entry from &shada).
  kSDReadUnknown = (1 << (SHADA_LAST_ENTRY + 1)),  ///< Determines whether
                                                   ///< unknown items should be
                                                   ///< read (usually disabled).
  kSDReadGlobalMarks = (1 << kSDItemGlobalMark),  ///< Determines whether global
                                                  ///< marks should be read. Can
                                                  ///< only be disabled by
                                                  ///< having f0 in &shada when
                                                  ///< writing.
  kSDReadLocalMarks = (1 << kSDItemLocalMark),  ///< Determines whether local
                                                ///< marks should be read. Can
                                                ///< only be disabled by
                                                ///< disabling &shada or putting
                                                ///< '0 there. Is also used for
                                                ///< v:oldfiles.
  kSDReadChanges = (1 << kSDItemChange),  ///< Determines whether change list
                                          ///< should be read. Can only be
                                          ///< disabled by disabling &shada or
                                          ///< putting '0 there.
};
// Note: SRNIFlags enum name was created only to make it possible to reference
// it. This name is not actually used anywhere outside of the documentation.

/// Structure defining a single ShaDa file entry
typedef struct {
  ShadaEntryType type;
  Timestamp timestamp;
  union {
    Dict header;
    struct shada_filemark {
      char name;
      pos_T mark;
      char *fname;
    } filemark;
    Dict(_shada_search_pat) search_pattern;
    struct history_item {
      uint8_t histtype;
      char *string;
      char sep;
    } history_item;
    struct reg {  // yankreg_T
      char name;
      MotionType type;
      String *contents;
      bool is_unnamed;
      size_t contents_size;
      size_t width;
    } reg;
    struct global_var {
      char *name;
      typval_T value;
    } global_var;
    struct {
      uint64_t type;
      char *contents;
      size_t size;
    } unknown_item;
    struct sub_string {
      char *sub;
    } sub_string;
    struct buffer_list {
      size_t size;
      struct buffer_list_buffer {
        pos_T pos;
        char *fname;
        AdditionalData *additional_data;
      } *buffers;
    } buffer_list;
  } data;
  AdditionalData *additional_data;
} ShadaEntry;

/// One entry in sized linked list
typedef struct hm_llist_entry {
  ShadaEntry data;              ///< Entry data.
  bool can_free_entry;          ///< True if data can be freed.
  struct hm_llist_entry *next;  ///< Pointer to next entry or NULL.
  struct hm_llist_entry *prev;  ///< Pointer to previous entry or NULL.
} HMLListEntry;

/// Sized linked list structure for history merger
typedef struct {
  HMLListEntry *entries;  ///< Pointer to the start of the allocated array of
                          ///< entries.
  HMLListEntry *first;    ///< First entry in the list (is not necessary start
                          ///< of the array) or NULL.
  HMLListEntry *last;     ///< Last entry in the list or NULL.
  HMLListEntry *free_entry;  ///< Last free entry removed by hmll_remove.
  HMLListEntry *last_free_entry;  ///< Last unused element in entries array.
  size_t size;            ///< Number of allocated entries.
  size_t num_entries;     ///< Number of entries already used.
  PMap(cstr_t) contained_entries;  ///< Map all history entry strings to
                                   ///< corresponding entry pointers.
} HMLList;

typedef struct {
  HMLList hmll;
  bool do_merge;
  bool reading;
  const void *iter;
  ShadaEntry last_hist_entry;
  uint8_t history_type;
} HistoryMergerState;

/// ShadaEntry structure that knows whether it should be freed
typedef struct {
  ShadaEntry data;      ///< ShadaEntry data.
  bool can_free_entry;  ///< True if entry can be freed.
} PossiblyFreedShadaEntry;

/// Structure that holds one file marks.
typedef struct {
  PossiblyFreedShadaEntry marks[NLOCALMARKS];  ///< All file marks.
  PossiblyFreedShadaEntry changes[JUMPLISTSIZE];  ///< All file changes.
  size_t changes_size;  ///< Number of changes occupied.
  ShadaEntry *additional_marks;  ///< All marks with unknown names.
  size_t additional_marks_size;  ///< Size of the additional_marks array.
  Timestamp greatest_timestamp;  ///< Greatest timestamp among marks.
} FileMarks;

/// State structure used by shada_write
///
/// Before actually writing most of the data is read to this structure.
typedef struct {
  HistoryMergerState hms[HIST_COUNT];  ///< Structures for history merging.
  PossiblyFreedShadaEntry global_marks[NMARKS];  ///< Named global marks.
  PossiblyFreedShadaEntry numbered_marks[EXTRA_MARKS];  ///< Numbered marks.
  PossiblyFreedShadaEntry registers[NUM_SAVED_REGISTERS];  ///< All registers.
  PossiblyFreedShadaEntry jumps[JUMPLISTSIZE];  ///< All dumped jumps.
  size_t jumps_size;  ///< Number of jumps occupied.
  PossiblyFreedShadaEntry search_pattern;  ///< Last search pattern.
  PossiblyFreedShadaEntry sub_search_pattern;  ///< Last s/ search pattern.
  PossiblyFreedShadaEntry replacement;  ///< Last s// replacement string.
  Set(cstr_t) dumped_variables;  ///< Names of already dumped variables.
  PMap(cstr_t) file_marks;  ///< All file marks.
} WriteMergerState;

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "shada.c.generated.h"
#endif

#define DEF_SDE(name, attr, ...) \
  [kSDItem##name] = { \
    .timestamp = 0, \
    .type = kSDItem##name, \
    .additional_data = NULL, \
    .data = { \
      .attr = { __VA_ARGS__ } \
    } \
  }
#define DEFAULT_POS { 1, 0, 0 }
static const pos_T default_pos = DEFAULT_POS;
static const ShadaEntry sd_default_values[] = {
  [kSDItemMissing] = { .type = kSDItemMissing, .timestamp = 0 },
  DEF_SDE(Header, header, .size = 0),
  DEF_SDE(SearchPattern, search_pattern,
          .magic = true,
          .smartcase = false,
          .has_line_offset = false,
          .place_cursor_at_end = false,
          .offset = 0,
          .is_last_used = true,
          .is_substitute_pattern = false,
          .highlighted = false,
          .search_backward = false,
          .pat = STRING_INIT),
  DEF_SDE(SubString, sub_string, .sub = NULL),
  DEF_SDE(HistoryEntry, history_item,
          .histtype = HIST_CMD,
          .string = NULL,
          .sep = NUL),
  DEF_SDE(Register, reg,
          .name = NUL,
          .type = kMTCharWise,
          .contents = NULL,
          .contents_size = 0,
          .is_unnamed = false,
          .width = 0),
  DEF_SDE(Variable, global_var,
          .name = NULL,
          .value = { .v_type = VAR_UNKNOWN, .vval = { .v_string = NULL } }),
  DEF_SDE(GlobalMark, filemark,
          .name = '"',
          .mark = DEFAULT_POS,
          .fname = NULL),
  DEF_SDE(Jump, filemark,
          .name = NUL,
          .mark = DEFAULT_POS,
          .fname = NULL),
  DEF_SDE(BufferList, buffer_list,
          .size = 0,
          .buffers = NULL),
  DEF_SDE(LocalMark, filemark,
          .name = '"',
          .mark = DEFAULT_POS,
          .fname = NULL),
  DEF_SDE(Change, filemark,
          .name = NUL,
          .mark = DEFAULT_POS,
          .fname = NULL),
};
#undef DEFAULT_POS
#undef DEF_SDE

/// Initialize new linked list
///
/// @param[out]  hmll       List to initialize.
/// @param[in]   size       Maximum size of the list.
static inline void hmll_init(HMLList *const hmll, const size_t size)
  FUNC_ATTR_NONNULL_ALL
{
  *hmll = (HMLList) {
    .entries = xcalloc(size, sizeof(hmll->entries[0])),
    .first = NULL,
    .last = NULL,
    .free_entry = NULL,
    .size = size,
    .num_entries = 0,
    .contained_entries = MAP_INIT,
  };
  hmll->last_free_entry = hmll->entries;
}

/// Iterate over HMLList in forward direction
///
/// @param  hmll       Pointer to the list.
/// @param  cur_entry  Name of the variable to iterate over.
/// @param  code       Code to execute on each iteration.
///
/// @return `for` cycle header (use `HMLL_FORALL(hmll, cur_entry) {body}`).
#define HMLL_FORALL(hmll, cur_entry, code) \
  for (HMLListEntry *(cur_entry) = (hmll)->first; (cur_entry) != NULL; \
       (cur_entry) = (cur_entry)->next) { \
    code \
  } \

/// Remove entry from the linked list
///
/// @param  hmll        List to remove from.
/// @param  hmll_entry  Entry to remove.
static inline void hmll_remove(HMLList *const hmll, HMLListEntry *const hmll_entry)
  FUNC_ATTR_NONNULL_ALL
{
  if (hmll_entry == hmll->last_free_entry - 1) {
    hmll->last_free_entry--;
  } else {
    assert(hmll->free_entry == NULL);
    hmll->free_entry = hmll_entry;
  }
  ptr_t val = pmap_del(cstr_t)(&hmll->contained_entries,
                               hmll_entry->data.data.history_item.string, NULL);
  assert(val);
  (void)val;
  if (hmll_entry->next == NULL) {
    hmll->last = hmll_entry->prev;
  } else {
    hmll_entry->next->prev = hmll_entry->prev;
  }
  if (hmll_entry->prev == NULL) {
    hmll->first = hmll_entry->next;
  } else {
    hmll_entry->prev->next = hmll_entry->next;
  }
  hmll->num_entries--;
  if (hmll_entry->can_free_entry) {
    shada_free_shada_entry(&hmll_entry->data);
  }
}

/// Insert entry to the linked list
///
/// @param[out]  hmll            List to insert to.
/// @param[in]   hmll_entry      Entry to insert after or NULL if it is needed
///                              to insert at the first entry.
/// @param[in]   data            Data to insert.
/// @param[in]   can_free_entry  True if data can be freed.
static inline void hmll_insert(HMLList *const hmll, HMLListEntry *hmll_entry, const ShadaEntry data,
                               const bool can_free_entry)
  FUNC_ATTR_NONNULL_ARG(1)
{
  if (hmll->num_entries == hmll->size) {
    if (hmll_entry == hmll->first) {
      hmll_entry = NULL;
    }
    assert(hmll->first != NULL);
    hmll_remove(hmll, hmll->first);
  }
  HMLListEntry *target_entry;
  if (hmll->free_entry == NULL) {
    assert((size_t)(hmll->last_free_entry - hmll->entries)
           == hmll->num_entries);
    target_entry = hmll->last_free_entry++;
  } else {
    assert((size_t)(hmll->last_free_entry - hmll->entries) - 1
           == hmll->num_entries);
    target_entry = hmll->free_entry;
    hmll->free_entry = NULL;
  }
  target_entry->data = data;
  target_entry->can_free_entry = can_free_entry;
  bool new_item = false;
  ptr_t *val = pmap_put_ref(cstr_t)(&hmll->contained_entries, data.data.history_item.string,
                                    NULL, &new_item);
  if (new_item) {
    *val = target_entry;
  }
  hmll->num_entries++;
  target_entry->prev = hmll_entry;
  if (hmll_entry == NULL) {
    target_entry->next = hmll->first;
    hmll->first = target_entry;
  } else {
    target_entry->next = hmll_entry->next;
    hmll_entry->next = target_entry;
  }
  if (target_entry->next == NULL) {
    hmll->last = target_entry;
  } else {
    target_entry->next->prev = target_entry;
  }
}

/// Free linked list
///
/// @param[in]  hmll  List to free.
static inline void hmll_dealloc(HMLList *const hmll)
  FUNC_ATTR_NONNULL_ALL
{
  map_destroy(cstr_t, &hmll->contained_entries);
  xfree(hmll->entries);
}

/// Wrapper for read that can be used when lseek cannot be used
///
/// E.g. when trying to read from a pipe.
///
/// @param[in,out]  sd_reader  File read.
/// @param[in]      offset     Amount of bytes to skip.
///
/// @return kSDReadStatusReadError, kSDReadStatusNotShaDa or
///         kSDReadStatusSuccess.
static ShaDaReadResult sd_reader_skip(FileDescriptor *const sd_reader, const size_t offset)
  FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_NONNULL_ALL
{
  const ptrdiff_t skip_bytes = file_skip(sd_reader, offset);
  if (skip_bytes < 0) {
    semsg(_(SERR "System error while skipping in ShaDa file: %s"), os_strerror((int)skip_bytes));
    return kSDReadStatusReadError;
  } else if (skip_bytes != (ptrdiff_t)offset) {
    assert(skip_bytes < (ptrdiff_t)offset);
    if (file_eof(sd_reader)) {
      semsg(_(RCERR "Reading ShaDa file: last entry specified that it occupies %" PRIu64 " bytes, "
              "but file ended earlier"),
            (uint64_t)offset);
    } else {
      semsg(_(SERR "System error while skipping in ShaDa file: %s"), _("too few bytes read"));
    }
    return kSDReadStatusNotShaDa;
  }
  return kSDReadStatusSuccess;
}

/// Wrapper for closing file descriptors
static void close_file(FileDescriptor *cookie)
{
  const int error = file_close(cookie, !!p_fs);
  if (error != 0) {
    semsg(_(SERR "System error while closing ShaDa file: %s"),
          os_strerror(error));
  }
}

/// Read ShaDa file
///
/// @param[in]  file   File to read or NULL to use default name.
/// @param[in]  flags  Flags, see ShaDaReadFileFlags enum.
///
/// @return FAIL if reading failed for some reason and OK otherwise.
static int shada_read_file(const char *const file, const int flags)
  FUNC_ATTR_WARN_UNUSED_RESULT
{
  char *const fname = shada_filename(file);
  if (fname == NULL) {
    return FAIL;
  }

  FileDescriptor sd_reader;
  int of_ret = file_open(&sd_reader, fname, kFileReadOnly, 0);

  if (p_verbose > 1) {
    verbose_enter();
    smsg(0, _("Reading ShaDa file \"%s\"%s%s%s%s"),
         fname,
         (flags & kShaDaWantInfo) ? _(" info") : "",
         (flags & kShaDaWantMarks) ? _(" marks") : "",
         (flags & kShaDaGetOldfiles) ? _(" oldfiles") : "",
         of_ret != 0 ? _(" FAILED") : "");
    verbose_leave();
  }

  if (of_ret != 0) {
    if (of_ret != UV_ENOENT || (flags & kShaDaMissingError)) {
      semsg(_(SERR "System error while opening ShaDa file %s for reading: %s"),
            fname, os_strerror(of_ret));
    }
    xfree(fname);
    return FAIL;
  }
  xfree(fname);

  shada_read(&sd_reader, flags);
  close_file(&sd_reader);

  return OK;
}

/// Wrapper for hist_iter() function which produces ShadaEntry values
///
/// @param[in]   iter          Current iteration state.
/// @param[in]   history_type  Type of the history (HIST_*).
/// @param[in]   zero          If true, then item is removed from instance
///                            memory upon reading.
/// @param[out]  hist          Location where iteration results should be saved.
///
/// @return Next iteration state.
static const void *shada_hist_iter(const void *const iter, const uint8_t history_type,
                                   const bool zero, ShadaEntry *const hist)
  FUNC_ATTR_NONNULL_ARG(4) FUNC_ATTR_WARN_UNUSED_RESULT
{
  histentry_T hist_he;
  const void *const ret = hist_iter(iter, history_type, zero, &hist_he);
  if (hist_he.hisstr == NULL) {
    *hist = (ShadaEntry) { .type = kSDItemMissing };
  } else {
    *hist = (ShadaEntry) {
      .type = kSDItemHistoryEntry,
      .timestamp = hist_he.timestamp,
      .data = {
        .history_item = {
          .histtype = history_type,
          .string = hist_he.hisstr,
          .sep = (char)(history_type == HIST_SEARCH
                        ? hist_he.hisstr[hist_he.hisstrlen + 1]
                        : 0),
        }
      },
      .additional_data = hist_he.additional_data,
    };
  }
  return ret;
}

/// Insert history entry
///
/// Inserts history entry at the end of the ring buffer (may insert earlier
/// according to the timestamp). If entry was already in the ring buffer
/// existing entry will be removed unless it has greater timestamp.
///
/// Before the new entry entries from the current Neovim history will be
/// inserted unless `do_iter` argument is false.
///
/// @param[in,out]  hms_p           Ring buffer and associated structures.
/// @param[in]      entry           Inserted entry.
/// @param[in]      do_iter         Determines whether Neovim own history should
///                                 be used. Must be true only if inserting
///                                 entry from current Neovim history.
/// @param[in]      can_free_entry  True if entry can be freed.
static void hms_insert(HistoryMergerState *const hms_p, const ShadaEntry entry, const bool do_iter,
                       const bool can_free_entry)
  FUNC_ATTR_NONNULL_ALL
{
  if (do_iter) {
    while (hms_p->last_hist_entry.type != kSDItemMissing
           && hms_p->last_hist_entry.timestamp < entry.timestamp) {
      hms_insert(hms_p, hms_p->last_hist_entry, false, hms_p->reading);
      if (hms_p->iter == NULL) {
        hms_p->last_hist_entry.type = kSDItemMissing;
        break;
      }
      hms_p->iter = shada_hist_iter(hms_p->iter, hms_p->history_type,
                                    hms_p->reading, &hms_p->last_hist_entry);
    }
  }
  HMLList *const hmll = &hms_p->hmll;
  cstr_t *key_alloc = NULL;
  ptr_t *val = pmap_ref(cstr_t)(&hms_p->hmll.contained_entries, entry.data.history_item.string,
                                &key_alloc);
  if (val) {
    HMLListEntry *const existing_entry = *val;
    if (entry.timestamp > existing_entry->data.timestamp) {
      hmll_remove(hmll, existing_entry);
    } else if (!do_iter && entry.timestamp == existing_entry->data.timestamp) {
      // Prefer entry from the current Neovim instance.
      if (existing_entry->can_free_entry) {
        shada_free_shada_entry(&existing_entry->data);
      }
      existing_entry->data = entry;
      existing_entry->can_free_entry = can_free_entry;
      // Previous key was freed above, as part of freeing the ShaDa entry.
      *key_alloc = entry.data.history_item.string;
      return;
    } else {
      return;
    }
  }
  HMLListEntry *insert_after;
  // Iterate over HMLList in backward direction
  for (insert_after = hmll->last; insert_after != NULL; insert_after = insert_after->prev) {
    if (insert_after->data.timestamp <= entry.timestamp) {
      break;
    }
  }
  hmll_insert(hmll, insert_after, entry, can_free_entry);
}

/// Initialize the history merger
///
/// @param[out]  hms_p         Structure to be initialized.
/// @param[in]   history_type  History type (one of HIST_\* values).
/// @param[in]   num_elements  Number of elements in the result.
/// @param[in]   do_merge      Prepare structure for merging elements.
/// @param[in]   reading       If true, then merger is reading history for use
///                            in Neovim.
static inline void hms_init(HistoryMergerState *const hms_p, const uint8_t history_type,
                            const size_t num_elements, const bool do_merge, const bool reading)
  FUNC_ATTR_NONNULL_ALL
{
  hmll_init(&hms_p->hmll, num_elements);
  hms_p->do_merge = do_merge;
  hms_p->reading = reading;
  hms_p->iter = shada_hist_iter(NULL, history_type, hms_p->reading,
                                &hms_p->last_hist_entry);
  hms_p->history_type = history_type;
}

/// Merge in all remaining Neovim own history entries
///
/// @param[in,out]  hms_p  Merger structure into which history should be
///                        inserted.
static inline void hms_insert_whole_neovim_history(HistoryMergerState *const hms_p)
  FUNC_ATTR_NONNULL_ALL
{
  while (hms_p->last_hist_entry.type != kSDItemMissing) {
    hms_insert(hms_p, hms_p->last_hist_entry, false, hms_p->reading);
    if (hms_p->iter == NULL) {
      break;
    }
    hms_p->iter = shada_hist_iter(hms_p->iter, hms_p->history_type,
                                  hms_p->reading, &hms_p->last_hist_entry);
  }
}

/// Convert merger structure to Neovim internal structure for history
///
/// @param[in]   hms_p       Converted merger structure.
/// @param[out]  hist_array  Array with the results.
/// @param[out]  new_hisidx  New last history entry index.
/// @param[out]  new_hisnum  Amount of history items in merger structure.
static inline void hms_to_he_array(const HistoryMergerState *const hms_p,
                                   histentry_T *const hist_array, int *const new_hisidx,
                                   int *const new_hisnum)
  FUNC_ATTR_NONNULL_ALL
{
  histentry_T *hist = hist_array;
  HMLL_FORALL(&hms_p->hmll, cur_entry,  {
    hist->timestamp = cur_entry->data.timestamp;
    hist->hisnum = (int)(hist - hist_array) + 1;
    hist->hisstr = cur_entry->data.data.history_item.string;
    hist->hisstrlen = strlen(cur_entry->data.data.history_item.string);
    hist->additional_data = cur_entry->data.additional_data;
    hist++;
  })
  *new_hisnum = (int)(hist - hist_array);
  *new_hisidx = *new_hisnum - 1;
}

/// Free history merger structure
///
/// @param[in]  hms_p  Structure to be freed.
static inline void hms_dealloc(HistoryMergerState *const hms_p)
  FUNC_ATTR_NONNULL_ALL
{
  hmll_dealloc(&hms_p->hmll);
}

/// Iterate over all history entries in history merger, in order
///
/// @param[in]   hms_p      Merger structure to iterate over.
/// @param[out]  cur_entry  Name of the iterator variable.
/// @param       code       Code to execute on each iteration.
///
/// @return for cycle header. Use `HMS_ITER(hms_p, cur_entry) {body}`.
#define HMS_ITER(hms_p, cur_entry, code) \
  HMLL_FORALL(&((hms_p)->hmll), cur_entry, code)

/// Iterate over global variables
///
/// @warning No modifications to global variable Dict must be performed
///          while iteration is in progress.
///
/// @param[in]   iter   Iterator. Pass NULL to start iteration.
/// @param[out]  name   Variable name.
/// @param[out]  rettv  Variable value.
///
/// @return Pointer that needs to be passed to next `var_shada_iter` invocation
///         or NULL to indicate that iteration is over.
static const void *var_shada_iter(const void *const iter, const char **const name, typval_T *rettv,
                                  var_flavour_T flavour)
  FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_NONNULL_ARG(2, 3)
{
  const hashitem_T *hi;
  const hashitem_T *hifirst = globvarht.ht_array;
  const size_t hinum = (size_t)globvarht.ht_mask + 1;
  *name = NULL;
  if (iter == NULL) {
    hi = globvarht.ht_array;
    while ((size_t)(hi - hifirst) < hinum
           && (HASHITEM_EMPTY(hi)
               || !(var_flavour(hi->hi_key) & flavour))) {
      hi++;
    }
    if ((size_t)(hi - hifirst) == hinum) {
      return NULL;
    }
  } else {
    hi = (const hashitem_T *)iter;
  }
  *name = TV_DICT_HI2DI(hi)->di_key;
  tv_copy(&TV_DICT_HI2DI(hi)->di_tv, rettv);
  while ((size_t)(++hi - hifirst) < hinum) {
    if (!HASHITEM_EMPTY(hi) && (var_flavour(hi->hi_key) & flavour)) {
      return hi;
    }
  }
  return NULL;
}

/// Find buffer for given buffer name (cached)
///
/// @param[in,out]  fname_bufs  Cache containing fname to buffer mapping.
/// @param[in]      fname       File name to find.
///
/// @return Pointer to the buffer or NULL.
static buf_T *find_buffer(PMap(cstr_t) *const fname_bufs, const char *const fname)
  FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_NONNULL_ALL
{
  cstr_t *key_alloc = NULL;
  bool new_item = false;
  buf_T **ref = (buf_T **)pmap_put_ref(cstr_t)(fname_bufs, fname, &key_alloc, &new_item);
  if (new_item) {
    *key_alloc = xstrdup(fname);
  } else {
    return *ref;  // item already existed (can be a NULL value)
  }

  FOR_ALL_BUFFERS(buf) {
    if (buf->b_ffname != NULL) {
      if (path_fnamecmp(fname, buf->b_ffname) == 0) {
        *ref = buf;
        return buf;
      }
    }
  }
  *ref = NULL;
  return NULL;
}

/// Compare two marks
static inline bool marks_equal(const pos_T a, const pos_T b)
{
  return (a.lnum == b.lnum) && (a.col == b.col);
}

/// adjust "jumps_arr" to make space to insert an item just before the item at "i"
/// (or after the last if i == jl_len)
///
/// Higher incidies indicate newer items. If the list is full, discard the oldest item
/// (or don't insert the considered item if it is older)
///
/// @return the actual position a new item should be inserted or -1 if it shouldn't be inserted
static int marklist_insert(void *jumps_arr, size_t jump_size, int jl_len, int i)
{
  char *jumps = (char *)jumps_arr;  // for pointer maffs
  if (i > 0) {
    if (jl_len == JUMPLISTSIZE) {
      i--;
      if (i > 0) {
        // delete oldest item to make room for new element
        memmove(jumps, jumps + jump_size, jump_size * (size_t)i);
      }
    } else if (i != jl_len) {
      // insert at position i, move newer items out of the way
      memmove(jumps + (size_t)(i + 1) * jump_size, jumps + (size_t)i * jump_size,
              jump_size * (size_t)(jl_len - i));
    }
  } else if (i == 0) {
    if (jl_len == JUMPLISTSIZE) {
      return -1;  // don't insert, older than the entire list
    } else if (jl_len > 0) {
      // insert i as the oldest item
      memmove(jumps + jump_size, jumps, jump_size * (size_t)jl_len);
    }
  }
  return i;
}

/// Read data from ShaDa file
///
/// @param[in]  sd_reader  Structure containing file reader definition.
/// @param[in]  flags      What to read, see ShaDaReadFileFlags enum.
static void shada_read(FileDescriptor *const sd_reader, const int flags)
  FUNC_ATTR_NONNULL_ALL
{
  list_T *oldfiles_list = get_vim_var_list(VV_OLDFILES);
  const bool force = flags & kShaDaForceit;
  const bool get_old_files = (flags & (kShaDaGetOldfiles | kShaDaForceit)
                              && (force || tv_list_len(oldfiles_list) == 0));
  const bool want_marks = flags & kShaDaWantMarks;
  const unsigned srni_flags =
    (unsigned)(
               (flags & kShaDaWantInfo
                ? (kSDReadUndisableableData
                   | kSDReadRegisters
                   | kSDReadGlobalMarks
                   | (p_hi ? kSDReadHistory : 0)
                   | (find_shada_parameter('!') != NULL
                      ? kSDReadVariables
                      : 0)
                   | (find_shada_parameter('%') != NULL
                      && ARGCOUNT == 0
                      ? kSDReadBufferList
                      : 0))
                : 0)
               | (want_marks && get_shada_parameter('\'') > 0
                  ? kSDReadLocalMarks | kSDReadChanges
                  : 0)
               | (get_old_files
                  ? kSDReadLocalMarks
                  : 0));
  if (srni_flags == 0) {
    // Nothing to do.
    return;
  }
  HistoryMergerState hms[HIST_COUNT];
  if (srni_flags & kSDReadHistory) {
    for (int i = 0; i < HIST_COUNT; i++) {
      hms_init(&hms[i], (uint8_t)i, (size_t)p_hi, true, true);
    }
  }
  ShadaEntry cur_entry;
  Set(ptr_t) cl_bufs = SET_INIT;
  PMap(cstr_t) fname_bufs = MAP_INIT;
  Set(cstr_t) oldfiles_set = SET_INIT;
  if (get_old_files && (oldfiles_list == NULL || force)) {
    oldfiles_list = tv_list_alloc(kListLenUnknown);
    set_vim_var_list(VV_OLDFILES, oldfiles_list);
  }
  ShaDaReadResult srni_ret;
  while ((srni_ret = shada_read_next_item(sd_reader, &cur_entry, srni_flags, 0))
         != kSDReadStatusFinished) {
    switch (srni_ret) {
    case kSDReadStatusSuccess:
      break;
    case kSDReadStatusFinished:
      // Should be handled by the while condition.
      abort();
    case kSDReadStatusNotShaDa:
    case kSDReadStatusReadError:
      goto shada_read_main_cycle_end;
    case kSDReadStatusMalformed:
      continue;
    }
    switch (cur_entry.type) {
    case kSDItemMissing:
      abort();
    case kSDItemUnknown:
      break;
    case kSDItemHeader:
      shada_free_shada_entry(&cur_entry);
      break;
    case kSDItemSearchPattern:
      if (!force) {
        SearchPattern pat;
        if (cur_entry.data.search_pattern.is_substitute_pattern) {
          get_substitute_pattern(&pat);
        } else {
          get_search_pattern(&pat);
        }
        if (pat.pat != NULL && pat.timestamp >= cur_entry.timestamp) {
          shada_free_shada_entry(&cur_entry);
          break;
        }
      }

      SearchPattern spat = (SearchPattern) {
        .magic = cur_entry.data.search_pattern.magic,
        .no_scs = !cur_entry.data.search_pattern.smartcase,
        .off = {
          .dir = cur_entry.data.search_pattern.search_backward ? '?' : '/',
          .line = cur_entry.data.search_pattern.has_line_offset,
          .end = cur_entry.data.search_pattern.place_cursor_at_end,
          .off = cur_entry.data.search_pattern.offset,
        },
        .pat = cur_entry.data.search_pattern.pat.data,
        .patlen = cur_entry.data.search_pattern.pat.size,
        .additional_data = cur_entry.additional_data,
        .timestamp = cur_entry.timestamp,
      };

      if (cur_entry.data.search_pattern.is_substitute_pattern) {
        set_substitute_pattern(spat);
      } else {
        set_search_pattern(spat);
      }

      if (cur_entry.data.search_pattern.is_last_used) {
        set_last_used_pattern(cur_entry.data.search_pattern.is_substitute_pattern);
        set_no_hlsearch(!cur_entry.data.search_pattern.highlighted);
      }
      // Do not free shada entry: its allocated memory was saved above.
      break;
    case kSDItemSubString:
      if (!force) {
        SubReplacementString sub;
        sub_get_replacement(&sub);
        if (sub.sub != NULL && sub.timestamp >= cur_entry.timestamp) {
          shada_free_shada_entry(&cur_entry);
          break;
        }
      }
      sub_set_replacement((SubReplacementString) {
        .sub = cur_entry.data.sub_string.sub,
        .timestamp = cur_entry.timestamp,
        .additional_data = cur_entry.additional_data,
      });
      // Without using regtilde and without / &cpo flag previous substitute
      // string is close to useless: you can only use it with :& or :~ and
      // that’s all because s//~ is not available until the first call to
      // regtilde. Vim was not calling this for some reason.
      regtilde(cur_entry.data.sub_string.sub, magic_isset(), false);
      // Do not free shada entry: its allocated memory was saved above.
      break;
    case kSDItemHistoryEntry:
      if (cur_entry.data.history_item.histtype >= HIST_COUNT) {
        shada_free_shada_entry(&cur_entry);
        break;
      }
      hms_insert(hms + cur_entry.data.history_item.histtype, cur_entry, true,
                 true);
      // Do not free shada entry: its allocated memory was saved above.
      break;
    case kSDItemRegister:
      if (cur_entry.data.reg.type != kMTCharWise
          && cur_entry.data.reg.type != kMTLineWise
          && cur_entry.data.reg.type != kMTBlockWise) {
        shada_free_shada_entry(&cur_entry);
        break;
      }
      if (!force) {
        const yankreg_T *const reg = op_reg_get(cur_entry.data.reg.name);
        if (reg == NULL || reg->timestamp >= cur_entry.timestamp) {
          shada_free_shada_entry(&cur_entry);
          break;
        }
      }
      if (!op_reg_set(cur_entry.data.reg.name, (yankreg_T) {
        .y_array = cur_entry.data.reg.contents,
        .y_size = cur_entry.data.reg.contents_size,
        .y_type = cur_entry.data.reg.type,
        .y_width = (colnr_T)cur_entry.data.reg.width,
        .timestamp = cur_entry.timestamp,
        .additional_data = cur_entry.additional_data,
      }, cur_entry.data.reg.is_unnamed)) {
        shada_free_shada_entry(&cur_entry);
      }
      // Do not free shada entry: its allocated memory was saved above.
      break;
    case kSDItemVariable:
      var_set_global(cur_entry.data.global_var.name,
                     cur_entry.data.global_var.value);
      cur_entry.data.global_var.value.v_type = VAR_UNKNOWN;
      shada_free_shada_entry(&cur_entry);
      break;
    case kSDItemJump:
    case kSDItemGlobalMark: {
      buf_T *buf = find_buffer(&fname_bufs, cur_entry.data.filemark.fname);
      if (buf != NULL) {
        XFREE_CLEAR(cur_entry.data.filemark.fname);
      }
      xfmark_T fm = (xfmark_T) {
        .fname = buf == NULL ? cur_entry.data.filemark.fname : NULL,
        .fmark = {
          .mark = cur_entry.data.filemark.mark,
          .fnum = (buf == NULL ? 0 : buf->b_fnum),
          .timestamp = cur_entry.timestamp,
          .view = INIT_FMARKV,
          .additional_data = cur_entry.additional_data,
        },
      };
      if (cur_entry.type == kSDItemGlobalMark) {
        if (!mark_set_global(cur_entry.data.filemark.name, fm, !force)) {
          shada_free_shada_entry(&cur_entry);
          break;
        }
      } else {
        int i;
        for (i = curwin->w_jumplistlen; i > 0; i--) {
          const xfmark_T jl_entry = curwin->w_jumplist[i - 1];
          if (jl_entry.fmark.timestamp <= cur_entry.timestamp) {
            if (marks_equal(jl_entry.fmark.mark, cur_entry.data.filemark.mark)
                && (buf == NULL
                    ? (jl_entry.fname != NULL && strcmp(fm.fname, jl_entry.fname) == 0)
                    : fm.fmark.fnum == jl_entry.fmark.fnum)) {
              i = -1;
            }
            break;
          }
        }
        if (i > 0 && curwin->w_jumplistlen == JUMPLISTSIZE) {
          free_xfmark(curwin->w_jumplist[0]);
        }
        i = marklist_insert(curwin->w_jumplist, sizeof(*curwin->w_jumplist),
                            curwin->w_jumplistlen, i);

        if (i != -1) {
          curwin->w_jumplist[i] = fm;
          if (curwin->w_jumplistlen < JUMPLISTSIZE) {
            curwin->w_jumplistlen++;
          }
          if (curwin->w_jumplistidx >= i && curwin->w_jumplistidx + 1 <= curwin->w_jumplistlen) {
            curwin->w_jumplistidx++;
          }
        } else {
          shada_free_shada_entry(&cur_entry);
        }
      }

      // Do not free shada entry: its allocated memory was saved above.
      break;
    }
    case kSDItemBufferList:
      for (size_t i = 0; i < cur_entry.data.buffer_list.size; i++) {
        char *const sfname =
          path_try_shorten_fname(cur_entry.data.buffer_list.buffers[i].fname);
        buf_T *const buf =
          buflist_new(cur_entry.data.buffer_list.buffers[i].fname, sfname, 0, BLN_LISTED);
        if (buf != NULL) {
          fmarkv_T view = INIT_FMARKV;
          RESET_FMARK(&buf->b_last_cursor,
                      cur_entry.data.buffer_list.buffers[i].pos, 0, view);
          buflist_setfpos(buf, curwin, buf->b_last_cursor.mark.lnum,
                          buf->b_last_cursor.mark.col, false);

          xfree(buf->additional_data);
          buf->additional_data = cur_entry.data.buffer_list.buffers[i].additional_data;
          cur_entry.data.buffer_list.buffers[i].additional_data = NULL;
        }
      }
      shada_free_shada_entry(&cur_entry);
      break;
    case kSDItemChange:
    case kSDItemLocalMark: {
      if (get_old_files && !set_has(cstr_t, &oldfiles_set, cur_entry.data.filemark.fname)) {
        char *fname = cur_entry.data.filemark.fname;
        if (want_marks) {
          // Do not bother with allocating memory for the string if already
          // allocated string from cur_entry can be used. It cannot be used if
          // want_marks is set because this way it may be used for a mark.
          fname = xstrdup(fname);
        }
        set_put(cstr_t, &oldfiles_set, fname);
        tv_list_append_allocated_string(oldfiles_list, fname);
        if (!want_marks) {
          // Avoid free because this string was already used.
          cur_entry.data.filemark.fname = NULL;
        }
      }
      if (!want_marks) {
        shada_free_shada_entry(&cur_entry);
        break;
      }
      buf_T *buf = find_buffer(&fname_bufs, cur_entry.data.filemark.fname);
      if (buf == NULL) {
        shada_free_shada_entry(&cur_entry);
        break;
      }
      const fmark_T fm = (fmark_T) {
        .mark = cur_entry.data.filemark.mark,
        .fnum = 0,
        .timestamp = cur_entry.timestamp,
        .view = INIT_FMARKV,
        .additional_data = cur_entry.additional_data,
      };
      if (cur_entry.type == kSDItemLocalMark) {
        if (!mark_set_local(cur_entry.data.filemark.name, buf, fm, !force)) {
          shada_free_shada_entry(&cur_entry);
          break;
        }
      } else {
        set_put(ptr_t, &cl_bufs, buf);
        int i;
        for (i = buf->b_changelistlen; i > 0; i--) {
          const fmark_T jl_entry = buf->b_changelist[i - 1];
          if (jl_entry.timestamp <= cur_entry.timestamp) {
            if (marks_equal(jl_entry.mark, cur_entry.data.filemark.mark)) {
              i = -1;
            }
            break;
          }
        }
        if (i > 0 && buf->b_changelistlen == JUMPLISTSIZE) {
          free_fmark(buf->b_changelist[0]);
        }
        i = marklist_insert(buf->b_changelist, sizeof(*buf->b_changelist), buf->b_changelistlen, i);
        if (i != -1) {
          buf->b_changelist[i] = fm;
          if (buf->b_changelistlen < JUMPLISTSIZE) {
            buf->b_changelistlen++;
          }
        } else {
          xfree(fm.additional_data);
        }
      }
      // only free fname part of shada entry, as additional_data was saved or freed above.
      xfree(cur_entry.data.filemark.fname);
      break;
    }
    }
  }
shada_read_main_cycle_end:
  // Warning: shada_hist_iter returns ShadaEntry elements which use strings from
  //          original history list. This means that once such entry is removed
  //          from the history Neovim array will no longer be valid. To reduce
  //          amount of memory allocations ShaDa file reader allocates enough
  //          memory for the history string itself and separator character which
  //          may be assigned right away.
  if (srni_flags & kSDReadHistory) {
    for (int i = 0; i < HIST_COUNT; i++) {
      hms_insert_whole_neovim_history(&hms[i]);
      clr_history(i);
      int *new_hisidx;
      int *new_hisnum;
      histentry_T *hist = hist_get_array((uint8_t)i, &new_hisidx, &new_hisnum);
      if (hist != NULL) {
        hms_to_he_array(&hms[i], hist, new_hisidx, new_hisnum);
      }
      hms_dealloc(&hms[i]);
    }
  }
  if (cl_bufs.h.n_occupied) {
    FOR_ALL_TAB_WINDOWS(tp, wp) {
      (void)tp;
      if (set_has(ptr_t, &cl_bufs, wp->w_buffer)) {
        wp->w_changelistidx = wp->w_buffer->b_changelistlen;
      }
    }
  }
  set_destroy(ptr_t, &cl_bufs);
  const char *key;
  map_foreach_key(&fname_bufs, key, {
    xfree((char *)key);
  })
  map_destroy(cstr_t, &fname_bufs);
  set_destroy(cstr_t, &oldfiles_set);
}

/// Default shada file location: cached path
static char *default_shada_file = NULL;

/// Get the default ShaDa file
static const char *shada_get_default_file(void)
  FUNC_ATTR_WARN_UNUSED_RESULT
{
  if (default_shada_file == NULL) {
    char *shada_dir = stdpaths_user_state_subpath("shada", 0, false);
    default_shada_file = concat_fnames_realloc(shada_dir, "main.shada", true);
  }
  return default_shada_file;
}

/// Get the ShaDa file name to use
///
/// If "file" is given and not empty, use it (has already been expanded by
/// cmdline functions). Otherwise use "-i file_name", value from 'shada' or the
/// default, and expand environment variables.
///
/// @param[in]  file  Forced file name or NULL.
///
/// @return  An allocated string containing shada file name,
///          or NULL if shada file should not be used.
static char *shada_filename(const char *file)
  FUNC_ATTR_MALLOC FUNC_ATTR_WARN_UNUSED_RESULT
{
  if (file == NULL || *file == NUL) {
    if (p_shadafile != NULL && *p_shadafile != NUL) {
      // Check if writing to ShaDa file was disabled ("-i NONE" or "--clean").
      if (!strequal(p_shadafile, "NONE")) {
        file = p_shadafile;
      } else {
        return NULL;
      }
    } else {
      if ((file = find_shada_parameter('n')) == NULL || *file == NUL) {
        file = shada_get_default_file();
      }
      // XXX It used to be one level lower, so that whatever is in
      //     `p_shadafile` was expanded. I intentionally moved it here
      //     because various expansions must have already be done by the shell.
      //     If shell is not performing them then they should be done in main.c
      //     where arguments are parsed, *not here*.
      size_t len = expand_env((char *)file, &(NameBuff[0]), MAXPATHL);
      file = &(NameBuff[0]);
      return xmemdupz(file, len);
    }
  }
  return xstrdup(file);
}

#define KEY_NAME_(s) #s
#define PACK_KEY(s) mpack_str(STATIC_CSTR_AS_STRING(KEY_NAME_(s)), &sbuf);
#define KEY_NAME(s) KEY_NAME_(s)

#define SHADA_MPACK_FREE_SPACE (4 * MPACK_ITEM_SIZE)

static void shada_check_buffer(PackerBuffer *packer)
{
  if (mpack_remaining(packer) < SHADA_MPACK_FREE_SPACE) {
    packer->packer_flush(packer);
  }
}

static uint32_t additional_data_len(AdditionalData *src)
{
  return src ? src->nitems : 0;
}

static void dump_additional_data(AdditionalData *src, PackerBuffer *sbuf)
{
  if (src != NULL) {
    mpack_raw(src->data, src->nbytes, sbuf);
  }
}

/// Write single ShaDa entry
///
/// @param[in]  packer     Packer used to write entry.
/// @param[in]  entry      Entry written.
/// @param[in]  max_kbyte  Maximum size of an item in KiB. Zero means no
///                        restrictions.
///
/// @return kSDWriteSuccessful, kSDWriteFailed or kSDWriteIgnError.
static ShaDaWriteResult shada_pack_entry(PackerBuffer *const packer, ShadaEntry entry,
                                         const size_t max_kbyte)
  FUNC_ATTR_NONNULL_ALL
{
  ShaDaWriteResult ret = kSDWriteFailed;
  PackerBuffer sbuf = packer_string_buffer();

#define CHECK_DEFAULT(entry, attr) \
  (sd_default_values[(entry).type].data.attr == (entry).data.attr)
#define ONE_IF_NOT_DEFAULT(entry, attr) \
  ((uint32_t)(!CHECK_DEFAULT(entry, attr)))

#define PACK_BOOL(entry, name, attr) \
  do { \
    if (!CHECK_DEFAULT(entry, search_pattern.attr)) { \
      PACK_KEY(name); \
      mpack_bool(&sbuf.ptr, !sd_default_values[(entry).type].data.search_pattern.attr); \
    } \
  } while (0)

  shada_check_buffer(&sbuf);
  switch (entry.type) {
  case kSDItemMissing:
    abort();
  case kSDItemUnknown:
    mpack_raw(entry.data.unknown_item.contents, entry.data.unknown_item.size, &sbuf);
    break;
  case kSDItemHistoryEntry: {
    const bool is_hist_search =
      entry.data.history_item.histtype == HIST_SEARCH;
    uint32_t arr_size = (2 + (uint32_t)is_hist_search
                         + additional_data_len(entry.additional_data));
    mpack_array(&sbuf.ptr, arr_size);
    mpack_uint(&sbuf.ptr, entry.data.history_item.histtype);
    mpack_bin(cstr_as_string(entry.data.history_item.string), &sbuf);
    if (is_hist_search) {
      mpack_uint(&sbuf.ptr, (uint8_t)entry.data.history_item.sep);
    }
    dump_additional_data(entry.additional_data, &sbuf);
    break;
  }
  case kSDItemVariable: {
    bool is_blob = (entry.data.global_var.value.v_type == VAR_BLOB);
    uint32_t arr_size = 2 + (is_blob ? 1 : 0) + additional_data_len(entry.additional_data);
    mpack_array(&sbuf.ptr, arr_size);
    const String varname = cstr_as_string(entry.data.global_var.name);
    mpack_bin(varname, &sbuf);
    char vardesc[256] = "variable g:";
    memcpy(&vardesc[sizeof("variable g:") - 1], varname.data,
           varname.size + 1);
    if (encode_vim_to_msgpack(&sbuf, &entry.data.global_var.value, vardesc)
        == FAIL) {
      ret = kSDWriteIgnError;
      semsg(_(WERR "Failed to write variable %s"),
            entry.data.global_var.name);
      goto shada_pack_entry_error;
    }
    if (is_blob) {
      mpack_check_buffer(&sbuf);
      mpack_integer(&sbuf.ptr, VAR_TYPE_BLOB);
    }
    dump_additional_data(entry.additional_data, &sbuf);
    break;
  }
  case kSDItemSubString: {
    uint32_t arr_size = 1 + additional_data_len(entry.additional_data);
    mpack_array(&sbuf.ptr, arr_size);
    mpack_bin(cstr_as_string(entry.data.sub_string.sub), &sbuf);
    dump_additional_data(entry.additional_data, &sbuf);
    break;
  }
  case kSDItemSearchPattern: {
    uint32_t entry_map_size = (1  // Search pattern is always present
                               + ONE_IF_NOT_DEFAULT(entry, search_pattern.magic)
                               + ONE_IF_NOT_DEFAULT(entry, search_pattern.is_last_used)
                               + ONE_IF_NOT_DEFAULT(entry, search_pattern.smartcase)
                               + ONE_IF_NOT_DEFAULT(entry, search_pattern.has_line_offset)
                               + ONE_IF_NOT_DEFAULT(entry, search_pattern.place_cursor_at_end)
                               + ONE_IF_NOT_DEFAULT(entry,
                                                    search_pattern.is_substitute_pattern)
                               + ONE_IF_NOT_DEFAULT(entry, search_pattern.highlighted)
                               + ONE_IF_NOT_DEFAULT(entry, search_pattern.offset)
                               + ONE_IF_NOT_DEFAULT(entry, search_pattern.search_backward)
                               + additional_data_len(entry.additional_data));
    mpack_map(&sbuf.ptr, entry_map_size);
    PACK_KEY(SEARCH_KEY_PAT);
    mpack_bin(entry.data.search_pattern.pat, &sbuf);
    PACK_BOOL(entry, SEARCH_KEY_MAGIC, magic);
    PACK_BOOL(entry, SEARCH_KEY_IS_LAST_USED, is_last_used);
    PACK_BOOL(entry, SEARCH_KEY_SMARTCASE, smartcase);
    PACK_BOOL(entry, SEARCH_KEY_HAS_LINE_OFFSET, has_line_offset);
    PACK_BOOL(entry, SEARCH_KEY_PLACE_CURSOR_AT_END, place_cursor_at_end);
    PACK_BOOL(entry, SEARCH_KEY_IS_SUBSTITUTE_PATTERN, is_substitute_pattern);
    PACK_BOOL(entry, SEARCH_KEY_HIGHLIGHTED, highlighted);
    PACK_BOOL(entry, SEARCH_KEY_BACKWARD, search_backward);
    if (!CHECK_DEFAULT(entry, search_pattern.offset)) {
      PACK_KEY(SEARCH_KEY_OFFSET);
      mpack_integer(&sbuf.ptr, entry.data.search_pattern.offset);
    }
#undef PACK_BOOL
    dump_additional_data(entry.additional_data, &sbuf);
    break;
  }
  case kSDItemChange:
  case kSDItemGlobalMark:
  case kSDItemLocalMark:
  case kSDItemJump: {
    size_t entry_map_size = (1  // File name
                             + ONE_IF_NOT_DEFAULT(entry, filemark.mark.lnum)
                             + ONE_IF_NOT_DEFAULT(entry, filemark.mark.col)
                             + ONE_IF_NOT_DEFAULT(entry, filemark.name)
                             + additional_data_len(entry.additional_data));
    mpack_map(&sbuf.ptr, (uint32_t)entry_map_size);
    PACK_KEY(KEY_FILE);
    mpack_bin(cstr_as_string(entry.data.filemark.fname), &sbuf);
    if (!CHECK_DEFAULT(entry, filemark.mark.lnum)) {
      PACK_KEY(KEY_LNUM);
      mpack_integer(&sbuf.ptr, entry.data.filemark.mark.lnum);
    }
    if (!CHECK_DEFAULT(entry, filemark.mark.col)) {
      PACK_KEY(KEY_COL);
      mpack_integer(&sbuf.ptr, entry.data.filemark.mark.col);
    }
    assert(entry.type == kSDItemJump || entry.type == kSDItemChange
           ? CHECK_DEFAULT(entry, filemark.name)
           : true);
    if (!CHECK_DEFAULT(entry, filemark.name)) {
      PACK_KEY(KEY_NAME_CHAR);
      mpack_uint(&sbuf.ptr, (uint8_t)entry.data.filemark.name);
    }
    dump_additional_data(entry.additional_data, &sbuf);
    break;
  }
  case kSDItemRegister: {
    uint32_t entry_map_size = (2  // Register contents and name
                               + ONE_IF_NOT_DEFAULT(entry, reg.type)
                               + ONE_IF_NOT_DEFAULT(entry, reg.width)
                               + ONE_IF_NOT_DEFAULT(entry, reg.is_unnamed)
                               + additional_data_len(entry.additional_data));

    mpack_map(&sbuf.ptr, entry_map_size);
    PACK_KEY(REG_KEY_CONTENTS);
    mpack_array(&sbuf.ptr, (uint32_t)entry.data.reg.contents_size);
    for (size_t i = 0; i < entry.data.reg.contents_size; i++) {
      mpack_bin(entry.data.reg.contents[i], &sbuf);
    }
    PACK_KEY(KEY_NAME_CHAR);
    mpack_uint(&sbuf.ptr, (uint8_t)entry.data.reg.name);
    if (!CHECK_DEFAULT(entry, reg.type)) {
      PACK_KEY(REG_KEY_TYPE);
      mpack_uint(&sbuf.ptr, (uint8_t)entry.data.reg.type);
    }
    if (!CHECK_DEFAULT(entry, reg.width)) {
      PACK_KEY(REG_KEY_WIDTH);
      mpack_uint64(&sbuf.ptr, (uint64_t)entry.data.reg.width);
    }
    if (!CHECK_DEFAULT(entry, reg.is_unnamed)) {
      PACK_KEY(REG_KEY_UNNAMED);
      mpack_bool(&sbuf.ptr, entry.data.reg.is_unnamed);
    }
    dump_additional_data(entry.additional_data, &sbuf);
    break;
  }
  case kSDItemBufferList:
    mpack_array(&sbuf.ptr, (uint32_t)entry.data.buffer_list.size);
    for (size_t i = 0; i < entry.data.buffer_list.size; i++) {
      size_t entry_map_size = (1  // Buffer name
                               + (size_t)(entry.data.buffer_list.buffers[i].pos.lnum
                                          != default_pos.lnum)
                               + (size_t)(entry.data.buffer_list.buffers[i].pos.col
                                          != default_pos.col)
                               + additional_data_len(entry.data.buffer_list.buffers[i].
                                                     additional_data));
      mpack_map(&sbuf.ptr, (uint32_t)entry_map_size);
      PACK_KEY(KEY_FILE);
      mpack_bin(cstr_as_string(entry.data.buffer_list.buffers[i].fname), &sbuf);
      if (entry.data.buffer_list.buffers[i].pos.lnum != 1) {
        PACK_KEY(KEY_LNUM);
        mpack_uint64(&sbuf.ptr, (uint64_t)entry.data.buffer_list.buffers[i].pos.lnum);
      }
      if (entry.data.buffer_list.buffers[i].pos.col != 0) {
        PACK_KEY(KEY_COL);
        mpack_uint64(&sbuf.ptr, (uint64_t)entry.data.buffer_list.buffers[i].pos.col);
      }
      dump_additional_data(entry.data.buffer_list.buffers[i].additional_data, &sbuf);
    }
    break;
  case kSDItemHeader:
    mpack_map(&sbuf.ptr, (uint32_t)entry.data.header.size);
    for (size_t i = 0; i < entry.data.header.size; i++) {
      mpack_str(entry.data.header.items[i].key, &sbuf);
      const Object obj = entry.data.header.items[i].value;
      switch (obj.type) {
      case kObjectTypeString:
        mpack_bin(obj.data.string, &sbuf);
        break;
      case kObjectTypeInteger:
        mpack_integer(&sbuf.ptr, obj.data.integer);
        break;
      default:
        abort();
      }
    }
    break;
  }
#undef CHECK_DEFAULT
#undef ONE_IF_NOT_DEFAULT
  String packed = packer_take_string(&sbuf);
  if (!max_kbyte || packed.size <= max_kbyte * 1024) {
    shada_check_buffer(packer);

    if (entry.type == kSDItemUnknown) {
      mpack_uint64(&packer->ptr, entry.data.unknown_item.type);
    } else {
      mpack_uint64(&packer->ptr, (uint64_t)entry.type);
    }
    mpack_uint64(&packer->ptr, (uint64_t)entry.timestamp);
    if (packed.size > 0) {
      mpack_uint64(&packer->ptr, (uint64_t)packed.size);
      mpack_raw(packed.data, packed.size, packer);
    }

    if (packer->anyint != 0) {  // error code
      goto shada_pack_entry_error;
    }
  }
  ret = kSDWriteSuccessful;
shada_pack_entry_error:
  xfree(sbuf.startptr);
  return ret;
}

/// Write single ShaDa entry and free it afterwards
///
/// Will not free if entry could not be freed.
///
/// @param[in]  packer     Packer used to write entry.
/// @param[in]  entry      Entry written.
/// @param[in]  max_kbyte  Maximum size of an item in KiB. Zero means no
///                        restrictions.
static inline ShaDaWriteResult shada_pack_pfreed_entry(PackerBuffer *const packer,
                                                       PossiblyFreedShadaEntry entry,
                                                       const size_t max_kbyte)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_ALWAYS_INLINE
{
  ShaDaWriteResult ret = kSDWriteSuccessful;
  ret = shada_pack_entry(packer, entry.data, max_kbyte);
  if (entry.can_free_entry) {
    shada_free_shada_entry(&entry.data);
  }
  return ret;
}

/// Compare two FileMarks structure to order them by greatest_timestamp
///
/// Order is reversed: structure with greatest greatest_timestamp comes first.
/// Function signature is compatible with qsort.
static int compare_file_marks(const void *a, const void *b)
  FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_PURE
{
  const FileMarks *const *const a_fms = a;
  const FileMarks *const *const b_fms = b;
  return ((*a_fms)->greatest_timestamp == (*b_fms)->greatest_timestamp
          ? 0
          : ((*a_fms)->greatest_timestamp > (*b_fms)->greatest_timestamp ? -1 : 1));
}

/// Parse msgpack object that has given length
///
/// @param[in]   sd_reader     Structure containing file reader definition.
/// @param[in]   length        Object length.
/// @param[out]  ret_unpacked  Location where read result should be saved. If
///                            NULL then unpacked data will be freed. Must be
///                            NULL if `ret_buf` is NULL.
/// @param[out]  ret_buf       Buffer containing parsed string.
///
/// @return kSDReadStatusNotShaDa, kSDReadStatusReadError or
///         kSDReadStatusSuccess.
static ShaDaReadResult shada_check_status(uintmax_t initial_fpos, int status, size_t remaining)
  FUNC_ATTR_WARN_UNUSED_RESULT
{
  switch (status) {
  case MPACK_OK:
    if (remaining) {
      semsg(_(RCERR "Failed to parse ShaDa file: extra bytes in msgpack string "
              "at position %" PRIu64),
            (uint64_t)initial_fpos);
      return kSDReadStatusNotShaDa;
    }
    return kSDReadStatusSuccess;
  case MPACK_EOF:
    semsg(_(RCERR "Failed to parse ShaDa file: incomplete msgpack string "
            "at position %" PRIu64),
          (uint64_t)initial_fpos);
    return kSDReadStatusNotShaDa;
  default:
    semsg(_(RCERR "Failed to parse ShaDa file due to a msgpack parser error "
            "at position %" PRIu64),
          (uint64_t)initial_fpos);
    return kSDReadStatusNotShaDa;
  }
}

/// Format shada entry for debugging purposes
///
/// @param[in]  entry  ShaDa entry to format.
///
/// @return string representing ShaDa entry in a static buffer.
static const char *shada_format_entry(const ShadaEntry entry)
  FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_UNUSED FUNC_ATTR_NONNULL_RET
{
  static char ret[1024];
  ret[0] = 0;
  vim_snprintf(S_LEN(ret), "%s", "[ ] ts=%" PRIu64 " ");
  //                         ^ Space for `can_free_entry`
#define FORMAT_MARK_ENTRY(entry_name, name_fmt, name_fmt_arg) \
  do { \
    vim_snprintf_add(S_LEN(ret), \
                     entry_name " {" name_fmt " file=[%zu]\"%.512s\", " \
                     "pos={l=%" PRIdLINENR ",c=%" PRIdCOLNR ",a=%" PRIdCOLNR "}, " \
                     "}", \
                     name_fmt_arg, \
                     strlen(entry.data.filemark.fname), \
                     entry.data.filemark.fname, \
                     entry.data.filemark.mark.lnum, \
                     entry.data.filemark.mark.col, \
                     entry.data.filemark.mark.coladd); \
  } while (0)
  switch (entry.type) {
  case kSDItemMissing:
    vim_snprintf_add(S_LEN(ret), "Missing");
    break;
  case kSDItemHeader:
    vim_snprintf_add(S_LEN(ret), "Header { TODO }");
    break;
  case kSDItemBufferList:
    vim_snprintf_add(S_LEN(ret), "BufferList { TODO }");
    break;
  case kSDItemUnknown:
    vim_snprintf_add(S_LEN(ret), "Unknown { TODO }");
    break;
  case kSDItemSearchPattern:
    vim_snprintf_add(S_LEN(ret), "SearchPattern { TODO }");
    break;
  case kSDItemSubString:
    vim_snprintf_add(S_LEN(ret), "SubString { TODO }");
    break;
  case kSDItemHistoryEntry:
    vim_snprintf_add(S_LEN(ret), "HistoryEntry { TODO }");
    break;
  case kSDItemRegister:
    vim_snprintf_add(S_LEN(ret), "Register { TODO }");
    break;
  case kSDItemVariable:
    vim_snprintf_add(S_LEN(ret), "Variable { TODO }");
    break;
  case kSDItemGlobalMark:
    FORMAT_MARK_ENTRY("GlobalMark", " name='%c',", entry.data.filemark.name);
    break;
  case kSDItemChange:
    FORMAT_MARK_ENTRY("Change", "%s", "");
    break;
  case kSDItemLocalMark:
    FORMAT_MARK_ENTRY("LocalMark", " name='%c',", entry.data.filemark.name);
    break;
  case kSDItemJump:
    FORMAT_MARK_ENTRY("Jump", "%s", "");
    break;
#undef FORMAT_MARK_ENTRY
  }
  return ret;
}

/// Format possibly freed shada entry for debugging purposes
///
/// @param[in]  entry  ShaDa entry to format.
///
/// @return string representing ShaDa entry in a static buffer.
static const char *shada_format_pfreed_entry(const PossiblyFreedShadaEntry pfs_entry)
  FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_UNUSED FUNC_ATTR_NONNULL_RET
{
  char *ret = (char *)shada_format_entry(pfs_entry.data);
  ret[1] = (pfs_entry.can_free_entry ? 'T' : 'F');
  return ret;
}

/// Read and merge in ShaDa file, used when writing
///
/// @param[in]      sd_reader   Structure containing file reader definition.
/// @param[in]      srni_flags  Flags determining what to read.
/// @param[in]      max_kbyte   Maximum size of one element.
/// @param[in,out]  ret_wms     Location where results are saved.
/// @param[out]     packer      MessagePack packer for entries which are not
///                             merged.
static inline ShaDaWriteResult shada_read_when_writing(FileDescriptor *const sd_reader,
                                                       const unsigned srni_flags,
                                                       const size_t max_kbyte,
                                                       WriteMergerState *const wms,
                                                       PackerBuffer *const packer)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_WARN_UNUSED_RESULT
{
  ShaDaWriteResult ret = kSDWriteSuccessful;
  ShadaEntry entry;
  ShaDaReadResult srni_ret;

#define COMPARE_WITH_ENTRY(wms_entry_, entry) \
  do { \
    PossiblyFreedShadaEntry *const wms_entry = (wms_entry_); \
    if (wms_entry->data.type != kSDItemMissing) { \
      if (wms_entry->data.timestamp >= (entry).timestamp) { \
        shada_free_shada_entry(&(entry)); \
        break; \
      } \
      if (wms_entry->can_free_entry) { \
        shada_free_shada_entry(&wms_entry->data); \
      } \
    } \
    *wms_entry = pfs_entry; \
  } while (0)

  while ((srni_ret = shada_read_next_item(sd_reader, &entry, srni_flags,
                                          max_kbyte))
         != kSDReadStatusFinished) {
    switch (srni_ret) {
    case kSDReadStatusSuccess:
      break;
    case kSDReadStatusFinished:
      // Should be handled by the while condition.
      abort();
    case kSDReadStatusNotShaDa:
      ret = kSDWriteReadNotShada;
      FALLTHROUGH;
    case kSDReadStatusReadError:
      return ret;
    case kSDReadStatusMalformed:
      continue;
    }
    const PossiblyFreedShadaEntry pfs_entry = {
      .can_free_entry = true,
      .data = entry,
    };
    switch (entry.type) {
    case kSDItemMissing:
      break;
    case kSDItemHeader:
    case kSDItemBufferList:
      abort();
    case kSDItemUnknown:
      ret = shada_pack_entry(packer, entry, 0);
      shada_free_shada_entry(&entry);
      break;
    case kSDItemSearchPattern:
      COMPARE_WITH_ENTRY((entry.data.search_pattern.is_substitute_pattern
                          ? &wms->sub_search_pattern
                          : &wms->search_pattern), entry);
      break;
    case kSDItemSubString:
      COMPARE_WITH_ENTRY(&wms->replacement, entry);
      break;
    case kSDItemHistoryEntry:
      if (entry.data.history_item.histtype >= HIST_COUNT) {
        ret = shada_pack_entry(packer, entry, 0);
        shada_free_shada_entry(&entry);
        break;
      }
      if (wms->hms[entry.data.history_item.histtype].hmll.size != 0) {
        hms_insert(&wms->hms[entry.data.history_item.histtype], entry, true,
                   true);
      } else {
        shada_free_shada_entry(&entry);
      }
      break;
    case kSDItemRegister: {
      const int idx = op_reg_index(entry.data.reg.name);
      if (idx < 0) {
        ret = shada_pack_entry(packer, entry, 0);
        shada_free_shada_entry(&entry);
        break;
      }
      COMPARE_WITH_ENTRY(&wms->registers[idx], entry);
      break;
    }
    case kSDItemVariable:
      if (!set_has(cstr_t, &wms->dumped_variables, entry.data.global_var.name)) {
        ret = shada_pack_entry(packer, entry, 0);
      }
      shada_free_shada_entry(&entry);
      break;
    case kSDItemGlobalMark:
      if (ascii_isdigit(entry.data.filemark.name)) {
        bool processed_mark = false;
        // Completely ignore numbered mark names, make a list sorted by
        // timestamp.
        for (size_t i = ARRAY_SIZE(wms->numbered_marks); i > 0; i--) {
          ShadaEntry wms_entry = wms->numbered_marks[i - 1].data;
          if (wms_entry.type != kSDItemGlobalMark) {
            continue;
          }
          // Ignore duplicates.
          if (wms_entry.timestamp == entry.timestamp
              && (wms_entry.additional_data == NULL
                  && entry.additional_data == NULL)
              && marks_equal(wms_entry.data.filemark.mark,
                             entry.data.filemark.mark)
              && strcmp(wms_entry.data.filemark.fname,
                        entry.data.filemark.fname) == 0) {
            shada_free_shada_entry(&entry);
            processed_mark = true;
            break;
          }
          if (wms_entry.timestamp >= entry.timestamp) {
            processed_mark = true;
            if (i < ARRAY_SIZE(wms->numbered_marks)) {
              replace_numbered_mark(wms, i, pfs_entry);
            } else {
              shada_free_shada_entry(&entry);
            }
            break;
          }
        }
        if (!processed_mark) {
          replace_numbered_mark(wms, 0, pfs_entry);
        }
      } else {
        const int idx = mark_global_index(entry.data.filemark.name);
        if (idx < 0) {
          ret = shada_pack_entry(packer, entry, 0);
          shada_free_shada_entry(&entry);
          break;
        }

        // Global or numbered mark.
        PossiblyFreedShadaEntry *mark
          = idx < 26 ? &wms->global_marks[idx] : &wms->numbered_marks[idx - 26];

        if (mark->data.type == kSDItemMissing) {
          if (namedfm[idx].fmark.timestamp >= entry.timestamp) {
            shada_free_shada_entry(&entry);
            break;
          }
        }
        COMPARE_WITH_ENTRY(mark, entry);
      }
      break;
    case kSDItemChange:
    case kSDItemLocalMark: {
      if (shada_removable(entry.data.filemark.fname)) {
        shada_free_shada_entry(&entry);
        break;
      }
      const char *const fname = entry.data.filemark.fname;
      cstr_t *key = NULL;
      bool new_item = false;
      ptr_t *val = pmap_put_ref(cstr_t)(&wms->file_marks, fname, &key, &new_item);
      if (new_item) {
        *key = xstrdup(fname);
      }
      if (*val == NULL) {
        *val = xcalloc(1, sizeof(FileMarks));
      }
      FileMarks *const filemarks = *val;
      if (entry.timestamp > filemarks->greatest_timestamp) {
        filemarks->greatest_timestamp = entry.timestamp;
      }
      if (entry.type == kSDItemLocalMark) {
        const int idx = mark_local_index(entry.data.filemark.name);
        if (idx < 0) {
          filemarks->additional_marks = xrealloc(filemarks->additional_marks,
                                                 (++filemarks->additional_marks_size
                                                  * sizeof(filemarks->additional_marks[0])));
          filemarks->additional_marks[filemarks->additional_marks_size - 1] =
            entry;
        } else {
          PossiblyFreedShadaEntry *const wms_entry = &filemarks->marks[idx];
          bool set_wms = true;
          if (wms_entry->data.type != kSDItemMissing) {
            if (wms_entry->data.timestamp >= entry.timestamp) {
              shada_free_shada_entry(&entry);
              break;
            }
            if (wms_entry->can_free_entry) {
              if (*key == wms_entry->data.data.filemark.fname) {
                *key = entry.data.filemark.fname;
              }
              shada_free_shada_entry(&wms_entry->data);
            }
          } else {
            FOR_ALL_BUFFERS(buf) {
              if (buf->b_ffname != NULL
                  && path_fnamecmp(entry.data.filemark.fname, buf->b_ffname) == 0) {
                fmark_T fm;
                mark_get(buf, curwin, &fm, kMarkBufLocal, (int)entry.data.filemark.name);
                if (fm.timestamp >= entry.timestamp) {
                  set_wms = false;
                  shada_free_shada_entry(&entry);
                  break;
                }
              }
            }
          }
          if (set_wms) {
            *wms_entry = pfs_entry;
          }
        }
      } else {
        int i;
        for (i = (int)filemarks->changes_size; i > 0; i--) {
          const PossiblyFreedShadaEntry jl_entry = filemarks->changes[i - 1];
          if (jl_entry.data.timestamp <= (entry).timestamp) {
            if (marks_equal(jl_entry.data.data.filemark.mark, entry.data.filemark.mark)) {
              i = -1;
            }
            break;
          }
        }
        if (i > 0 && filemarks->changes_size == JUMPLISTSIZE) {
          if (filemarks->changes[0].can_free_entry) {
            shada_free_shada_entry(&filemarks->changes[0].data);
          }
        }
        i = marklist_insert(filemarks->changes, sizeof(*filemarks->changes),
                            (int)filemarks->changes_size, i);
        if (i != -1) {
          filemarks->changes[i] = (PossiblyFreedShadaEntry) { .can_free_entry = true,
                                                              .data = entry };
          if (filemarks->changes_size < JUMPLISTSIZE) {
            filemarks->changes_size++;
          }
        } else {
          shada_free_shada_entry(&(entry));
        }
      }
      break;
    }
    case kSDItemJump:
      ;
      int i;
      for (i = (int)wms->jumps_size; i > 0; i--) {
        const PossiblyFreedShadaEntry jl_entry = wms->jumps[i - 1];
        if (jl_entry.data.timestamp <= entry.timestamp) {
          if (marks_equal(jl_entry.data.data.filemark.mark, entry.data.filemark.mark)
              && strcmp(jl_entry.data.data.filemark.fname, entry.data.filemark.fname) == 0) {
            i = -1;
          }
          break;
        }
      }
      if (i > 0 && wms->jumps_size == JUMPLISTSIZE) {
        if (wms->jumps[0].can_free_entry) {
          shada_free_shada_entry(&wms->jumps[0].data);
        }
      }
      i = marklist_insert(wms->jumps, sizeof(*wms->jumps), (int)wms->jumps_size, i);
      if (i != -1) {
        wms->jumps[i] = (PossiblyFreedShadaEntry) { .can_free_entry = true, .data = entry };
        if (wms->jumps_size < JUMPLISTSIZE) {
          wms->jumps_size++;
        }
      } else {
        shada_free_shada_entry(&(entry));
      }
      break;
    }
  }
#undef COMPARE_WITH_ENTRY
  return ret;
}

/// Check whether buffer should be ignored
///
/// @param[in]  buf  buf_T* to check.
/// @param[in]  removable_bufs  Cache of buffers ignored due to their location.
///
/// @return true or false.
static inline bool ignore_buf(const buf_T *const buf, Set(ptr_t) *const removable_bufs)
  FUNC_ATTR_PURE FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_ALWAYS_INLINE
{
  return (buf == NULL || buf->b_ffname == NULL || !buf->b_p_bl || bt_quickfix(buf) \
          || bt_terminal(buf) || set_has(ptr_t, removable_bufs, (ptr_t)buf));
}

/// Get list of buffers to write to the shada file
///
/// @param[in]  removable_bufs  Buffers which are ignored
///
/// @return  ShadaEntry  List of buffers to save, kSDItemBufferList entry.
static inline ShadaEntry shada_get_buflist(Set(ptr_t) *const removable_bufs)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_ALWAYS_INLINE
{
  int max_bufs = get_shada_parameter('%');
  size_t buf_count = 0;
  FOR_ALL_BUFFERS(buf) {
    if (!ignore_buf(buf, removable_bufs)
        && (max_bufs < 0 || buf_count < (size_t)max_bufs)) {
      buf_count++;
    }
  }

  ShadaEntry buflist_entry = (ShadaEntry) {
    .type = kSDItemBufferList,
    .timestamp = os_time(),
    .data = {
      .buffer_list = {
        .size = buf_count,
        .buffers = xmalloc(buf_count
                           * sizeof(*buflist_entry.data.buffer_list.buffers)),
      },
    },
  };
  size_t i = 0;
  FOR_ALL_BUFFERS(buf) {
    if (ignore_buf(buf, removable_bufs)) {
      continue;
    }
    if (i >= buf_count) {
      break;
    }
    buflist_entry.data.buffer_list.buffers[i] = (struct buffer_list_buffer) {
      .pos = buf->b_last_cursor.mark,
      .fname = buf->b_ffname,
      .additional_data = buf->additional_data,
    };
    i++;
  }

  return buflist_entry;
}

/// Save search pattern to PossiblyFreedShadaEntry
///
/// @param[out]  ret_pse  Location where result will be saved.
/// @param[in]  get_pattern  Function used to get pattern.
/// @param[in]  is_substitute_pattern  True if pattern in question is substitute
///                                    pattern. Also controls whether some
///                                    fields should be initialized to default
///                                    or values from get_pattern.
/// @param[in]  search_last_used  Result of search_was_last_used().
/// @param[in]  search_highlighted  True if search pattern was highlighted by
///                                 &hlsearch and this information should be
///                                 saved.
static inline void add_search_pattern(PossiblyFreedShadaEntry *const ret_pse,
                                      const SearchPatternGetter get_pattern,
                                      const bool is_substitute_pattern, const bool search_last_used,
                                      const bool search_highlighted)
  FUNC_ATTR_ALWAYS_INLINE
{
  const ShadaEntry defaults = sd_default_values[kSDItemSearchPattern];
  SearchPattern pat;
  get_pattern(&pat);
  if (pat.pat != NULL) {
    *ret_pse = (PossiblyFreedShadaEntry) {
      .can_free_entry = false,
      .data = {
        .type = kSDItemSearchPattern,
        .timestamp = pat.timestamp,
        .data = {
          .search_pattern = {
            .magic = pat.magic,
            .smartcase = !pat.no_scs,
            .has_line_offset = (is_substitute_pattern
                                ? defaults.data.search_pattern.has_line_offset
                                : pat.off.line),
            .place_cursor_at_end = (
                                    is_substitute_pattern
                                    ? defaults.data.search_pattern.place_cursor_at_end
                                    : pat.off.end),
            .offset = (is_substitute_pattern
                       ? defaults.data.search_pattern.offset
                       : pat.off.off),
            .is_last_used = (is_substitute_pattern ^ search_last_used),
            .is_substitute_pattern = is_substitute_pattern,
            .highlighted = ((is_substitute_pattern ^ search_last_used)
                            && search_highlighted),
            .pat = cstr_as_string(pat.pat),
            .search_backward = (!is_substitute_pattern && pat.off.dir == '?'),
          }
        },
        .additional_data = pat.additional_data,
      }
    };
  }
}

/// Initialize registers for writing to the ShaDa file
///
/// @param[in]  wms  The WriteMergerState used when writing.
/// @param[in]  max_reg_lines  The maximum number of register lines.
static inline void shada_initialize_registers(WriteMergerState *const wms, int max_reg_lines)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_ALWAYS_INLINE
{
  const void *reg_iter = NULL;
  const bool limit_reg_lines = max_reg_lines >= 0;
  do {
    yankreg_T reg;
    char name = NUL;
    bool is_unnamed = false;
    reg_iter = op_global_reg_iter(reg_iter, &name, &reg, &is_unnamed);
    if (name == NUL) {
      break;
    }
    if (limit_reg_lines && reg.y_size > (size_t)max_reg_lines) {
      continue;
    }
    wms->registers[op_reg_index(name)] = (PossiblyFreedShadaEntry) {
      .can_free_entry = false,
      .data = {
        .type = kSDItemRegister,
        .timestamp = reg.timestamp,
        .data = {
          .reg = {
            .contents = reg.y_array,
            .contents_size = reg.y_size,
            .type = reg.y_type,
            .width = (size_t)(reg.y_type == kMTBlockWise ? reg.y_width : 0),
            .name = name,
            .is_unnamed = is_unnamed,
          }
        },
        .additional_data = reg.additional_data,
      }
    };
  } while (reg_iter != NULL);
}

/// Replace numbered mark in WriteMergerState
///
/// Frees the last mark, moves (including adjusting mark names) marks from idx
/// to the last-but-one one and saves the new mark at given index.
///
/// @param[out]  wms  Merger state to adjust.
/// @param[in]  idx  Index at which new mark should be placed.
/// @param[in]  entry  New mark.
static inline void replace_numbered_mark(WriteMergerState *const wms, const size_t idx,
                                         const PossiblyFreedShadaEntry entry)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_ALWAYS_INLINE
{
  if (ARRAY_LAST_ENTRY(wms->numbered_marks).can_free_entry) {
    shada_free_shada_entry(&ARRAY_LAST_ENTRY(wms->numbered_marks).data);
  }
  for (size_t i = idx; i < ARRAY_SIZE(wms->numbered_marks) - 1; i++) {
    if (wms->numbered_marks[i].data.type == kSDItemGlobalMark) {
      wms->numbered_marks[i].data.data.filemark.name = (char)('0' + (int)i + 1);
    }
  }
  memmove(wms->numbered_marks + idx + 1, wms->numbered_marks + idx,
          sizeof(wms->numbered_marks[0])
          * (ARRAY_SIZE(wms->numbered_marks) - 1 - idx));
  wms->numbered_marks[idx] = entry;
  wms->numbered_marks[idx].data.data.filemark.name = (char)('0' + (int)idx);
}

/// Find buffers ignored due to their location.
///
/// @param[out]  removable_bufs  Cache of buffers ignored due to their location.
static inline void find_removable_bufs(Set(ptr_t) *removable_bufs)
{
  FOR_ALL_BUFFERS(buf) {
    if (buf->b_ffname != NULL && shada_removable(buf->b_ffname)) {
      set_put(ptr_t, removable_bufs, (ptr_t)buf);
    }
  }
}

/// Translate a history type number to the associated character
static int hist_type2char(const int type)
  FUNC_ATTR_CONST
{
  switch (type) {
  case HIST_CMD:
    return ':';
  case HIST_SEARCH:
    return '/';
  case HIST_EXPR:
    return '=';
  case HIST_INPUT:
    return '@';
  case HIST_DEBUG:
    return '>';
  default:
    abort();
  }
  return NUL;
}

static PackerBuffer packer_buffer_for_file(FileDescriptor *file)
{
  if (file_space(file) < SHADA_MPACK_FREE_SPACE) {
    file_flush(file);
  }
  return (PackerBuffer) {
    .startptr = file->buffer,
    .ptr = file->write_pos,
    .endptr = file->buffer + ARENA_BLOCK_SIZE,
    .anydata = file,
    .anyint = 0,  // set to nonzero if error
    .packer_flush = flush_file_buffer,
  };
}

static void flush_file_buffer(PackerBuffer *buffer)
{
  FileDescriptor *fd = buffer->anydata;
  fd->write_pos = buffer->ptr;
  buffer->anyint = file_flush(fd);
  buffer->ptr = fd->write_pos;
}

/// Write ShaDa file
///
/// @param[in]  sd_writer  Structure containing file writer definition.
/// @param[in]  sd_reader  Structure containing file reader definition. If it is
///                        not NULL then contents of this file will be merged
///                        with current Neovim runtime.
static ShaDaWriteResult shada_write(FileDescriptor *const sd_writer,
                                    FileDescriptor *const sd_reader)
  FUNC_ATTR_NONNULL_ARG(1)
{
  ShaDaWriteResult ret = kSDWriteSuccessful;
  int max_kbyte_i = get_shada_parameter('s');
  if (max_kbyte_i < 0) {
    max_kbyte_i = 10;
  }
  if (max_kbyte_i == 0) {
    return ret;
  }

  WriteMergerState *const wms = xcalloc(1, sizeof(*wms));
  bool dump_one_history[HIST_COUNT];
  const bool dump_global_vars = (find_shada_parameter('!') != NULL);
  int max_reg_lines = get_shada_parameter('<');
  if (max_reg_lines < 0) {
    max_reg_lines = get_shada_parameter('"');
  }
  const bool dump_registers = (max_reg_lines != 0);
  Set(ptr_t) removable_bufs = SET_INIT;
  const size_t max_kbyte = (size_t)max_kbyte_i;
  const size_t num_marked_files = (size_t)get_shada_parameter('\'');
  const bool dump_global_marks = get_shada_parameter('f') != 0;
  bool dump_history = false;

  // Initialize history merger
  for (int i = 0; i < HIST_COUNT; i++) {
    int num_saved = get_shada_parameter(hist_type2char(i));
    if (num_saved == -1) {
      num_saved = (int)p_hi;
    }
    if (num_saved > 0) {
      dump_history = true;
      dump_one_history[i] = true;
      hms_init(&wms->hms[i], (uint8_t)i, (size_t)num_saved, sd_reader != NULL, false);
    } else {
      dump_one_history[i] = false;
    }
  }

  const unsigned srni_flags = (unsigned)(kSDReadUndisableableData
                                         | kSDReadUnknown
                                         | (dump_history ? kSDReadHistory : 0)
                                         | (dump_registers ? kSDReadRegisters : 0)
                                         | (dump_global_vars ? kSDReadVariables : 0)
                                         | (dump_global_marks ? kSDReadGlobalMarks : 0)
                                         | (num_marked_files ? kSDReadLocalMarks |
                                            kSDReadChanges : 0));

  PackerBuffer packer = packer_buffer_for_file(sd_writer);

  // Set b_last_cursor for all the buffers that have a window.
  //
  // It is needed to correctly save '"' mark on exit. Has a side effect of
  // setting '"' mark in all windows on :wshada to the current cursor
  // position (basically what :wviminfo used to do).
  FOR_ALL_TAB_WINDOWS(tp, wp) {
    set_last_cursor(wp);
  }

  find_removable_bufs(&removable_bufs);

  // Write header
  if (shada_pack_entry(&packer, (ShadaEntry) {
    .type = kSDItemHeader,
    .timestamp = os_time(),
    .data = {
      .header = {
        .size = 5,
        .capacity = 5,
        .items = ((KeyValuePair[]) {
          { STATIC_CSTR_AS_STRING("generator"),
            STATIC_CSTR_AS_OBJ("nvim") },
          { STATIC_CSTR_AS_STRING("version"),
            CSTR_AS_OBJ(longVersion) },
          { STATIC_CSTR_AS_STRING("max_kbyte"),
            INTEGER_OBJ((Integer)max_kbyte) },
          { STATIC_CSTR_AS_STRING("pid"),
            INTEGER_OBJ((Integer)os_get_pid()) },
          { STATIC_CSTR_AS_STRING("encoding"),
            CSTR_AS_OBJ(p_enc) },
        }),
      }
    }
  }, 0) == kSDWriteFailed) {
    ret = kSDWriteFailed;
    goto shada_write_exit;
  }

  // Write buffer list
  if (find_shada_parameter('%') != NULL) {
    ShadaEntry buflist_entry = shada_get_buflist(&removable_bufs);
    if (shada_pack_entry(&packer, buflist_entry, 0) == kSDWriteFailed) {
      xfree(buflist_entry.data.buffer_list.buffers);
      ret = kSDWriteFailed;
      goto shada_write_exit;
    }
    xfree(buflist_entry.data.buffer_list.buffers);
  }

  // Write some of the variables
  if (dump_global_vars) {
    const void *var_iter = NULL;
    const Timestamp cur_timestamp = os_time();
    do {
      typval_T vartv;
      const char *name = NULL;
      var_iter = var_shada_iter(var_iter, &name, &vartv, VAR_FLAVOUR_SHADA);
      if (name == NULL) {
        break;
      }
      switch (vartv.v_type) {
      case VAR_FUNC:
      case VAR_PARTIAL:
        tv_clear(&vartv);
        continue;
      case VAR_DICT: {
        dict_T *di = vartv.vval.v_dict;
        int copyID = get_copyID();
        if (!set_ref_in_ht(&di->dv_hashtab, copyID, NULL)
            && copyID == di->dv_copyID) {
          tv_clear(&vartv);
          continue;
        }
        break;
      }
      case VAR_LIST: {
        list_T *l = vartv.vval.v_list;
        int copyID = get_copyID();
        if (!set_ref_in_list_items(l, copyID, NULL)
            && copyID == l->lv_copyID) {
          tv_clear(&vartv);
          continue;
        }
        break;
      }
      default:
        break;
      }
      typval_T tgttv;
      tv_copy(&vartv, &tgttv);
      ShaDaWriteResult spe_ret;
      if ((spe_ret = shada_pack_entry(&packer, (ShadaEntry) {
        .type = kSDItemVariable,
        .timestamp = cur_timestamp,
        .data = {
          .global_var = {
            .name = (char *)name,
            .value = tgttv,
          }
        },
        .additional_data = NULL,
      }, max_kbyte)) == kSDWriteFailed) {
        tv_clear(&vartv);
        tv_clear(&tgttv);
        ret = kSDWriteFailed;
        goto shada_write_exit;
      }
      tv_clear(&vartv);
      tv_clear(&tgttv);
      if (spe_ret == kSDWriteSuccessful) {
        set_put(cstr_t, &wms->dumped_variables, name);
      }
    } while (var_iter != NULL);
  }

  if (num_marked_files > 0) {  // Skip if '0 in 'shada'
    // Initialize jump list
    wms->jumps_size = shada_init_jumps(wms->jumps, &removable_bufs);
  }

  if (dump_one_history[HIST_SEARCH] > 0) {  // Skip if /0 in 'shada'
    const bool search_highlighted = !(no_hlsearch
                                      || find_shada_parameter('h') != NULL);
    const bool search_last_used = search_was_last_used();

    // Initialize search pattern
    add_search_pattern(&wms->search_pattern, &get_search_pattern, false,
                       search_last_used, search_highlighted);

    // Initialize substitute search pattern
    add_search_pattern(&wms->sub_search_pattern, &get_substitute_pattern, true,
                       search_last_used, search_highlighted);

    // Initialize substitute replacement string
    SubReplacementString sub;
    sub_get_replacement(&sub);
    if (sub.sub != NULL) {  // Don't store empty replacement string
      wms->replacement = (PossiblyFreedShadaEntry) {
        .can_free_entry = false,
        .data = {
          .type = kSDItemSubString,
          .timestamp = sub.timestamp,
          .data = {
            .sub_string = {
              .sub = sub.sub,
            }
          },
          .additional_data = sub.additional_data,
        }
      };
    }
  }

  // Initialize global marks
  if (dump_global_marks) {
    const void *global_mark_iter = NULL;
    size_t digit_mark_idx = 0;
    do {
      char name = NUL;
      xfmark_T fm;
      global_mark_iter = mark_global_iter(global_mark_iter, &name, &fm);
      if (name == NUL) {
        break;
      }
      const char *fname;
      if (fm.fmark.fnum == 0) {
        assert(fm.fname != NULL);
        if (shada_removable(fm.fname)) {
          continue;
        }
        fname = fm.fname;
      } else {
        const buf_T *const buf = buflist_findnr(fm.fmark.fnum);
        if (ignore_buf(buf, &removable_bufs)) {
          continue;
        }
        fname = buf->b_ffname;
      }
      const PossiblyFreedShadaEntry pf_entry = {
        .can_free_entry = false,
        .data = {
          .type = kSDItemGlobalMark,
          .timestamp = fm.fmark.timestamp,
          .data = {
            .filemark = {
              .mark = fm.fmark.mark,
              .name = name,
              .fname = (char *)fname,
            }
          },
          .additional_data = fm.fmark.additional_data,
        },
      };
      if (ascii_isdigit(name)) {
        replace_numbered_mark(wms, digit_mark_idx++, pf_entry);
      } else {
        wms->global_marks[mark_global_index(name)] = pf_entry;
      }
    } while (global_mark_iter != NULL);
  }

  // Initialize registers
  if (dump_registers) {
    shada_initialize_registers(wms, max_reg_lines);
  }

  // Initialize buffers
  if (num_marked_files > 0) {
    FOR_ALL_BUFFERS(buf) {
      if (ignore_buf(buf, &removable_bufs)) {
        continue;
      }
      const void *local_marks_iter = NULL;
      const char *const fname = buf->b_ffname;
      cstr_t *map_key = NULL;
      bool new_item = false;
      ptr_t *val = pmap_put_ref(cstr_t)(&wms->file_marks, fname, &map_key, &new_item);
      if (new_item) {
        *map_key = xstrdup(fname);
      }
      if (*val == NULL) {
        *val = xcalloc(1, sizeof(FileMarks));
      }
      FileMarks *const filemarks = *val;
      do {
        fmark_T fm;
        char name = NUL;
        local_marks_iter = mark_buffer_iter(local_marks_iter, buf, &name, &fm);
        if (name == NUL) {
          break;
        }
        filemarks->marks[mark_local_index(name)] = (PossiblyFreedShadaEntry) {
          .can_free_entry = false,
          .data = {
            .type = kSDItemLocalMark,
            .timestamp = fm.timestamp,
            .data = {
              .filemark = {
                .mark = fm.mark,
                .name = name,
                .fname = (char *)fname,
              }
            },
            .additional_data = fm.additional_data,
          }
        };
        if (fm.timestamp > filemarks->greatest_timestamp) {
          filemarks->greatest_timestamp = fm.timestamp;
        }
      } while (local_marks_iter != NULL);
      for (int i = 0; i < buf->b_changelistlen; i++) {
        const fmark_T fm = buf->b_changelist[i];
        filemarks->changes[i] = (PossiblyFreedShadaEntry) {
          .can_free_entry = false,
          .data = {
            .type = kSDItemChange,
            .timestamp = fm.timestamp,
            .data = {
              .filemark = {
                .mark = fm.mark,
                .fname = (char *)fname,
              }
            },
            .additional_data = fm.additional_data,
          }
        };
        if (fm.timestamp > filemarks->greatest_timestamp) {
          filemarks->greatest_timestamp = fm.timestamp;
        }
      }
      filemarks->changes_size = (size_t)buf->b_changelistlen;
    }
  }

  if (sd_reader != NULL) {
    const ShaDaWriteResult srww_ret = shada_read_when_writing(sd_reader, srni_flags, max_kbyte, wms,
                                                              &packer);
    if (srww_ret != kSDWriteSuccessful) {
      ret = srww_ret;
    }
  }

  // Update numbered marks: replace '0 mark with the current position,
  // remove '9 and shift all other marks. Skip if f0 in 'shada'.
  if (dump_global_marks && !ignore_buf(curbuf, &removable_bufs) && curwin->w_cursor.lnum != 0) {
    replace_numbered_mark(wms, 0, (PossiblyFreedShadaEntry) {
      .can_free_entry = false,
      .data = {
        .type = kSDItemGlobalMark,
        .timestamp = os_time(),
        .data = {
          .filemark = {
            .mark = curwin->w_cursor,
            .name = '0',
            .fname = curbuf->b_ffname,
          }
        },
        .additional_data = NULL,
      },
    });
  }

  // Write the rest
#define PACK_WMS_ARRAY(wms_array) \
  do { \
    for (size_t i_ = 0; i_ < ARRAY_SIZE(wms_array); i_++) { \
      if ((wms_array)[i_].data.type != kSDItemMissing) { \
        if (shada_pack_pfreed_entry(&packer, (wms_array)[i_], max_kbyte) \
            == kSDWriteFailed) { \
          ret = kSDWriteFailed; \
          goto shada_write_exit; \
        } \
      } \
    } \
  } while (0)
  PACK_WMS_ARRAY(wms->global_marks);
  PACK_WMS_ARRAY(wms->numbered_marks);
  PACK_WMS_ARRAY(wms->registers);
  for (size_t i = 0; i < wms->jumps_size; i++) {
    if (shada_pack_pfreed_entry(&packer, wms->jumps[i], max_kbyte)
        == kSDWriteFailed) {
      ret = kSDWriteFailed;
      goto shada_write_exit;
    }
  }
#define PACK_WMS_ENTRY(wms_entry) \
  do { \
    if ((wms_entry).data.type != kSDItemMissing) { \
      if (shada_pack_pfreed_entry(&packer, wms_entry, max_kbyte) \
          == kSDWriteFailed) { \
        ret = kSDWriteFailed; \
        goto shada_write_exit; \
      } \
    } \
  } while (0)
  PACK_WMS_ENTRY(wms->search_pattern);
  PACK_WMS_ENTRY(wms->sub_search_pattern);
  PACK_WMS_ENTRY(wms->replacement);
#undef PACK_WMS_ENTRY

  const size_t file_markss_size = map_size(&wms->file_marks);
  FileMarks **const all_file_markss =
    xmalloc(file_markss_size * sizeof(*all_file_markss));
  FileMarks **cur_file_marks = all_file_markss;
  ptr_t val;
  map_foreach_value(&wms->file_marks, val, {
    *cur_file_marks++ = val;
  })
  qsort((void *)all_file_markss, file_markss_size, sizeof(*all_file_markss),
        &compare_file_marks);
  const size_t file_markss_to_dump = MIN(num_marked_files, file_markss_size);
  for (size_t i = 0; i < file_markss_to_dump; i++) {
    PACK_WMS_ARRAY(all_file_markss[i]->marks);
    for (size_t j = 0; j < all_file_markss[i]->changes_size; j++) {
      if (shada_pack_pfreed_entry(&packer, all_file_markss[i]->changes[j],
                                  max_kbyte) == kSDWriteFailed) {
        ret = kSDWriteFailed;
        goto shada_write_exit;
      }
    }
    for (size_t j = 0; j < all_file_markss[i]->additional_marks_size; j++) {
      if (shada_pack_entry(&packer, all_file_markss[i]->additional_marks[j],
                           0) == kSDWriteFailed) {
        shada_free_shada_entry(&all_file_markss[i]->additional_marks[j]);
        ret = kSDWriteFailed;
        goto shada_write_exit;
      }
      shada_free_shada_entry(&all_file_markss[i]->additional_marks[j]);
    }
    xfree(all_file_markss[i]->additional_marks);
  }
  xfree(all_file_markss);
#undef PACK_WMS_ARRAY

  if (dump_history) {
    for (int i = 0; i < HIST_COUNT; i++) {
      if (dump_one_history[i]) {
        hms_insert_whole_neovim_history(&wms->hms[i]);
        HMS_ITER(&wms->hms[i], cur_entry, {
          if (shada_pack_pfreed_entry(&packer, (PossiblyFreedShadaEntry) {
            .data = cur_entry->data,
            .can_free_entry = cur_entry->can_free_entry,
          }, max_kbyte) == kSDWriteFailed) {
            ret = kSDWriteFailed;
            break;
          }
        })
        if (ret == kSDWriteFailed) {
          goto shada_write_exit;
        }
      }
    }
  }

shada_write_exit:
  for (int i = 0; i < HIST_COUNT; i++) {
    if (dump_one_history[i]) {
      hms_dealloc(&wms->hms[i]);
    }
  }
  const char *stored_key = NULL;
  map_foreach(&wms->file_marks, stored_key, val, {
    xfree((char *)stored_key);
    xfree(val);
  })
  map_destroy(cstr_t, &wms->file_marks);
  set_destroy(ptr_t, &removable_bufs);
  packer.packer_flush(&packer);
  set_destroy(cstr_t, &wms->dumped_variables);
  xfree(wms);
  return ret;
}

#undef PACK_KEY

/// Write ShaDa file to a given location
///
/// @param[in]  fname    File to write to. If it is NULL or empty then default
///                      location is used.
/// @param[in]  nomerge  If true then old file is ignored.
///
/// @return OK if writing was successful, FAIL otherwise.
int shada_write_file(const char *const file, bool nomerge)
{
  char *const fname = shada_filename(file);
  if (fname == NULL) {
    return FAIL;
  }

  char *tempname = NULL;
  FileDescriptor sd_writer;
  FileDescriptor sd_reader;
  bool did_open_writer = false;
  bool did_open_reader = false;

  if (!nomerge) {
    int error;
    if ((error = file_open(&sd_reader, fname, kFileReadOnly, 0)) != 0) {
      if (error != UV_ENOENT) {
        semsg(_(SERR "System error while opening ShaDa file %s for reading "
                "to merge before writing it: %s"),
              fname, os_strerror(error));
        // Try writing the file even if opening it emerged any issues besides
        // file not existing: maybe writing will succeed nevertheless.
      }
      nomerge = true;
      goto shada_write_file_nomerge;
    } else {
      did_open_reader = true;
    }
    tempname = modname(fname, ".tmp.a", false);
    if (tempname == NULL) {
      nomerge = true;
      goto shada_write_file_nomerge;
    }

    // Save permissions from the original file, with modifications:
    int perm = (int)os_getperm(fname);
    perm = (perm >= 0) ? ((perm & 0777) | 0600) : 0600;
    //                 ^3         ^1       ^2      ^2,3
    // 1: Strip SUID bit if any.
    // 2: Make sure that user can always read and write the result.
    // 3: If somebody happened to delete the file after it was opened for
    //    reading use u=rw permissions.
shada_write_file_open: {}
    error = file_open(&sd_writer, tempname, kFileCreateOnly|kFileNoSymlink, perm);
    if (error) {
      if (error == UV_EEXIST || error == UV_ELOOP) {
        // File already exists, try another name
        char *const wp = tempname + strlen(tempname) - 1;
        if (*wp == 'z') {
          // Tried names from .tmp.a to .tmp.z, all failed. Something must be
          // wrong then.
          semsg(_("E138: All %s.tmp.X files exist, cannot write ShaDa file!"),
                fname);
          xfree(fname);
          xfree(tempname);
          if (did_open_reader) {
            close_file(&sd_reader);
          }
          return FAIL;
        }
        (*wp)++;
        goto shada_write_file_open;
      } else {
        semsg(_(SERR "System error while opening temporary ShaDa file %s "
                "for writing: %s"), tempname, os_strerror(error));
      }
    } else {
      did_open_writer = true;
    }
  }
  if (nomerge) {
shada_write_file_nomerge: {}
    char *const tail = path_tail_with_sep(fname);
    if (tail != fname) {
      const char tail_save = *tail;
      *tail = NUL;
      if (!os_isdir(fname)) {
        int ret;
        char *failed_dir;
        if ((ret = os_mkdir_recurse(fname, 0700, &failed_dir, NULL)) != 0) {
          semsg(_(SERR "Failed to create directory %s "
                  "for writing ShaDa file: %s"),
                failed_dir, os_strerror(ret));
          xfree(fname);
          xfree(failed_dir);
          return FAIL;
        }
      }
      *tail = tail_save;
    }
    int error = file_open(&sd_writer, fname, kFileCreate|kFileTruncate, 0600);
    if (error) {
      semsg(_(SERR "System error while opening ShaDa file %s for writing: %s"),
            fname, os_strerror(error));
    } else {
      did_open_writer = true;
    }
  }

  if (!did_open_writer) {
    xfree(fname);
    xfree(tempname);
    if (did_open_reader) {
      close_file(&sd_reader);
    }
    return FAIL;
  }

  if (p_verbose > 1) {
    verbose_enter();
    smsg(0, _("Writing ShaDa file \"%s\""), fname);
    verbose_leave();
  }

  const ShaDaWriteResult sw_ret = shada_write(&sd_writer, (nomerge ? NULL : &sd_reader));
  assert(sw_ret != kSDWriteIgnError);
  if (!nomerge) {
    if (did_open_reader) {
      close_file(&sd_reader);
    }
    bool did_remove = false;
    if (sw_ret == kSDWriteSuccessful) {
      FileInfo old_info;
      if (!os_fileinfo(fname, &old_info)
          || S_ISDIR(old_info.stat.st_mode)
#ifdef UNIX
          // For Unix we check the owner of the file.  It's not very nice
          // to overwrite a user's viminfo file after a "su root", with a
          // viminfo file that the user can't read.
          || (getuid() != ROOT_UID
              && !(old_info.stat.st_uid == getuid()
                   ? (old_info.stat.st_mode & 0200)
                   : (old_info.stat.st_gid == getgid()
                      ? (old_info.stat.st_mode & 0020)
                      : (old_info.stat.st_mode & 0002))))
#endif
          ) {
        semsg(_("E137: ShaDa file is not writable: %s"), fname);
        goto shada_write_file_did_not_remove;
      }
#ifdef UNIX
      if (getuid() == ROOT_UID) {
        if (old_info.stat.st_uid != ROOT_UID
            || old_info.stat.st_gid != getgid()) {
          const uv_uid_t old_uid = (uv_uid_t)old_info.stat.st_uid;
          const uv_gid_t old_gid = (uv_gid_t)old_info.stat.st_gid;
          const int fchown_ret = os_fchown(file_fd(&sd_writer),
                                           old_uid, old_gid);
          if (fchown_ret != 0) {
            semsg(_(RNERR "Failed setting uid and gid for file %s: %s"),
                  tempname, os_strerror(fchown_ret));
            goto shada_write_file_did_not_remove;
          }
        }
      }
#endif
      if (vim_rename(tempname, fname) == -1) {
        semsg(_(RNERR "Can't rename ShaDa file from %s to %s!"),
              tempname, fname);
      } else {
        did_remove = true;
        os_remove(tempname);
      }
    } else {
      if (sw_ret == kSDWriteReadNotShada) {
        semsg(_(RNERR "Did not rename %s because %s "
                "does not look like a ShaDa file"), tempname, fname);
      } else {
        semsg(_(RNERR "Did not rename %s to %s because there were errors "
                "during writing it"), tempname, fname);
      }
    }
    if (!did_remove) {
shada_write_file_did_not_remove:
      semsg(_(RNERR "Do not forget to remove %s or rename it manually to %s."),
            tempname, fname);
    }
    xfree(tempname);
  }
  close_file(&sd_writer);

  xfree(fname);
  return OK;
}

/// Read marks information from ShaDa file
///
/// @return OK in case of success, FAIL otherwise.
int shada_read_marks(void)
{
  return shada_read_file(NULL, kShaDaWantMarks);
}

/// Read all information from ShaDa file
///
/// @param[in]  fname    File to write to. If it is NULL or empty then default
/// @param[in]  forceit  If true, use forced reading (prioritize file contents
///                      over current Neovim state).
/// @param[in]  missing_ok  If true, do not error out when file is missing.
///
/// @return OK in case of success, FAIL otherwise.
int shada_read_everything(const char *const fname, const bool forceit, const bool missing_ok)
{
  return shada_read_file(fname,
                         kShaDaWantInfo|kShaDaWantMarks|kShaDaGetOldfiles
                         |(forceit ? kShaDaForceit : 0)
                         |(missing_ok ? 0 : kShaDaMissingError));
}

static void shada_free_shada_entry(ShadaEntry *const entry)
{
  if (entry == NULL) {
    return;
  }
  switch (entry->type) {
  case kSDItemMissing:
    break;
  case kSDItemUnknown:
    xfree(entry->data.unknown_item.contents);
    break;
  case kSDItemHeader:
    api_free_dict(entry->data.header);
    break;
  case kSDItemChange:
  case kSDItemJump:
  case kSDItemGlobalMark:
  case kSDItemLocalMark:
    xfree(entry->data.filemark.fname);
    break;
  case kSDItemSearchPattern:
    api_free_string(entry->data.search_pattern.pat);
    break;
  case kSDItemRegister:
    for (size_t i = 0; i < entry->data.reg.contents_size; i++) {
      api_free_string(entry->data.reg.contents[i]);
    }
    xfree(entry->data.reg.contents);
    break;
  case kSDItemHistoryEntry:
    xfree(entry->data.history_item.string);
    break;
  case kSDItemVariable:
    xfree(entry->data.global_var.name);
    tv_clear(&entry->data.global_var.value);
    break;
  case kSDItemSubString:
    xfree(entry->data.sub_string.sub);
    break;
  case kSDItemBufferList:
    for (size_t i = 0; i < entry->data.buffer_list.size; i++) {
      xfree(entry->data.buffer_list.buffers[i].fname);
      xfree(entry->data.buffer_list.buffers[i].additional_data);
    }
    xfree(entry->data.buffer_list.buffers);
    break;
  }
  XFREE_CLEAR(entry->additional_data);
}

#ifndef HAVE_BE64TOH
static inline uint64_t be64toh(uint64_t big_endian_64_bits)
{
# ifdef ORDER_BIG_ENDIAN
  return big_endian_64_bits;
# else
  // It may appear that when !defined(ORDER_BIG_ENDIAN) actual order is big
  // endian. This variant is suboptimal, but it works regardless of actual
  // order.
  uint8_t *buf = (uint8_t *)&big_endian_64_bits;
  uint64_t ret = 0;
  for (size_t i = 8; i; i--) {
    ret |= ((uint64_t)buf[i - 1]) << ((8 - i) * 8);
  }
  return ret;
# endif
}
#endif

/// Read given number of bytes into given buffer, display error if needed
///
/// @param[in]   sd_reader  Structure containing file reader definition.
/// @param[out]  buffer     Where to save the results.
/// @param[in]   length     How many bytes should be read.
///
/// @return kSDReadStatusSuccess if everything was OK, kSDReadStatusNotShaDa if
///         there were not enough bytes to read or kSDReadStatusReadError if
///         there was some error while reading.
static ShaDaReadResult fread_len(FileDescriptor *const sd_reader, char *const buffer,
                                 const size_t length)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_WARN_UNUSED_RESULT
{
  const ptrdiff_t read_bytes = file_read(sd_reader, buffer, length);
  if (read_bytes < 0) {
    semsg(_(SERR "System error while reading ShaDa file: %s"),
          os_strerror((int)read_bytes));
    return kSDReadStatusReadError;
  }

  if (read_bytes != (ptrdiff_t)length) {
    semsg(_(RCERR "Error while reading ShaDa file: "
            "last entry specified that it occupies %" PRIu64 " bytes, "
            "but file ended earlier"),
          (uint64_t)length);
    return kSDReadStatusNotShaDa;
  }
  return kSDReadStatusSuccess;
}

/// Read next unsigned integer from file
///
/// Errors out if the result is not an unsigned integer.
///
/// Unlike msgpack own function this one works with `FILE *` and reads *exactly*
/// as much bytes as needed, making it possible to avoid both maintaining own
/// buffer and calling `fseek`.
///
/// One byte from file stream is always consumed, even if it is not correct.
///
/// @param[in]   sd_reader  Structure containing file reader definition.
/// @param[out]  result     Location where result is saved.
///
/// @return kSDReadStatusSuccess if reading was successful,
///         kSDReadStatusNotShaDa if there were not enough bytes to read or
///         kSDReadStatusReadError if reading failed for whatever reason.
///         kSDReadStatusFinished if eof and that was allowed
static ShaDaReadResult msgpack_read_uint64(FileDescriptor *const sd_reader, bool allow_eof,
                                           uint64_t *const result)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_WARN_UNUSED_RESULT
{
  const uintmax_t fpos = sd_reader->bytes_read;

  uint8_t ret;
  ptrdiff_t read_bytes = file_read(sd_reader, (char *)&ret, 1);

  if (read_bytes < 0) {
    semsg(_(SERR "System error while reading integer from ShaDa file: %s"),
          os_strerror((int)read_bytes));
    return kSDReadStatusReadError;
  } else if (read_bytes == 0) {
    if (allow_eof && file_eof(sd_reader)) {
      return kSDReadStatusFinished;
    }
    semsg(_(RCERR "Error while reading ShaDa file: "
            "expected positive integer at position %" PRIu64
            ", but got nothing"),
          (uint64_t)fpos);
    return kSDReadStatusNotShaDa;
  }

  int first_char = (int)ret;
  if (~first_char & 0x80) {
    // Positive fixnum
    *result = (uint64_t)((uint8_t)first_char);
  } else {
    size_t length = 0;
    switch (first_char) {
    case 0xCC:    // uint8
      length = 1;
      break;
    case 0xCD:    // uint16
      length = 2;
      break;
    case 0xCE:    // uint32
      length = 4;
      break;
    case 0xCF:    // uint64
      length = 8;
      break;
    default:
      semsg(_(RCERR "Error while reading ShaDa file: "
              "expected positive integer at position %" PRIu64),
            (uint64_t)fpos);
      return kSDReadStatusNotShaDa;
    }
    uint64_t buf = 0;
    char *buf_u8 = (char *)&buf;
    ShaDaReadResult fl_ret;
    if ((fl_ret = fread_len(sd_reader, &(buf_u8[sizeof(buf) - length]), length))
        != kSDReadStatusSuccess) {
      return fl_ret;
    }
    *result = be64toh(buf);
  }
  return kSDReadStatusSuccess;
}

#define READERR(entry_name, error_desc) \
  RERR "Error while reading ShaDa file: " \
  entry_name " entry at position %" PRIu64 " " \
  error_desc

/// Iterate over shada file contents
///
/// @param[in]   sd_reader  Structure containing file reader definition.
/// @param[out]  entry      Address where next entry contents will be saved.
/// @param[in]   flags      Flags, determining whether and which items should be
///                         skipped (see SRNIFlags enum).
/// @param[in]   max_kbyte  If non-zero, skip reading entries which have length
///                         greater then given.
///
/// @return Any value from ShaDaReadResult enum.
static ShaDaReadResult shada_read_next_item(FileDescriptor *const sd_reader,
                                            ShadaEntry *const entry, const unsigned flags,
                                            const size_t max_kbyte)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_WARN_UNUSED_RESULT
{
  ShaDaReadResult ret = kSDReadStatusMalformed;
shada_read_next_item_start:
  // Set entry type to kSDItemMissing and also make sure that all pointers in
  // data union are NULL so they are safe to xfree(). This is needed in case
  // somebody calls goto shada_read_next_item_error before anything is set in
  // the switch.
  CLEAR_POINTER(entry);
  if (file_eof(sd_reader)) {
    return kSDReadStatusFinished;
  }

  bool verify_but_ignore = false;

  // First: manually unpack type, timestamp and length.
  // This is needed to avoid both seeking and having to maintain a buffer.
  uint64_t type_u64 = (uint64_t)kSDItemMissing;
  uint64_t timestamp_u64;
  uint64_t length_u64;

  const uint64_t initial_fpos = sd_reader->bytes_read;
  AdditionalDataBuilder ad = KV_INITIAL_VALUE;
  uint32_t read_additional_array_elements = 0;
  char *error_alloc = NULL;

  ShaDaReadResult mru_ret;
  if (((mru_ret = msgpack_read_uint64(sd_reader, true, &type_u64))
       != kSDReadStatusSuccess)
      || ((mru_ret = msgpack_read_uint64(sd_reader, false,
                                         &timestamp_u64))
          != kSDReadStatusSuccess)
      || ((mru_ret = msgpack_read_uint64(sd_reader, false,
                                         &length_u64))
          != kSDReadStatusSuccess)) {
    return mru_ret;
  }

  if (length_u64 > PTRDIFF_MAX) {
    semsg(_(RCERR "Error while reading ShaDa file: "
            "there is an item at position %" PRIu64 " "
            "that is stated to be too long"),
          initial_fpos);
    return kSDReadStatusNotShaDa;
  }

  const size_t length = (size_t)length_u64;
  entry->timestamp = (Timestamp)timestamp_u64;

  if (type_u64 == 0) {
    // kSDItemUnknown cannot possibly pass that far because it is -1 and that
    // will fail in msgpack_read_uint64. But kSDItemMissing may and it will
    // otherwise be skipped because (1 << 0) will never appear in flags.
    semsg(_(RCERR "Error while reading ShaDa file: "
            "there is an item at position %" PRIu64 " "
            "that must not be there: Missing items are "
            "for internal uses only"),
          initial_fpos);
    return kSDReadStatusNotShaDa;
  }

  if ((type_u64 > SHADA_LAST_ENTRY
       ? !(flags & kSDReadUnknown)
       : !((unsigned)(1 << type_u64) & flags))
      || (max_kbyte && length > max_kbyte * 1024)) {
    // First entry is unknown or equal to "\n" (10)? Most likely this means that
    // current file is not a ShaDa file because first item should normally be
    // a header (excluding tests where first item is tested item). Check this by
    // parsing entry contents: in non-ShaDa files this will most likely result
    // in incomplete MessagePack string.
    if (initial_fpos == 0
        && (type_u64 == '\n' || type_u64 > SHADA_LAST_ENTRY)) {
      verify_but_ignore = true;
    } else {
      const ShaDaReadResult srs_ret = sd_reader_skip(sd_reader, length);
      if (srs_ret != kSDReadStatusSuccess) {
        return srs_ret;
      }
      goto shada_read_next_item_start;
    }
  }

  const uint64_t parse_pos = sd_reader->bytes_read;
  bool buf_allocated = false;
  // try to avoid allocation for small items which fits entirely
  // in the internal buffer of sd_reader
  char *buf = file_try_read_buffered(sd_reader, length);
  if (!buf) {
    buf_allocated = true;
    buf = xmalloc(length);
    const ShaDaReadResult fl_ret = fread_len(sd_reader, buf, length);
    if (fl_ret != kSDReadStatusSuccess) {
      ret = fl_ret;
      goto shada_read_next_item_error;
    }
  }

  const char *read_ptr = buf;
  size_t read_size = length;

  if (verify_but_ignore) {
    int status = unpack_skip(&read_ptr, &read_size);
    ShaDaReadResult spm_ret = shada_check_status(parse_pos, status, read_size);
    if (buf_allocated) {
      xfree(buf);
    }
    if (spm_ret != kSDReadStatusSuccess) {
      return spm_ret;
    }
    goto shada_read_next_item_start;
  }

  if (type_u64 > SHADA_LAST_ENTRY) {
    entry->type = kSDItemUnknown;
    entry->data.unknown_item.size = length;
    entry->data.unknown_item.type = type_u64;
    if (initial_fpos == 0) {
      int status = unpack_skip(&read_ptr, &read_size);
      ShaDaReadResult spm_ret = shada_check_status(parse_pos, status, read_size);
      if (spm_ret != kSDReadStatusSuccess) {
        if (buf_allocated) {
          xfree(buf);
        }
        entry->type = kSDItemMissing;
        return spm_ret;
      }
    }
    entry->data.unknown_item.contents = buf_allocated ? buf : xmemdup(buf, length);
    return kSDReadStatusSuccess;
  }

  entry->data = sd_default_values[type_u64].data;
  switch ((ShadaEntryType)type_u64) {
  case kSDItemHeader:
    // TODO(bfredl): header is written to file and provides useful debugging
    // info. It is never read by nvim (earlier we parsed it back to a
    // Dict, but that value was never used)
    break;
  case kSDItemSearchPattern: {
    Dict(_shada_search_pat) *it = &entry->data.search_pattern;
    if (!unpack_keydict(it, DictHash(_shada_search_pat), &ad, &read_ptr, &read_size,
                        &error_alloc)) {
      semsg(_(READERR("search pattern", "%s")), initial_fpos, error_alloc);
      it->pat = NULL_STRING;
      goto shada_read_next_item_error;
    }

    if (!HAS_KEY(it, _shada_search_pat, sp)) {  // SEARCH_KEY_PAT
      semsg(_(READERR("search pattern", "has no pattern")), initial_fpos);
      goto shada_read_next_item_error;
    }
    entry->data.search_pattern.pat = copy_string(entry->data.search_pattern.pat, NULL);

    break;
  }
  case kSDItemChange:
  case kSDItemJump:
  case kSDItemGlobalMark:
  case kSDItemLocalMark: {
    Dict(_shada_mark) it = { 0 };
    if (!unpack_keydict(&it, DictHash(_shada_mark), &ad, &read_ptr, &read_size, &error_alloc)) {
      semsg(_(READERR("mark", "%s")), initial_fpos, error_alloc);
      goto shada_read_next_item_error;
    }

    if (HAS_KEY(&it, _shada_mark, n)) {
      if (type_u64 == kSDItemJump || type_u64 == kSDItemChange) {
        semsg(_(READERR("mark", "has n key which is only valid for "
                        "local and global mark entries")), initial_fpos);
        goto shada_read_next_item_error;
      }
      entry->data.filemark.name = (char)it.n;
    }

    if (HAS_KEY(&it, _shada_mark, l)) {
      entry->data.filemark.mark.lnum = (linenr_T)it.l;
    }
    if (HAS_KEY(&it, _shada_mark, c)) {
      entry->data.filemark.mark.col = (colnr_T)it.c;
    }
    if (HAS_KEY(&it, _shada_mark, f)) {
      entry->data.filemark.fname = xmemdupz(it.f.data, it.f.size);
    }

    if (entry->data.filemark.fname == NULL) {
      semsg(_(READERR("mark", "is missing file name")), initial_fpos);
      goto shada_read_next_item_error;
    }
    if (entry->data.filemark.mark.lnum <= 0) {
      semsg(_(READERR("mark", "has invalid line number")), initial_fpos);
      goto shada_read_next_item_error;
    }
    if (entry->data.filemark.mark.col < 0) {
      semsg(_(READERR("mark", "has invalid column number")), initial_fpos);
      goto shada_read_next_item_error;
    }
    break;
  }
  case kSDItemRegister: {
    Dict(_shada_register) it = { 0 };
    if (!unpack_keydict(&it, DictHash(_shada_register), &ad, &read_ptr, &read_size, &error_alloc)) {
      semsg(_(READERR("register", "%s")), initial_fpos, error_alloc);
      kv_destroy(it.rc);
      goto shada_read_next_item_error;
    }
    if (it.rc.size == 0) {
      semsg(_(READERR("register",
                      "has " KEY_NAME(REG_KEY_CONTENTS) " key with missing or empty array")),
            initial_fpos);
      goto shada_read_next_item_error;
    }
    entry->data.reg.contents_size = it.rc.size;
    entry->data.reg.contents = xmalloc(it.rc.size * sizeof(String));
    for (size_t j = 0; j < it.rc.size; j++) {
      entry->data.reg.contents[j] = copy_string(it.rc.items[j], NULL);
    }
    kv_destroy(it.rc);

#define REGISTER_VAL(name, loc, type) \
  if (HAS_KEY(&it, _shada_register, name)) { \
    loc = (type)it.name; \
  }
    REGISTER_VAL(REG_KEY_UNNAMED, entry->data.reg.is_unnamed, bool)
    REGISTER_VAL(REG_KEY_TYPE, entry->data.reg.type, uint8_t)
    REGISTER_VAL(KEY_NAME_CHAR, entry->data.reg.name, char)
    REGISTER_VAL(REG_KEY_WIDTH, entry->data.reg.width, size_t)
    break;
  }
  case kSDItemHistoryEntry: {
    ssize_t len = unpack_array(&read_ptr, &read_size);

    if (len < 2) {
      semsg(_(READERR("history", "is not an array with enough elements")), initial_fpos);
      goto shada_read_next_item_error;
    }
    Integer hist_type;
    if (!unpack_integer(&read_ptr, &read_size, &hist_type)) {
      semsg(_(READERR("history", "has wrong history type type")), initial_fpos);
      goto shada_read_next_item_error;
    }
    const String item = unpack_string(&read_ptr, &read_size);
    if (!item.data) {
      semsg(_(READERR("history", "has wrong history string type")), initial_fpos);
      goto shada_read_next_item_error;
    }
    if (memchr(item.data, 0, item.size) != NULL) {
      semsg(_(READERR("history", "contains string with zero byte inside")), initial_fpos);
      goto shada_read_next_item_error;
    }
    entry->data.history_item.histtype = (uint8_t)hist_type;
    const bool is_hist_search = entry->data.history_item.histtype == HIST_SEARCH;
    if (is_hist_search) {
      if (len < 3) {
        semsg(_(READERR("search history",
                        "does not have separator character")), initial_fpos);
        goto shada_read_next_item_error;
      }
      Integer sep_type;
      if (!unpack_integer(&read_ptr, &read_size, &sep_type)) {
        semsg(_(READERR("search history", "has wrong history separator type")), initial_fpos);
        goto shada_read_next_item_error;
      }
      entry->data.history_item.sep = (char)sep_type;
    }
    size_t strsize = (item.size
                      + 1  // Zero byte
                      + 1);  // Separator character
    entry->data.history_item.string = xmalloc(strsize);
    memcpy(entry->data.history_item.string, item.data, item.size);
    entry->data.history_item.string[strsize - 2] = 0;
    entry->data.history_item.string[strsize - 1] = entry->data.history_item.sep;
    read_additional_array_elements = (uint32_t)(len - (2 + is_hist_search));
    break;
  }
  case kSDItemVariable: {
    ssize_t len = unpack_array(&read_ptr, &read_size);

    if (len < 2) {
      semsg(_(READERR("variable", "is not an array with enough elements")), initial_fpos);
      goto shada_read_next_item_error;
    }

    String name = unpack_string(&read_ptr, &read_size);

    if (!name.data) {
      semsg(_(READERR("variable", "has wrong variable name type")), initial_fpos);
      goto shada_read_next_item_error;
    }
    entry->data.global_var.name = xmemdupz(name.data, name.size);

    String binval = unpack_string(&read_ptr, &read_size);

    bool is_blob = false;
    if (binval.data) {
      if (len > 2) {
        // A msgpack BIN could be a String or Blob; an additional VAR_TYPE_BLOB
        // element is stored with Blobs which can be used to differentiate them
        Integer type;
        if (!unpack_integer(&read_ptr, &read_size, &type) || type != VAR_TYPE_BLOB) {
          semsg(_(READERR("variable", "has wrong variable type")),
                initial_fpos);
          goto shada_read_next_item_error;
        }
        is_blob = true;
      }
      entry->data.global_var.value = decode_string(binval.data, binval.size, is_blob, false);
    } else {
      int status = unpack_typval(&read_ptr, &read_size, &entry->data.global_var.value);
      if (status != MPACK_OK) {
        semsg(_(READERR("variable", "has value that cannot "
                        "be converted to the Vimscript value")), initial_fpos);
        goto shada_read_next_item_error;
      }
    }
    read_additional_array_elements = (uint32_t)(len - 2 - (is_blob ? 1 : 0));
    break;
  }
  case kSDItemSubString: {
    ssize_t len = unpack_array(&read_ptr, &read_size);

    if (len < 1) {
      semsg(_(READERR("sub string", "is not an array with enough elements")), initial_fpos);
      goto shada_read_next_item_error;
    }

    String sub = unpack_string(&read_ptr, &read_size);
    if (!sub.data) {
      semsg(_(READERR("sub string", "has wrong sub string type")), initial_fpos);
      goto shada_read_next_item_error;
    }
    entry->data.sub_string.sub = xmemdupz(sub.data, sub.size);
    read_additional_array_elements = (uint32_t)(len - 1);
    break;
  }
  case kSDItemBufferList: {
    ssize_t len = unpack_array(&read_ptr, &read_size);
    if (len < 0) {
      semsg(_(READERR("buffer list", "is not an array")), initial_fpos);
      goto shada_read_next_item_error;
    }
    if (len == 0) {
      break;
    }
    entry->data.buffer_list.buffers = xcalloc((size_t)len,
                                              sizeof(*entry->data.buffer_list.buffers));
    for (size_t i = 0; i < (size_t)len; i++) {
      entry->data.buffer_list.size++;
      Dict(_shada_buflist_item) it = { 0 };
      AdditionalDataBuilder it_ad = KV_INITIAL_VALUE;
      if (!unpack_keydict(&it, DictHash(_shada_buflist_item), &it_ad, &read_ptr, &read_size,
                          &error_alloc)) {
        semsg(_(RERR "Error while reading ShaDa file: "
                "buffer list at position %" PRIu64 " contains entry that %s"),
              initial_fpos, error_alloc);
        kv_destroy(it_ad);
        goto shada_read_next_item_error;
      }
      struct buffer_list_buffer *e = &entry->data.buffer_list.buffers[i];
      e->additional_data = (AdditionalData *)it_ad.items;
      e->pos = default_pos;
      if (HAS_KEY(&it, _shada_buflist_item, l)) {
        e->pos.lnum = (linenr_T)it.l;
      }
      if (HAS_KEY(&it, _shada_buflist_item, c)) {
        e->pos.col = (colnr_T)it.c;
      }
      if (HAS_KEY(&it, _shada_buflist_item, f)) {
        e->fname = xmemdupz(it.f.data, it.f.size);
      }

      if (e->pos.lnum <= 0) {
        semsg(_(RERR "Error while reading ShaDa file: "
                "buffer list at position %" PRIu64 " "
                "contains entry with invalid line number"),
              initial_fpos);
        goto shada_read_next_item_error;
      }
      if (e->pos.col < 0) {
        semsg(_(RERR "Error while reading ShaDa file: "
                "buffer list at position %" PRIu64 " "
                "contains entry with invalid column number"),
              initial_fpos);
        goto shada_read_next_item_error;
      }
      if (e->fname == NULL) {
        semsg(_(RERR "Error while reading ShaDa file: "
                "buffer list at position %" PRIu64 " "
                "contains entry that does not have a file name"),
              initial_fpos);
        goto shada_read_next_item_error;
      }
    }
    break;
  }
  case kSDItemMissing:
  case kSDItemUnknown:
    abort();
  }

  for (uint32_t i = 0; i < read_additional_array_elements; i++) {
    const char *item_start = read_ptr;
    int status = unpack_skip(&read_ptr, &read_size);
    if (status) {
      goto shada_read_next_item_error;
    }

    push_additional_data(&ad, item_start, (size_t)(read_ptr - item_start));
  }

  if (read_size) {
    semsg(_(READERR("item", "additional bytes")), initial_fpos);
    goto shada_read_next_item_error;
  }

  entry->type = (ShadaEntryType)type_u64;
  entry->additional_data = (AdditionalData *)ad.items;
  ret = kSDReadStatusSuccess;
shada_read_next_item_end:
  if (buf_allocated) {
    xfree(buf);
  }
  return ret;
shada_read_next_item_error:
  entry->type = (ShadaEntryType)type_u64;
  shada_free_shada_entry(entry);
  entry->type = kSDItemMissing;
  xfree(error_alloc);
  kv_destroy(ad);
  goto shada_read_next_item_end;
}

/// Check whether "name" is on removable media (according to 'shada')
///
/// @param[in]  name  Checked name.
///
/// @return True if it is, false otherwise.
static bool shada_removable(const char *name)
  FUNC_ATTR_WARN_UNUSED_RESULT
{
  char part[MAXPATHL + 1];
  bool retval = false;

  char *new_name = home_replace_save(NULL, name);
  for (char *p = p_shada; *p;) {
    copy_option_part(&p, part, ARRAY_SIZE(part), ", ");
    if (part[0] == 'r') {
      home_replace(NULL, part + 1, NameBuff, MAXPATHL, true);
      size_t n = strlen(NameBuff);
      if (mb_strnicmp(NameBuff, new_name, n) == 0) {
        retval = true;
        break;
      }
    }
  }
  xfree(new_name);
  return retval;
}

/// Initialize ShaDa jumplist entries.
///
/// @param[in,out]  jumps           Array of ShaDa entries to set.
/// @param[in]      removable_bufs  Cache of buffers ignored due to their
///                                 location.
///
/// @return number of jumplist entries
static inline size_t shada_init_jumps(PossiblyFreedShadaEntry *jumps,
                                      Set(ptr_t) *const removable_bufs)
{
  // Initialize jump list
  size_t jumps_size = 0;
  const void *jump_iter = NULL;
  setpcmark();
  cleanup_jumplist(curwin, false);
  do {
    xfmark_T fm;
    jump_iter = mark_jumplist_iter(jump_iter, curwin, &fm);

    if (fm.fmark.mark.lnum == 0) {
      siemsg("ShaDa: mark lnum zero (ji:%p, js:%p, len:%i)",
             (void *)jump_iter, (void *)&curwin->w_jumplist[0],
             curwin->w_jumplistlen);
      continue;
    }
    const buf_T *const buf = (fm.fmark.fnum == 0 ? NULL : buflist_findnr(fm.fmark.fnum));
    if (buf != NULL ? ignore_buf(buf, removable_bufs) : fm.fmark.fnum != 0) {
      continue;
    }
    const char *const fname =
      (fm.fmark.fnum == 0 ? (fm.fname == NULL ? NULL : fm.fname) : buf ? buf->b_ffname : NULL);
    if (fname == NULL) {
      continue;
    }
    jumps[jumps_size++] = (PossiblyFreedShadaEntry) {
      .can_free_entry = false,
      .data = {
        .type = kSDItemJump,
        .timestamp = fm.fmark.timestamp,
        .data = {
          .filemark = {
            .name = NUL,
            .mark = fm.fmark.mark,
            .fname = (char *)fname,
          }
        },
        .additional_data = fm.fmark.additional_data,
      }
    };
  } while (jump_iter != NULL);
  return jumps_size;
}

/// Write registers ShaDa entries in given msgpack_sbuffer.
///
/// @param[in]  sbuf  target msgpack_sbuffer to write to.
String shada_encode_regs(void)
  FUNC_ATTR_NONNULL_ALL
{
  WriteMergerState *const wms = xcalloc(1, sizeof(*wms));
  shada_initialize_registers(wms, -1);
  PackerBuffer packer = packer_string_buffer();
  for (size_t i = 0; i < ARRAY_SIZE(wms->registers); i++) {
    if (wms->registers[i].data.type == kSDItemRegister) {
      if (kSDWriteFailed
          == shada_pack_pfreed_entry(&packer, wms->registers[i], 0)) {
        abort();
      }
    }
  }
  xfree(wms);
  return packer_take_string(&packer);
}

/// Write jumplist ShaDa entries in given msgpack_sbuffer.
///
/// @param[in]  sbuf            target msgpack_sbuffer to write to.
String shada_encode_jumps(void)
  FUNC_ATTR_NONNULL_ALL
{
  Set(ptr_t) removable_bufs = SET_INIT;
  find_removable_bufs(&removable_bufs);
  PossiblyFreedShadaEntry jumps[JUMPLISTSIZE];
  size_t jumps_size = shada_init_jumps(jumps, &removable_bufs);
  PackerBuffer packer = packer_string_buffer();
  for (size_t i = 0; i < jumps_size; i++) {
    if (kSDWriteFailed == shada_pack_pfreed_entry(&packer, jumps[i], 0)) {
      abort();
    }
  }
  return packer_take_string(&packer);
}

/// Write buffer list ShaDa entry in given msgpack_sbuffer.
///
/// @param[in]  sbuf            target msgpack_sbuffer to write to.
String shada_encode_buflist(void)
  FUNC_ATTR_NONNULL_ALL
{
  Set(ptr_t) removable_bufs = SET_INIT;
  find_removable_bufs(&removable_bufs);
  ShadaEntry buflist_entry = shada_get_buflist(&removable_bufs);

  PackerBuffer packer = packer_string_buffer();
  if (kSDWriteFailed == shada_pack_entry(&packer, buflist_entry, 0)) {
    abort();
  }
  xfree(buflist_entry.data.buffer_list.buffers);
  return packer_take_string(&packer);
}

/// Write global variables ShaDa entries in given msgpack_sbuffer.
///
/// @param[in]  sbuf            target msgpack_sbuffer to write to.
String shada_encode_gvars(void)
  FUNC_ATTR_NONNULL_ALL
{
  PackerBuffer packer = packer_string_buffer();
  const void *var_iter = NULL;
  const Timestamp cur_timestamp = os_time();
  do {
    typval_T vartv;
    const char *name = NULL;
    var_iter = var_shada_iter(var_iter, &name, &vartv,
                              VAR_FLAVOUR_DEFAULT | VAR_FLAVOUR_SESSION | VAR_FLAVOUR_SHADA);
    if (name == NULL) {
      break;
    }
    if (vartv.v_type != VAR_FUNC && vartv.v_type != VAR_PARTIAL) {
      typval_T tgttv;
      tv_copy(&vartv, &tgttv);
      ShaDaWriteResult r = shada_pack_entry(&packer, (ShadaEntry) {
        .type = kSDItemVariable,
        .timestamp = cur_timestamp,
        .data = {
          .global_var = {
            .name = (char *)name,
            .value = tgttv,
          }
        },
        .additional_data = NULL,
      }, 0);
      if (kSDWriteFailed == r) {
        abort();
      }
      tv_clear(&tgttv);
    }
    tv_clear(&vartv);
  } while (var_iter != NULL);
  return packer_take_string(&packer);
}

/// Read ShaDa from String.
///
/// @param[in]  string   string to read from.
/// @param[in]  flags  Flags, see ShaDaReadFileFlags enum.
void shada_read_string(String string, const int flags)
  FUNC_ATTR_NONNULL_ALL
{
  if (string.size == 0) {
    return;
  }
  FileDescriptor sd_reader;
  file_open_buffer(&sd_reader, string.data, string.size);
  shada_read(&sd_reader, flags);
  close_file(&sd_reader);
}

/// Find the parameter represented by the given character (eg ', :, ", or /),
/// and return its associated value in the 'shada' string.
/// Only works for number parameters, not for 'r' or 'n'.
/// If the parameter is not specified in the string or there is no following
/// number, return -1.
int get_shada_parameter(int type)
{
  char *p = find_shada_parameter(type);
  if (p != NULL && ascii_isdigit(*p)) {
    return atoi(p);
  }
  return -1;
}

/// Find the parameter represented by the given character (eg ''', ':', '"', or
/// '/') in the 'shada' option and return a pointer to the string after it.
/// Return NULL if the parameter is not specified in the string.
char *find_shada_parameter(int type)
{
  for (char *p = p_shada; *p; p++) {
    if (*p == type) {
      return p + 1;
    }
    if (*p == 'n') {                // 'n' is always the last one
      break;
    }
    p = vim_strchr(p, ',');         // skip until next ','
    if (p == NULL) {                // hit the end without finding parameter
      break;
    }
  }
  return NULL;
}

/// Read marks for the current buffer from the ShaDa file, when we support
/// buffer marks and the buffer has a name.
void check_marks_read(void)
{
  if (!curbuf->b_marks_read && get_shada_parameter('\'') > 0
      && curbuf->b_ffname != NULL) {
    shada_read_marks();
  }

  // Always set b_marks_read; needed when 'shada' is changed to include
  // the ' parameter after opening a buffer.
  curbuf->b_marks_read = true;
}
