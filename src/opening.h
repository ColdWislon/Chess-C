#pragma once
#include "board.h"

typedef struct OpeningBook OpeningBook;

OpeningBook *book_load(const char *path);
void         book_free(OpeningBook *book);
Move         book_probe(const OpeningBook *book, const Position *pos);
