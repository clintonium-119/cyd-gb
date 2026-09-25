#include "cart_writer.h"

#include "picker_screen.h"

// The writer is the shared picker screen in one of its two writing modes. The
// screen, its buffers and its loop live in src/picker_screen.cpp.
enum boot_pick_e writer_open(enum writer_mode_e mode,
                             const catalog_reader_t* cat,
                             const boot_flags_t* flags, bool pending_set,
                             boot_selection_t* out) {
    return picker_screen_run((mode == WRITER_MODE_IMMEDIATE)
                                 ? PICKER_MODE_IMMEDIATE
                                 : PICKER_MODE_PENDING,
                             cat, flags, pending_set, out);
}
