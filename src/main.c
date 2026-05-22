#include "board.h"
#include "opening.h"
#include "search.h"
#include "syzygy.h"
#include "uci.h"
#include <stdio.h>

#define DEFAULT_SYZYGY_PATH "/home/bertrand/syzygy/"

int main(void) {
    board_init();
    search_init();

    OpeningBook *book = book_load("/home/bertrand/chess-c/book.bin");
    if (!book)
        book = book_load("book.bin");

    if (book)
        fprintf(stderr, "Opening book loaded.\n");
    else
        fprintf(stderr, "No opening book found — running without.\n");

    /* Best-effort default load: silent on miss so the engine still runs on a
       host with no tablebase files. UCI `setoption SyzygyPath` overrides. */
    if (syzygy_init(DEFAULT_SYZYGY_PATH))
        fprintf(stderr, "Syzygy tablebases loaded (up to %d-piece) from %s\n",
                syzygy_largest(), DEFAULT_SYZYGY_PATH);
    else
        fprintf(stderr, "Syzygy: no tablebase files at %s — running without.\n",
                DEFAULT_SYZYGY_PATH);

    uci_run(book);
    syzygy_shutdown();
    book_free(book);
    return 0;
}
