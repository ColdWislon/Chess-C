#include "opening.h"
#include "poly_keys.h"   /* auto-generated 781 Polyglot random numbers */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Polyglot random number layout ──────────────────────────
   [0..767]  piece keys: poly_piece_idx * 64 + square, where
             poly_piece_idx = (engine_piece) * 2 + (1 if WHITE else 0)
             ⇒ interleaved order (matches python-chess + canonical spec):
               0=bP 1=wP 2=bN 3=wN 4=bB 5=wB
               6=bR 7=wR 8=bQ 9=wQ 10=bK 11=wK
   [768..771] castling: WK WQ BK BQ
   [772..779] en-passant files a..h (only if a pawn could legally capture)
   [780]      side-to-move (XORed when WHITE moves — verified against
              the canonical startpos key 0x463b96181691fc9c which our
              book.bin contains)

   Bug fixed 2026-05-17: previous version used a blocked [bP..bK, wP..wK]
   layout AND inverted side-to-move (`BLACK`). Result: no book hit ever
   fired — the engine silently searched every move of every game despite
   "Opening book loaded." in startup logs.
   ─────────────────────────────────────────────────────────── */

static uint64_t poly_hash(const Position *pos) {
    uint64_t h = 0;

    for (int c = 0; c < 2; c++) {
        int color_off = (c == WHITE) ? 1 : 0;
        for (int p = 0; p < 6; p++) {
            int poly_p = p * 2 + color_off;
            uint64_t bb = pos->pieces[c][p];
            while (bb) {
                int sq = lsb64(bb); bb &= bb-1;
                h ^= POLY[poly_p * 64 + sq];
            }
        }
    }

    /* Castling */
    if (pos->castling & CASTLE_WK) h ^= POLY[768];
    if (pos->castling & CASTLE_WQ) h ^= POLY[769];
    if (pos->castling & CASTLE_BK) h ^= POLY[770];
    if (pos->castling & CASTLE_BQ) h ^= POLY[771];

    /* En passant — only hash if a pawn can actually capture
       (matches EnPassantMode::Legal used by python-chess + standard
       Polyglot generators). */
    if (pos->ep_sq >= 0) {
        int ep_file = FILE_OF(pos->ep_sq);
        bool can_ep;
        if (pos->side == WHITE)
            can_ep = (PAWN_ATTACKS[BLACK][pos->ep_sq] & pos->pieces[WHITE][PAWN]) != 0;
        else
            can_ep = (PAWN_ATTACKS[WHITE][pos->ep_sq] & pos->pieces[BLACK][PAWN]) != 0;
        if (can_ep) h ^= POLY[772 + ep_file];
    }

    /* Side to move */
    if (pos->side == WHITE) h ^= POLY[780];

    return h;
}

/* ── Book entry ── */
typedef struct { uint64_t key; uint16_t move; uint16_t weight; } PolyEntry;

struct OpeningBook {
    PolyEntry *entries;
    size_t     count;
};

static int entry_cmp(const void *a, const void *b) {
    const PolyEntry *ea = a, *eb = b;
    if (ea->key < eb->key) return -1;
    if (ea->key > eb->key) return  1;
    return 0;
}

OpeningBook *book_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Polyglot entries are exactly 16 bytes. Reject empty files and round
       down — many real-world books have a few trailing junk bytes; we read
       only the complete entries and ignore the tail. */
    if (sz < 16) { fclose(f); return NULL; }
    size_t count = (size_t)sz / 16;

    PolyEntry *entries = malloc(count * sizeof(PolyEntry));
    if (!entries) { fclose(f); return NULL; }

    for (size_t i = 0; i < count; i++) {
        uint8_t buf[16];
        if (fread(buf, 1, 16, f) != 16) { free(entries); fclose(f); return NULL; }
        entries[i].key    = (uint64_t)buf[0]<<56|(uint64_t)buf[1]<<48|(uint64_t)buf[2]<<40|(uint64_t)buf[3]<<32
                           |(uint64_t)buf[4]<<24|(uint64_t)buf[5]<<16|(uint64_t)buf[6]<<8|(uint64_t)buf[7];
        entries[i].move   = (uint16_t)((buf[8]<<8)|buf[9]);
        entries[i].weight = (uint16_t)((buf[10]<<8)|buf[11]);
    }
    fclose(f);
    qsort(entries, count, sizeof(PolyEntry), entry_cmp);

    OpeningBook *book = malloc(sizeof(OpeningBook));
    if (!book) { free(entries); return NULL; }
    book->entries = entries;
    book->count   = count;
    return book;
}

void book_free(OpeningBook *book) {
    if (book) { free(book->entries); free(book); }
}

static uint32_t lcg_rand(void) {
    static uint64_t s = 0;
    if (!s) s = (uint64_t)time(NULL) + 1;
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(s >> 33);
}

Move book_probe(const OpeningBook *book, const Position *pos) {
    if (!book) return MOVE_NONE;
    uint64_t key = poly_hash(pos);

    /* binary search for first entry with this key */
    size_t lo = 0, hi = book->count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (book->entries[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= book->count || book->entries[lo].key != key)
        return MOVE_NONE;

    /* collect all entries for this key */
    size_t start = lo;
    uint32_t total = 0;
    size_t end = start;
    while (end < book->count && book->entries[end].key == key)
        total += book->entries[end++].weight;

    if (total == 0) return MOVE_NONE;

    /* weighted random pick */
    uint32_t pick = lcg_rand() % total;
    const PolyEntry *chosen = &book->entries[start];
    for (size_t i = start; i < end; i++) {
        if (pick < book->entries[i].weight) { chosen = &book->entries[i]; break; }
        pick -= book->entries[i].weight;
    }

    /* decode Polyglot move: bits[0..2]=to_file [3..5]=to_rank [6..8]=from_file [9..11]=from_rank [12..14]=promo */
    uint16_t raw   = chosen->move;
    int to_file    = (raw >> 0) & 7;
    int to_rank    = (raw >> 3) & 7;
    int from_file  = (raw >> 6) & 7;
    int from_rank  = (raw >> 9) & 7;
    int poly_promo = (raw >> 12) & 7;

    /* Polyglot promo: 1=N 2=B 3=R 4=Q */
    static const int PROMO_MAP[8] = {
        PIECE_NONE, KNIGHT, BISHOP, ROOK, QUEEN, PIECE_NONE, PIECE_NONE, PIECE_NONE
    };
    int promo_req = PROMO_MAP[poly_promo];

    int from = SQ(from_file, from_rank);
    int to   = SQ(to_file,   to_rank);

    Move legal[MAX_MOVES];
    int n = pos_gen_moves(pos, legal);
    for (int i = 0; i < n; i++) {
        if (MOVE_FROM(legal[i]) == from && MOVE_TO(legal[i]) == to) {
            int mp = MOVE_PROMO(legal[i]);
            if (promo_req == PIECE_NONE || mp == promo_req)
                return legal[i];
        }
    }
    return MOVE_NONE;
}
