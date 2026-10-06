#include "cart_writer.h"

#include "picker_screen.h"

// The writer is the shared picker screen in one of its writing modes. The
// screen, its buffers and its loop live in src/picker_screen.cpp.
enum boot_pick_e writer_open(enum writer_mode_e mode,
                             const catalog_reader_t* cat,
                             const boot_flags_t* flags, bool pending_set,
                             const char* header, boot_selection_t* out) {
    enum picker_mode_e pm = PICKER_MODE_PENDING;

    switch (mode) {
        case WRITER_MODE_IMMEDIATE:
            pm = PICKER_MODE_IMMEDIATE;
            break;
        case WRITER_MODE_FINISH:
            pm = PICKER_MODE_FINISH;
            break;
        case WRITER_MODE_MAKE_MENU:
            pm = PICKER_MODE_MAKE_MENU;
            break;
        case WRITER_MODE_PENDING:
            break;
    }
    return picker_screen_run(pm, cat, flags, pending_set, header, out);
}
