#include <limits.h>

#include "babeld.h"
#include "reach_bitmap.h"

#include <stdio.h>

static void update_ends(struct reach_bitmap *bitmap);
static char get_begin_mask(int begin);
static char get_end_mask(int end);
static int distance(int begin, int end, int length);
static int at_abs(const struct reach_bitmap *bitmap, int offset);
/* count from newest, begin from 1 */
static int at_rel(const struct reach_bitmap *bitmap, int offset);
static int countset_byte(unsigned char byte);

const static char hex_map[16] = {
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
};


void reach_bitmap_init(struct reach_bitmap *bitmap)
{
    bitmap->begin = 0;
    bitmap->end = 0;
    bitmap->count_set = 0;
    update_ends(bitmap);
    bitmap->hex[HIST_SIZE_BYTES * 2 + 2] = '-';
    bitmap->hex[HIST_SIZE_BYTES * 2 + 3] = '\0';
}

unsigned short reach_bitmap_get_mask(const struct reach_bitmap *bitmap,
        unsigned short mask)
{
    unsigned long result, length_mask;
    int shift;

    if (bitmap->count == 0) {
        return 0;
    }

    /* get a long first, then truncate it */
    int byte_index = (bitmap->end + 7) / 8 - 1;
    byte_index = (byte_index + HIST_SIZE_BYTES) % HIST_SIZE_BYTES;
    result = bitmap->bitmap[byte_index];
    
    --byte_index;
    byte_index = (byte_index + HIST_SIZE_BYTES) % HIST_SIZE_BYTES;
    result |= (long)bitmap->bitmap[byte_index] << 8;

    --byte_index;
    byte_index = (byte_index + HIST_SIZE_BYTES) % HIST_SIZE_BYTES;
    result |= (long)bitmap->bitmap[byte_index] << 16;

    shift = bitmap->end % 8;
    shift = (8 - shift) % 8;

    /* truncate */
    result >>= shift;
    if (bitmap->count < CHAR_BIT * sizeof(result)) {
        length_mask = (unsigned long)0;
        length_mask = ~length_mask;
        length_mask >>= (CHAR_BIT * sizeof(result) - bitmap->count);
        result &= length_mask;
    }

    return result & mask;
}

void reach_bitmap_push_new(struct reach_bitmap *bitmap,
        unsigned short value, int length)
{
    unsigned char t;
    int cur_bits;
    int exist_bits;
    unsigned char *cur;
    int byte_index;

    /* ignore when bad input or nop */
    if (length <= 0 || length > CHAR_BIT * sizeof(value)) {
        return;
    }

    value &= (unsigned short)0xFFFF >> (CHAR_BIT * sizeof(value) - length);

    /* drop bits */
    while (distance(bitmap->begin, bitmap->end + length, HIST_SIZE_BITS)
            > MAX_HIST_BITS) {
        bitmap->count_set -= at_abs(bitmap, bitmap->begin);
        ++bitmap->begin;
        bitmap->begin = bitmap->begin % HIST_SIZE_BITS;
    }

    /*
     * for each round, move cur_bits off value into bitmap
     * 
     * +-------------------------------------+
     * |            | cur_bits |             |
     * +-------------------------------------+
     *              +-----+  length  +-------+
     * 
     *                   +-------------------+
     *                   |   | cur_bits |    |
     *                   +-------------------+
     *                   +   +
     *              exist_bits
     */
    while (length > 0) {
        exist_bits = bitmap->end % 8;
        cur_bits = 8 - exist_bits;

        if (cur_bits > length) {
            cur_bits = length;
        }

        t = value >> (length - cur_bits);
        t &= (0xFF) >> (8 - cur_bits);
        t <<= (8 - cur_bits - exist_bits);

        byte_index = bitmap->end / 8;
        cur = &bitmap->bitmap[byte_index];
        *cur = *cur & (0xFF << (8 - exist_bits));
        *cur = *cur | t;
        bitmap->count_set += countset_byte(t);

        bitmap->hex[byte_index * 2] = hex_map[*cur / 16];
        bitmap->hex[byte_index * 2 + 1] = hex_map[*cur % 16];

        bitmap->end += cur_bits;
        bitmap->end %= HIST_SIZE_BITS;
        length -= cur_bits;
    }

    update_ends(bitmap);
}

void reach_bitmap_pop_new(struct reach_bitmap *bitmap, int length)
{
    int i;
    if (bitmap->count < length) {
        length = bitmap->count;
    }
    for (i = 0; i < length; ++i) {
        bitmap->end = (bitmap->end + HIST_SIZE_BITS - 1) % HIST_SIZE_BITS;
        bitmap->count_set -= at_abs(bitmap, bitmap->end);
    }

    update_ends(bitmap);
}

int reach_bitmap_two_three(struct reach_bitmap *bitmap)
{
    return (at_rel(bitmap, 1) + at_rel(bitmap, 2) + at_rel(bitmap, 3)) >= 2;
}

unsigned reach_bitmap_metric(const struct reach_bitmap *bitmap,
        short line_cost, unsigned int delay)
{
    long long cost = 0;
    int last_bits = countset_byte(bitmap->old_reach >> 8) +
        countset_byte(bitmap->old_reach);
    long last_minval = 10000;
    if (last_bits < 14) {
        last_minval = 10000 + 20000 * (14 - last_bits);
    }
    
    // drop rate:
    //  at   0.0%:    1.0
    //  at  10.0%:    2.0
    //  at  16.0%:    8.0
    //  at  20.0%:   40.0
    //  at 100.0%: 1000.0
    long long drop_of_1e4 = (long long)(bitmap->count - bitmap->count_set)
        * 10000 / (bitmap->count);
    long multipler_1e4;
    if (drop_of_1e4 < 1000) {
        multipler_1e4 = 10000 + 10 * drop_of_1e4;
    } else if (drop_of_1e4 < 1600) {
        multipler_1e4 = 20000 + 100 * (drop_of_1e4 - 1000);
    } else if (drop_of_1e4 < 2000) {
        multipler_1e4 = 80000 + 80000 * (drop_of_1e4 - 1600);
    } else {
        multipler_1e4 = 400000 + 120000 * (drop_of_1e4 - 2000);
    }

    if (multipler_1e4 < last_minval) {
        multipler_1e4 = last_minval;
    }
    cost = line_cost * multipler_1e4 / 10000;
    if (delay > 40000) {
        cost = (cost * (delay - 20000) + 10000) / 20000;
    }
    if (cost > 0xffff) {
        return 0xffff;
    } else {
        return cost;
    }
}


static void update_ends(struct reach_bitmap *bitmap)
{
    bitmap->begin_mask = get_begin_mask(bitmap->begin);
    bitmap->count = bitmap->end - bitmap->begin;
    bitmap->count += CHAR_BIT * sizeof(bitmap->bitmap);
    bitmap->count %= CHAR_BIT * sizeof(bitmap->bitmap);

    sprintf(bitmap->dump, "%x", reach_bitmap_get_mask(bitmap, 0xffff));

    if (bitmap->count != 0) {
         if (bitmap->begin <= bitmap->end) {    /* single included */
             bitmap->begin1 = &bitmap->hex[bitmap->begin / 4];
             bitmap->end1 = &bitmap->hex[(bitmap->end + 3) / 4];
             bitmap->begin2 = bitmap->end1;
             bitmap->end2 = bitmap->end1;
         } else {
             bitmap->begin1 = &bitmap->hex[bitmap->begin / 4];
             bitmap->end1 = &bitmap->hex[MAX_HIST_BYTES * 2 + 8];
             bitmap->begin2 = &bitmap->hex[0];
             bitmap->end2 = &bitmap->hex[(bitmap->end + 3) / 4];
         }
         
         *bitmap->end1 = '\0';
         *bitmap->end2 = '\0';
    } else {
        bitmap->begin1 = &bitmap->hex[HIST_SIZE_BYTES * 2 + 2];
        bitmap->begin2 = &bitmap->hex[HIST_SIZE_BYTES * 2 + 3];
        bitmap->end2 = &bitmap->hex[(bitmap->end + 3) / 4];
    }

    bitmap->begin_mask = get_begin_mask(bitmap->begin);
    bitmap->end_mask = get_end_mask(bitmap->end);

    bitmap->old_reach = reach_bitmap_get_mask(bitmap, 0xFFFF);
    bitmap->old_reach = ((bitmap->old_reach & 0x5555) << 1) |
        ((bitmap->old_reach & 0xAAAA) >> 1);
    bitmap->old_reach = ((bitmap->old_reach & 0x3333) << 2) |
        ((bitmap->old_reach & 0xCCCC) >> 2);
    bitmap->old_reach = ((bitmap->old_reach & 0x0F0F) << 4) |
        ((bitmap->old_reach & 0xF0F0) >> 4);
    bitmap->old_reach = ((bitmap->old_reach & 0x00FF) << 8) |
        ((bitmap->old_reach & 0xFF00) >> 8);
}

static char get_begin_mask(int begin)
{
    const char begin_map[] = { 'F', '7', '3', '1' };
    return begin_map[begin % 4];
}

static char get_end_mask(int end)
{
    const char end_map[] = { 'F', '8', 'C', 'E' };
    return end_map[end % 4];
}

static int distance(int begin, int end, int length)
{
    return (end - begin + length) % length;
}

static int at_abs(const struct reach_bitmap *bitmap, int offset)
{
    return !!(bitmap->bitmap[offset / 8] & (1U << (7 - offset % 8)));
}

static int at_rel(const struct reach_bitmap *bitmap, int offset)
{
    int abs_offset;
    if (offset > bitmap->count) {
        return 0;
    }
    abs_offset = bitmap->end - offset;
    return at_abs(bitmap, (abs_offset + HIST_SIZE_BITS) % HIST_SIZE_BITS);
}

static int countset_byte(unsigned char byte)
{
    byte = (byte & 0x55) + ((byte & 0xAA) >> 1);
    byte = (byte & 0x33) + ((byte & 0xCC) >> 2);
    byte = (byte & 0x0F) + ((byte & 0xF0) >> 4);
    return byte;
}

#ifdef UNIT_TEST

static void print_bitmap(const struct reach_bitmap *bitmap)
{
    printf("beg:%d end:%d cnt:%d set:%d %c %s%s %c ",
            bitmap->begin, bitmap->end, bitmap->count, bitmap->count_set,
            bitmap->begin_mask, bitmap->begin1,
            bitmap->begin2, bitmap->end_mask);
}

int main(void)
{
    struct reach_bitmap bitmap;
    reach_bitmap_init(&bitmap);


reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x48, 8);
printf("%s ", "oper:push value:0x48 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 5);
printf("%s ", "oper:push value:0x0 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2ac, 11);
printf("%s ", "oper:push value:0x2ac bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc7a7, 16);
printf("%s ", "oper:push value:0xc7a7 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x229, 10);
printf("%s ", "oper:push value:0x229 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2ebb, 14);
printf("%s ", "oper:push value:0x2ebb bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 4);
printf("%s ", "oper:push value:0x0 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb7, 8);
printf("%s ", "oper:push value:0xb7 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x13e8, 13);
printf("%s ", "oper:push value:0x13e8 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x199, 10);
printf("%s ", "oper:push value:0x199 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x136, 9);
printf("%s ", "oper:push value:0x136 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x117, 9);
printf("%s ", "oper:push value:0x117 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7, 3);
printf("%s ", "oper:push value:0x7 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4ea4, 16);
printf("%s ", "oper:push value:0x4ea4 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xbf23, 16);
printf("%s ", "oper:push value:0xbf23 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 3);
printf("%s ", "oper:push value:0x3 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1358, 14);
printf("%s ", "oper:push value:0x1358 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x8e5, 12);
printf("%s ", "oper:push value:0x8e5 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x163, 11);
printf("%s ", "oper:push value:0x163 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 5);
printf("%s ", "oper:push value:0x6 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x72, 10);
printf("%s ", "oper:push value:0x72 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 2);
printf("%s ", "oper:push value:0x3 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 4);
printf("%s ", "oper:push value:0x6 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x17, 7);
printf("%s ", "oper:push value:0x17 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x25, 7);
printf("%s ", "oper:push value:0x25 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xa, 5);
printf("%s ", "oper:push value:0xa bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x33c, 12);
printf("%s ", "oper:push value:0x33c bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1e, 6);
printf("%s ", "oper:push value:0x1e bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1499, 13);
printf("%s ", "oper:push value:0x1499 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x28f4, 14);
printf("%s ", "oper:push value:0x28f4 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xf, 4);
printf("%s ", "oper:push value:0xf bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x391, 10);
printf("%s ", "oper:push value:0x391 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xf57, 12);
printf("%s ", "oper:push value:0xf57 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 2);
printf("%s ", "oper:push value:0x3 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5, 3);
printf("%s ", "oper:push value:0x5 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5, 4);
printf("%s ", "oper:push value:0x5 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5b, 10);
printf("%s ", "oper:push value:0x5b bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xd84, 12);
printf("%s ", "oper:push value:0xd84 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x20, 8);
printf("%s ", "oper:push value:0x20 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x158, 9);
printf("%s ", "oper:push value:0x158 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x54, 7);
printf("%s ", "oper:push value:0x54 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x9a2, 13);
printf("%s ", "oper:push value:0x9a2 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc89, 16);
printf("%s ", "oper:push value:0xc89 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x8, 4);
printf("%s ", "oper:push value:0x8 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x11, 7);
printf("%s ", "oper:push value:0x11 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xa, 6);
printf("%s ", "oper:push value:0xa bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1799, 14);
printf("%s ", "oper:push value:0x1799 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x8, 4);
printf("%s ", "oper:push value:0x8 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x38fd, 14);
printf("%s ", "oper:push value:0x38fd bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x18, 5);
printf("%s ", "oper:push value:0x18 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x527, 12);
printf("%s ", "oper:push value:0x527 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7, 3);
printf("%s ", "oper:push value:0x7 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x658, 11);
printf("%s ", "oper:push value:0x658 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5f8, 11);
printf("%s ", "oper:push value:0x5f8 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x597, 14);
printf("%s ", "oper:push value:0x597 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7, 3);
printf("%s ", "oper:push value:0x7 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xa58, 13);
printf("%s ", "oper:push value:0xa58 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb5f, 13);
printf("%s ", "oper:push value:0xb5f bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2, 2);
printf("%s ", "oper:push value:0x2 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x201, 10);
printf("%s ", "oper:push value:0x201 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x34a, 10);
printf("%s ", "oper:push value:0x34a bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1567, 14);
printf("%s ", "oper:push value:0x1567 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2, 2);
printf("%s ", "oper:push value:0x2 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1d1f, 14);
printf("%s ", "oper:push value:0x1d1f bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3d3, 10);
printf("%s ", "oper:push value:0x3d3 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x19, 7);
printf("%s ", "oper:push value:0x19 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2c, 9);
printf("%s ", "oper:push value:0x2c bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xf, 4);
printf("%s ", "oper:push value:0xf bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x96e, 12);
printf("%s ", "oper:push value:0x96e bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xe, 5);
printf("%s ", "oper:push value:0xe bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x44, 8);
printf("%s ", "oper:push value:0x44 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xd, 4);
printf("%s ", "oper:push value:0xd bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb, 5);
printf("%s ", "oper:push value:0xb bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb6e, 12);
printf("%s ", "oper:push value:0xb6e bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xe6f, 12);
printf("%s ", "oper:push value:0xe6f bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1e18, 14);
printf("%s ", "oper:push value:0x1e18 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xfa1, 12);
printf("%s ", "oper:push value:0xfa1 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 4);
printf("%s ", "oper:push value:0x3 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x15df, 15);
printf("%s ", "oper:push value:0x15df bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xa3, 8);
printf("%s ", "oper:push value:0xa3 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xed7, 13);
printf("%s ", "oper:push value:0xed7 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6677, 15);
printf("%s ", "oper:push value:0x6677 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xf63, 15);
printf("%s ", "oper:push value:0xf63 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x154, 9);
printf("%s ", "oper:push value:0x154 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x322, 10);
printf("%s ", "oper:push value:0x322 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7, 5);
printf("%s ", "oper:push value:0x7 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1bf, 10);
printf("%s ", "oper:push value:0x1bf bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1cfa, 13);
printf("%s ", "oper:push value:0x1cfa bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x17c, 9);
printf("%s ", "oper:push value:0x17c bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2, 2);
printf("%s ", "oper:push value:0x2 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5, 5);
printf("%s ", "oper:push value:0x5 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 2);
printf("%s ", "oper:push value:0x0 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1577, 15);
printf("%s ", "oper:push value:0x1577 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 2);
printf("%s ", "oper:push value:0x3 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x89, 8);
printf("%s ", "oper:push value:0x89 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x329d, 15);
printf("%s ", "oper:push value:0x329d bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1a06, 14);
printf("%s ", "oper:push value:0x1a06 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x77, 7);
printf("%s ", "oper:push value:0x77 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x346, 13);
printf("%s ", "oper:push value:0x346 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 2);
printf("%s ", "oper:push value:0x0 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x32, 6);
printf("%s ", "oper:push value:0x32 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x97, 9);
printf("%s ", "oper:push value:0x97 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xef, 8);
printf("%s ", "oper:push value:0xef bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x904, 12);
printf("%s ", "oper:push value:0x904 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x313, 11);
printf("%s ", "oper:push value:0x313 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6a42, 16);
printf("%s ", "oper:push value:0x6a42 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x353, 11);
printf("%s ", "oper:push value:0x353 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xf5, 10);
printf("%s ", "oper:push value:0xf5 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc, 4);
printf("%s ", "oper:push value:0xc bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5, 5);
printf("%s ", "oper:push value:0x5 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb1a5, 16);
printf("%s ", "oper:push value:0xb1a5 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb6c, 12);
printf("%s ", "oper:push value:0xb6c bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x110b, 14);
printf("%s ", "oper:push value:0x110b bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x31ef, 16);
printf("%s ", "oper:push value:0x31ef bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4de9, 15);
printf("%s ", "oper:push value:0x4de9 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x9e, 8);
printf("%s ", "oper:push value:0x9e bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x54, 10);
printf("%s ", "oper:push value:0x54 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2ed, 10);
printf("%s ", "oper:push value:0x2ed bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x86e8, 16);
printf("%s ", "oper:push value:0x86e8 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xd, 4);
printf("%s ", "oper:push value:0xd bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 4);
printf("%s ", "oper:push value:0x6 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x193, 9);
printf("%s ", "oper:push value:0x193 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x13d6, 13);
printf("%s ", "oper:push value:0x13d6 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x47, 7);
printf("%s ", "oper:push value:0x47 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4e33, 16);
printf("%s ", "oper:push value:0x4e33 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1a, 6);
printf("%s ", "oper:push value:0x1a bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x9, 5);
printf("%s ", "oper:push value:0x9 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x71, 7);
printf("%s ", "oper:push value:0x71 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4b, 7);
printf("%s ", "oper:push value:0x4b bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x29, 14);
printf("%s ", "oper:push value:0x29 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x73da, 16);
printf("%s ", "oper:push value:0x73da bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x389, 10);
printf("%s ", "oper:push value:0x389 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xffe, 12);
printf("%s ", "oper:push value:0xffe bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4bb6, 16);
printf("%s ", "oper:push value:0x4bb6 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x309, 10);
printf("%s ", "oper:push value:0x309 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x54, 8);
printf("%s ", "oper:push value:0x54 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x92, 8);
printf("%s ", "oper:push value:0x92 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2d7, 13);
printf("%s ", "oper:push value:0x2d7 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1d, 6);
printf("%s ", "oper:push value:0x1d bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xed, 8);
printf("%s ", "oper:push value:0xed bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1e, 5);
printf("%s ", "oper:push value:0x1e bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xd91, 12);
printf("%s ", "oper:push value:0xd91 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x45, 8);
printf("%s ", "oper:push value:0x45 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2c5, 10);
printf("%s ", "oper:push value:0x2c5 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1de, 10);
printf("%s ", "oper:push value:0x1de bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x15e7, 13);
printf("%s ", "oper:push value:0x15e7 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x345, 10);
printf("%s ", "oper:push value:0x345 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 3);
printf("%s ", "oper:push value:0x0 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x563, 12);
printf("%s ", "oper:push value:0x563 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5c1e, 15);
printf("%s ", "oper:push value:0x5c1e bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2cc, 11);
printf("%s ", "oper:push value:0x2cc bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xa, 5);
printf("%s ", "oper:push value:0xa bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x29, 6);
printf("%s ", "oper:push value:0x29 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 2);
printf("%s ", "oper:push value:0x0 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1a82, 13);
printf("%s ", "oper:push value:0x1a82 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4, 3);
printf("%s ", "oper:push value:0x4 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x103, 9);
printf("%s ", "oper:push value:0x103 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 3);
printf("%s ", "oper:push value:0x6 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 3);
printf("%s ", "oper:push value:0x1 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x95a, 13);
printf("%s ", "oper:push value:0x95a bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x8748, 16);
printf("%s ", "oper:push value:0x8748 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb, 5);
printf("%s ", "oper:push value:0xb bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5d, 7);
printf("%s ", "oper:push value:0x5d bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1e9, 11);
printf("%s ", "oper:push value:0x1e9 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x89, 8);
printf("%s ", "oper:push value:0x89 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5c1f, 15);
printf("%s ", "oper:push value:0x5c1f bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4685, 15);
printf("%s ", "oper:push value:0x4685 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x9d3, 14);
printf("%s ", "oper:push value:0x9d3 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 5);
printf("%s ", "oper:push value:0x3 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 3);
printf("%s ", "oper:push value:0x3 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xa, 4);
printf("%s ", "oper:push value:0xa bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2647, 15);
printf("%s ", "oper:push value:0x2647 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 3);
printf("%s ", "oper:push value:0x6 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x933, 13);
printf("%s ", "oper:push value:0x933 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5587, 15);
printf("%s ", "oper:push value:0x5587 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2, 2);
printf("%s ", "oper:push value:0x2 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x18b, 9);
printf("%s ", "oper:push value:0x18b bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xf, 4);
printf("%s ", "oper:push value:0xf bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xdd, 8);
printf("%s ", "oper:push value:0xdd bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1f, 6);
printf("%s ", "oper:push value:0x1f bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x38a, 11);
printf("%s ", "oper:push value:0x38a bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x9b, 8);
printf("%s ", "oper:push value:0x9b bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 3);
printf("%s ", "oper:push value:0x1 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x17, 5);
printf("%s ", "oper:push value:0x17 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc4, 9);
printf("%s ", "oper:push value:0xc4 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x975, 12);
printf("%s ", "oper:push value:0x975 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xfb68, 16);
printf("%s ", "oper:push value:0xfb68 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xa6b0, 16);
printf("%s ", "oper:push value:0xa6b0 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3692, 16);
printf("%s ", "oper:push value:0x3692 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1f, 6);
printf("%s ", "oper:push value:0x1f bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x702, 16);
printf("%s ", "oper:push value:0x702 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x134, 9);
printf("%s ", "oper:push value:0x134 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x66f, 11);
printf("%s ", "oper:push value:0x66f bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xff, 8);
printf("%s ", "oper:push value:0xff bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 4);
printf("%s ", "oper:push value:0x0 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xa1, 11);
printf("%s ", "oper:push value:0xa1 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x9a, 10);
printf("%s ", "oper:push value:0x9a bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xd861, 16);
printf("%s ", "oper:push value:0xd861 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x142, 9);
printf("%s ", "oper:push value:0x142 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x55b9, 16);
printf("%s ", "oper:push value:0x55b9 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x10, 6);
printf("%s ", "oper:push value:0x10 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 2);
printf("%s ", "oper:push value:0x3 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xef, 9);
printf("%s ", "oper:push value:0xef bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1c3, 10);
printf("%s ", "oper:push value:0x1c3 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 3);
printf("%s ", "oper:push value:0x0 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x9a, 8);
printf("%s ", "oper:push value:0x9a bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x53, 7);
printf("%s ", "oper:push value:0x53 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc, 4);
printf("%s ", "oper:push value:0xc bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2c, 7);
printf("%s ", "oper:push value:0x2c bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xf1, 8);
printf("%s ", "oper:push value:0xf1 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1ef, 11);
printf("%s ", "oper:push value:0x1ef bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x97, 8);
printf("%s ", "oper:push value:0x97 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4f6, 11);
printf("%s ", "oper:push value:0x4f6 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xfd2b, 16);
printf("%s ", "oper:push value:0xfd2b bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xef, 8);
printf("%s ", "oper:push value:0xef bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2b30, 14);
printf("%s ", "oper:push value:0x2b30 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xea8, 14);
printf("%s ", "oper:push value:0xea8 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x79, 7);
printf("%s ", "oper:push value:0x79 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x30, 6);
printf("%s ", "oper:push value:0x30 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xa, 7);
printf("%s ", "oper:push value:0xa bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1b, 5);
printf("%s ", "oper:push value:0x1b bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x73, 7);
printf("%s ", "oper:push value:0x73 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x18f0, 15);
printf("%s ", "oper:push value:0x18f0 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc, 4);
printf("%s ", "oper:push value:0xc bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x14, 5);
printf("%s ", "oper:push value:0x14 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x221, 11);
printf("%s ", "oper:push value:0x221 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x99, 8);
printf("%s ", "oper:push value:0x99 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x74, 11);
printf("%s ", "oper:push value:0x74 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5e, 9);
printf("%s ", "oper:push value:0x5e bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 2);
printf("%s ", "oper:push value:0x0 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x23b0, 15);
printf("%s ", "oper:push value:0x23b0 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3d33, 14);
printf("%s ", "oper:push value:0x3d33 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 3);
printf("%s ", "oper:push value:0x3 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb, 4);
printf("%s ", "oper:push value:0xb bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x61c, 13);
printf("%s ", "oper:push value:0x61c bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xf, 4);
printf("%s ", "oper:push value:0xf bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 3);
printf("%s ", "oper:push value:0x6 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x32ed, 14);
printf("%s ", "oper:push value:0x32ed bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4, 6);
printf("%s ", "oper:push value:0x4 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xf, 4);
printf("%s ", "oper:push value:0xf bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xbbc9, 16);
printf("%s ", "oper:push value:0xbbc9 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 2);
printf("%s ", "oper:push value:0x0 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x79, 7);
printf("%s ", "oper:push value:0x79 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb71, 13);
printf("%s ", "oper:push value:0xb71 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x462, 16);
printf("%s ", "oper:push value:0x462 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x469, 12);
printf("%s ", "oper:push value:0x469 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 2);
printf("%s ", "oper:push value:0x0 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 5);
printf("%s ", "oper:push value:0x6 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5d, 8);
printf("%s ", "oper:push value:0x5d bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1cd, 10);
printf("%s ", "oper:push value:0x1cd bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xadc, 14);
printf("%s ", "oper:push value:0xadc bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x55, 7);
printf("%s ", "oper:push value:0x55 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x16, 5);
printf("%s ", "oper:push value:0x16 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1a22, 16);
printf("%s ", "oper:push value:0x1a22 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6e50, 15);
printf("%s ", "oper:push value:0x6e50 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4, 4);
printf("%s ", "oper:push value:0x4 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5e1, 13);
printf("%s ", "oper:push value:0x5e1 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb, 5);
printf("%s ", "oper:push value:0xb bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x391, 10);
printf("%s ", "oper:push value:0x391 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x357, 12);
printf("%s ", "oper:push value:0x357 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2e, 11);
printf("%s ", "oper:push value:0x2e bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 4);
printf("%s ", "oper:push value:0x6 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x769b, 16);
printf("%s ", "oper:push value:0x769b bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xe, 4);
printf("%s ", "oper:push value:0xe bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3b, 8);
printf("%s ", "oper:push value:0x3b bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x19f4, 14);
printf("%s ", "oper:push value:0x19f4 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x18, 5);
printf("%s ", "oper:push value:0x18 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x51ec, 16);
printf("%s ", "oper:push value:0x51ec bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x8e, 9);
printf("%s ", "oper:push value:0x8e bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 3);
printf("%s ", "oper:push value:0x6 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x20, 6);
printf("%s ", "oper:push value:0x20 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x810, 13);
printf("%s ", "oper:push value:0x810 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 3);
printf("%s ", "oper:push value:0x1 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 4);
printf("%s ", "oper:push value:0x0 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x9, 4);
printf("%s ", "oper:push value:0x9 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3c4, 11);
printf("%s ", "oper:push value:0x3c4 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7, 4);
printf("%s ", "oper:push value:0x7 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1a3, 9);
printf("%s ", "oper:push value:0x1a3 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc, 5);
printf("%s ", "oper:push value:0xc bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x37b, 12);
printf("%s ", "oper:push value:0x37b bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x9e, 8);
printf("%s ", "oper:push value:0x9e bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x28, 6);
printf("%s ", "oper:push value:0x28 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x11f5, 14);
printf("%s ", "oper:push value:0x11f5 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x113e, 13);
printf("%s ", "oper:push value:0x113e bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xcfd, 13);
printf("%s ", "oper:push value:0xcfd bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5, 3);
printf("%s ", "oper:push value:0x5 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7c, 10);
printf("%s ", "oper:push value:0x7c bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x25, 6);
printf("%s ", "oper:push value:0x25 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3325, 16);
printf("%s ", "oper:push value:0x3325 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x8, 4);
printf("%s ", "oper:push value:0x8 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7ca, 13);
printf("%s ", "oper:push value:0x7ca bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x89, 9);
printf("%s ", "oper:push value:0x89 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x389, 10);
printf("%s ", "oper:push value:0x389 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3975, 14);
printf("%s ", "oper:push value:0x3975 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x13, 8);
printf("%s ", "oper:push value:0x13 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2806, 14);
printf("%s ", "oper:push value:0x2806 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1a8, 9);
printf("%s ", "oper:push value:0x1a8 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5620, 15);
printf("%s ", "oper:push value:0x5620 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 3);
printf("%s ", "oper:push value:0x6 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x42, 7);
printf("%s ", "oper:push value:0x42 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc41, 12);
printf("%s ", "oper:push value:0xc41 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5, 4);
printf("%s ", "oper:push value:0x5 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xf01e, 16);
printf("%s ", "oper:push value:0xf01e bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 2);
printf("%s ", "oper:push value:0x3 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7, 7);
printf("%s ", "oper:push value:0x7 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x782b, 15);
printf("%s ", "oper:push value:0x782b bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5, 3);
printf("%s ", "oper:push value:0x5 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc, 6);
printf("%s ", "oper:push value:0xc bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x14, 6);
printf("%s ", "oper:push value:0x14 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xd, 5);
printf("%s ", "oper:push value:0xd bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3b, 9);
printf("%s ", "oper:push value:0x3b bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x397f, 15);
printf("%s ", "oper:push value:0x397f bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x633, 14);
printf("%s ", "oper:push value:0x633 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 3);
printf("%s ", "oper:push value:0x1 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 4);
printf("%s ", "oper:push value:0x0 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x50c, 11);
printf("%s ", "oper:push value:0x50c bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x368, 11);
printf("%s ", "oper:push value:0x368 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1654, 13);
printf("%s ", "oper:push value:0x1654 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5, 4);
printf("%s ", "oper:push value:0x5 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4193, 15);
printf("%s ", "oper:push value:0x4193 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x26, 8);
printf("%s ", "oper:push value:0x26 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 2);
printf("%s ", "oper:push value:0x3 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 2);
printf("%s ", "oper:push value:0x3 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x151e, 13);
printf("%s ", "oper:push value:0x151e bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2, 2);
printf("%s ", "oper:push value:0x2 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7, 8);
printf("%s ", "oper:push value:0x7 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x36d, 10);
printf("%s ", "oper:push value:0x36d bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1f5, 9);
printf("%s ", "oper:push value:0x1f5 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb5, 9);
printf("%s ", "oper:push value:0xb5 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xa, 4);
printf("%s ", "oper:push value:0xa bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x311b, 14);
printf("%s ", "oper:push value:0x311b bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 4);
printf("%s ", "oper:push value:0x6 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x36e2, 15);
printf("%s ", "oper:push value:0x36e2 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7, 5);
printf("%s ", "oper:push value:0x7 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1447, 13);
printf("%s ", "oper:push value:0x1447 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 2);
printf("%s ", "oper:push value:0x0 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 4);
printf("%s ", "oper:push value:0x6 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb, 4);
printf("%s ", "oper:push value:0xb bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xe, 5);
printf("%s ", "oper:push value:0xe bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2e, 6);
printf("%s ", "oper:push value:0x2e bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4a, 7);
printf("%s ", "oper:push value:0x4a bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 3);
printf("%s ", "oper:push value:0x1 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 3);
printf("%s ", "oper:push value:0x6 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2e, 6);
printf("%s ", "oper:push value:0x2e bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x20, 7);
printf("%s ", "oper:push value:0x20 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4, 4);
printf("%s ", "oper:push value:0x4 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1b75, 14);
printf("%s ", "oper:push value:0x1b75 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1f, 5);
printf("%s ", "oper:push value:0x1f bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xf5e0, 16);
printf("%s ", "oper:push value:0xf5e0 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xa2, 8);
printf("%s ", "oper:push value:0xa2 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1e0, 11);
printf("%s ", "oper:push value:0x1e0 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6706, 15);
printf("%s ", "oper:push value:0x6706 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x64, 8);
printf("%s ", "oper:push value:0x64 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x68, 7);
printf("%s ", "oper:push value:0x68 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 4);
printf("%s ", "oper:push value:0x6 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc6c, 12);
printf("%s ", "oper:push value:0xc6c bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1ac8, 14);
printf("%s ", "oper:push value:0x1ac8 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 2);
printf("%s ", "oper:push value:0x0 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1460, 14);
printf("%s ", "oper:push value:0x1460 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2f2, 11);
printf("%s ", "oper:push value:0x2f2 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xa294, 16);
printf("%s ", "oper:push value:0xa294 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4c8, 11);
printf("%s ", "oper:push value:0x4c8 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x35, 6);
printf("%s ", "oper:push value:0x35 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x35, 6);
printf("%s ", "oper:push value:0x35 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xd1, 12);
printf("%s ", "oper:push value:0xd1 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3f69, 14);
printf("%s ", "oper:push value:0x3f69 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x66, 8);
printf("%s ", "oper:push value:0x66 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x32, 9);
printf("%s ", "oper:push value:0x32 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xccf6, 16);
printf("%s ", "oper:push value:0xccf6 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc8, 8);
printf("%s ", "oper:push value:0xc8 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4, 4);
printf("%s ", "oper:push value:0x4 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x86b, 14);
printf("%s ", "oper:push value:0x86b bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1d1, 10);
printf("%s ", "oper:push value:0x1d1 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x155, 9);
printf("%s ", "oper:push value:0x155 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x13, 7);
printf("%s ", "oper:push value:0x13 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xad, 10);
printf("%s ", "oper:push value:0xad bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x16, 5);
printf("%s ", "oper:push value:0x16 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 2);
printf("%s ", "oper:push value:0x0 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xd, 4);
printf("%s ", "oper:push value:0xd bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1c, 6);
printf("%s ", "oper:push value:0x1c bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x13c, 9);
printf("%s ", "oper:push value:0x13c bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xd16, 12);
printf("%s ", "oper:push value:0xd16 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x32, 7);
printf("%s ", "oper:push value:0x32 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4, 6);
printf("%s ", "oper:push value:0x4 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb, 6);
printf("%s ", "oper:push value:0xb bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x162, 9);
printf("%s ", "oper:push value:0x162 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x18d4, 13);
printf("%s ", "oper:push value:0x18d4 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5a, 7);
printf("%s ", "oper:push value:0x5a bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb1af, 16);
printf("%s ", "oper:push value:0xb1af bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 9);
printf("%s ", "oper:push value:0x1 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x8f, 8);
printf("%s ", "oper:push value:0x8f bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x175, 11);
printf("%s ", "oper:push value:0x175 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb6, 9);
printf("%s ", "oper:push value:0xb6 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6c, 7);
printf("%s ", "oper:push value:0x6c bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5f2, 11);
printf("%s ", "oper:push value:0x5f2 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 4);
printf("%s ", "oper:push value:0x6 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xe070, 16);
printf("%s ", "oper:push value:0xe070 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 2);
printf("%s ", "oper:push value:0x0 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2a, 8);
printf("%s ", "oper:push value:0x2a bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x5f4a, 15);
printf("%s ", "oper:push value:0x5f4a bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc, 4);
printf("%s ", "oper:push value:0xc bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xdd, 8);
printf("%s ", "oper:push value:0xdd bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7a2f, 16);
printf("%s ", "oper:push value:0x7a2f bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x74, 12);
printf("%s ", "oper:push value:0x74 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x953b, 16);
printf("%s ", "oper:push value:0x953b bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7, 3);
printf("%s ", "oper:push value:0x7 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xd, 5);
printf("%s ", "oper:push value:0xd bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 2);
printf("%s ", "oper:push value:0x3 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 2);
printf("%s ", "oper:push value:0x3 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6, 10);
printf("%s ", "oper:push value:0x6 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4a, 10);
printf("%s ", "oper:push value:0x4a bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3fa, 10);
printf("%s ", "oper:push value:0x3fa bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7, 3);
printf("%s ", "oper:push value:0x7 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x48, 7);
printf("%s ", "oper:push value:0x48 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x24, 8);
printf("%s ", "oper:push value:0x24 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x9e, 8);
printf("%s ", "oper:push value:0x9e bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 2);
printf("%s ", "oper:push value:0x3 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1ef7, 14);
printf("%s ", "oper:push value:0x1ef7 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x16a3, 16);
printf("%s ", "oper:push value:0x16a3 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x74, 10);
printf("%s ", "oper:push value:0x74 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc0, 9);
printf("%s ", "oper:push value:0xc0 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x69f, 11);
printf("%s ", "oper:push value:0x69f bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x35, 6);
printf("%s ", "oper:push value:0x35 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2, 3);
printf("%s ", "oper:push value:0x2 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 2);
printf("%s ", "oper:push value:0x3 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x17, 5);
printf("%s ", "oper:push value:0x17 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3255, 14);
printf("%s ", "oper:push value:0x3255 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1651, 13);
printf("%s ", "oper:push value:0x1651 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7, 6);
printf("%s ", "oper:push value:0x7 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7, 4);
printf("%s ", "oper:push value:0x7 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4769, 15);
printf("%s ", "oper:push value:0x4769 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xf, 7);
printf("%s ", "oper:push value:0xf bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x50, 7);
printf("%s ", "oper:push value:0x50 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6c8f, 15);
printf("%s ", "oper:push value:0x6c8f bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x382, 10);
printf("%s ", "oper:push value:0x382 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1d, 5);
printf("%s ", "oper:push value:0x1d bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x29b, 10);
printf("%s ", "oper:push value:0x29b bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x813, 13);
printf("%s ", "oper:push value:0x813 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 4);
printf("%s ", "oper:push value:0x1 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc, 4);
printf("%s ", "oper:push value:0xc bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6e, 11);
printf("%s ", "oper:push value:0x6e bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x36, 6);
printf("%s ", "oper:push value:0x36 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 2);
printf("%s ", "oper:push value:0x3 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x633, 14);
printf("%s ", "oper:push value:0x633 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x69e, 11);
printf("%s ", "oper:push value:0x69e bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x17, 5);
printf("%s ", "oper:push value:0x17 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb, 4);
printf("%s ", "oper:push value:0xb bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x26, 7);
printf("%s ", "oper:push value:0x26 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x18, 5);
printf("%s ", "oper:push value:0x18 bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x25, 6);
printf("%s ", "oper:push value:0x25 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3b, 6);
printf("%s ", "oper:push value:0x3b bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x550, 12);
printf("%s ", "oper:push value:0x550 bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 2);
printf("%s ", "oper:push value:0x0 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x3, 2);
printf("%s ", "oper:push value:0x3 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc3, 8);
printf("%s ", "oper:push value:0xc3 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x7, 3);
printf("%s ", "oper:push value:0x7 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xe7e, 12);
printf("%s ", "oper:push value:0xe7e bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x14, 7);
printf("%s ", "oper:push value:0x14 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xf, 7);
printf("%s ", "oper:push value:0xf bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x9, 4);
printf("%s ", "oper:push value:0x9 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x15d3, 16);
printf("%s ", "oper:push value:0x15d3 bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x697, 11);
printf("%s ", "oper:push value:0x697 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x323, 10);
printf("%s ", "oper:push value:0x323 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xdfd, 12);
printf("%s ", "oper:push value:0xdfd bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4, 4);
printf("%s ", "oper:push value:0x4 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x364, 10);
printf("%s ", "oper:push value:0x364 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x17, 6);
printf("%s ", "oper:push value:0x17 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x86, 10);
printf("%s ", "oper:push value:0x86 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x6f0b, 16);
printf("%s ", "oper:push value:0x6f0b bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x201, 10);
printf("%s ", "oper:push value:0x201 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1d47, 14);
printf("%s ", "oper:push value:0x1d47 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 1);
printf("%s ", "oper:push value:0x1 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1d04, 14);
printf("%s ", "oper:push value:0x1d04 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xe8, 8);
printf("%s ", "oper:push value:0xe8 bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x17, 6);
printf("%s ", "oper:push value:0x17 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x333, 10);
printf("%s ", "oper:push value:0x333 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x4, 3);
printf("%s ", "oper:push value:0x4 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 5);
printf("%s ", "oper:pop value:--- bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1c, 6);
printf("%s ", "oper:push value:0x1c bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x8, 6);
printf("%s ", "oper:push value:0x8 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2215, 15);
printf("%s ", "oper:push value:0x2215 bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x449c, 16);
printf("%s ", "oper:push value:0x449c bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xe35, 13);
printf("%s ", "oper:push value:0xe35 bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x52, 7);
printf("%s ", "oper:push value:0x52 bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2, 3);
printf("%s ", "oper:push value:0x2 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 7);
printf("%s ", "oper:pop value:--- bits:7");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 14);
printf("%s ", "oper:pop value:--- bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x15b4, 14);
printf("%s ", "oper:push value:0x15b4 bits:14");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 9);
printf("%s ", "oper:pop value:--- bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x8, 4);
printf("%s ", "oper:push value:0x8 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x0, 1);
printf("%s ", "oper:push value:0x0 bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 8);
printf("%s ", "oper:pop value:--- bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 3);
printf("%s ", "oper:pop value:--- bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 6);
printf("%s ", "oper:pop value:--- bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x25, 6);
printf("%s ", "oper:push value:0x25 bits:6");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 10);
printf("%s ", "oper:pop value:--- bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x960d, 16);
printf("%s ", "oper:push value:0x960d bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x8c, 11);
printf("%s ", "oper:push value:0x8c bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xc3, 9);
printf("%s ", "oper:push value:0xc3 bits:9");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x174, 10);
printf("%s ", "oper:push value:0x174 bits:10");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 3);
printf("%s ", "oper:push value:0x1 bits:3");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x9, 4);
printf("%s ", "oper:push value:0x9 bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xb, 5);
printf("%s ", "oper:push value:0xb bits:5");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 2);
printf("%s ", "oper:pop value:--- bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x2, 2);
printf("%s ", "oper:push value:0x2 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 1);
printf("%s ", "oper:pop value:--- bits:1");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1e0d, 13);
printf("%s ", "oper:push value:0x1e0d bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 4);
printf("%s ", "oper:pop value:--- bits:4");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 11);
printf("%s ", "oper:pop value:--- bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 16);
printf("%s ", "oper:pop value:--- bits:16");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 12);
printf("%s ", "oper:pop value:--- bits:12");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 15);
printf("%s ", "oper:pop value:--- bits:15");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1f4, 11);
printf("%s ", "oper:push value:0x1f4 bits:11");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_pop_new(&bitmap, 13);
printf("%s ", "oper:pop value:--- bits:13");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0xdc, 8);
printf("%s ", "oper:push value:0xdc bits:8");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
reach_bitmap_push_new(&bitmap, 0x1, 2);
printf("%s ", "oper:push value:0x1 bits:2");
print_bitmap(&bitmap);
for (int i = 0; i < 16; ++i) {
printf("0x%x ", reach_bitmap_get_mask(&bitmap, 0xFFFF >> i));
}
printf("%d\n", reach_bitmap_two_three(&bitmap));
}



#endif


