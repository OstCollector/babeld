#ifndef REACH_BITMAP_H
#define REACH_BITMAP_H

#define MAX_HIST_BYTES (16)
#define MAX_HIST_BITS (MAX_HIST_BYTES * 8)
#define HIST_SIZE_BYTES (MAX_HIST_BYTES + 4)
#define HIST_SIZE_BITS (HIST_SIZE_BYTES * 8)

struct reach_bitmap {
    unsigned char bitmap[HIST_SIZE_BYTES];   /* can process up to */
    
    /* additional space for trailing '\0' */
    char hex[HIST_SIZE_BYTES * 2 + 4];

    char dump[9];

    int begin;      /* index of oldest */ 
    int end;        /* index of the next bit inserted */
    int count;      /* number of bits stored in this structure */
    int count_set;  /* number of bits set in this structure */
    char *begin1;   /* begin of hex represent of 1st segment */
    char *end1;     /* end of hex represent of 1st segment (the '\0' char) */
    char *begin2;   /* begin of hex represent of 2nd segment */
    char *end2;     /* end of hex represent of 2nd segment (the '\0' char) */

    char begin_mask;    /* valid bits of first character */
    char end_mask;      /* valid bits of last character (not '\0') */

    unsigned short old_reach; /* cache reach */
};

/*
 * Initialize a reach bitmap
 */
void reach_bitmap_init(struct reach_bitmap *bitmap);

/*
 * get the newest several bits, and perform AND option with _mask_
 * differ from current implements, LSB is the newest bit
 * (current implement use MSB as the newest bit)
 * accept and return 16 bits as maximal (may should use uint16_t instead)
 */
unsigned short reach_bitmap_get_mask(
        const struct reach_bitmap *bitmap, unsigned short mask);

/*
 * push new value into the bitmap, simulate 
 *     reach >>= 1; reach |= 0x8000
 *   and
 *     reach >>= missed_hellos
 * MSB is the oldest and LSB is the newest.
 * only lowest _length_ bits are used
 * if count is larger than limit, pop oldest
 */
void reach_bitmap_push_new(
        struct reach_bitmap *bitmap, unsigned short value, int length);

/*
 * pop some newest values from bitmap, simulate
 *     reach <<= -missed_hellos
 * older values are not pushed back
 */
void reach_bitmap_pop_new(struct reach_bitmap *bitmap, int length);

/*
 * test if received two of last three hello messages
 */
int reach_bitmap_two_three(struct reach_bitmap *bitmap);

/*
 * get metrics of the node, 
 */
unsigned reach_bitmap_metric(const struct reach_bitmap *bitmap,
        short line_cost, unsigned int delay);


#endif
