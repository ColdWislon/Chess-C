#include "board.h"
#include "opening.h"
#include "search.h"
#include "uci.h"
#include <stdio.h>

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

    uci_run(book);
    book_free(book);
    return 0;
}
