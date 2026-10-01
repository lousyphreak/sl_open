/*
 * Bink video decoder
 * Copyright (c) 2009 Konstantin Shishkov
 * Copyright (C) 2011 Peter Ross <pross@xvid.org>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "media/bink_compat.hpp"
#include "media/bink_data.hpp"
#include "media/bink_video.hpp"

#include <new>

#define BINK_FLAG_ALPHA 0x00100000
#define BINK_FLAG_GRAY  0x00020000

/**
 * IDs for different data types used in Bink video codec
 */
enum Sources {
    BINK_SRC_BLOCK_TYPES = 0, ///< 8x8 block types
    BINK_SRC_SUB_BLOCK_TYPES, ///< 16x16 block types (a subset of 8x8 block types)
    BINK_SRC_COLORS,          ///< pixel values used for different block types
    BINK_SRC_PATTERN,         ///< 8-bit values for 2-colour pattern fill
    BINK_SRC_X_OFF,           ///< X components of motion value
    BINK_SRC_Y_OFF,           ///< Y components of motion value
    BINK_SRC_INTRA_DC,        ///< DC values for intrablocks with DCT
    BINK_SRC_INTER_DC,        ///< DC values for interblocks with DCT
    BINK_SRC_RUN,             ///< run lengths for special fill block

    BINK_NB_SRC
};

/**
 * data needed to decode 4-bit Huffman-coded value
 */
typedef struct Tree {
    int     vlc_num;  ///< tree number (in bink_trees[])
    uint8_t syms[16]; ///< leaf value to symbol mapping
} Tree;

static int get_huff(GetBitContext* bits, int tree)
{
	unsigned code = 0;
	for (int length = 1; length <= 7; ++length)
	{
		code |= get_bits1(bits) << (length - 1);
		for (int symbol = 0; symbol < 16; ++symbol)
		{
			if (bink_tree_lens[tree][symbol] == length
				&& bink_tree_bits[tree][symbol] == code)
			{
				return symbol;
			}
		}
	}
	bits->failed = true;
	return 0;
}

#define GET_HUFF(gb, tree) ((tree).syms[get_huff((gb), (tree).vlc_num)])

/**
 * data structure used for decoding single Bink data type
 */
typedef struct Bundle {
    int     len;       ///< length of number of entries to decode (in bits)
    Tree    tree;      ///< Huffman tree-related data
    uint8_t *data;     ///< buffer for decoded symbols
    uint8_t *data_end; ///< buffer end
    uint8_t *cur_dec;  ///< pointer to the not yet decoded part of the buffer
    uint8_t *cur_ptr;  ///< pointer to the data that is not read from buffer yet
} Bundle;

/*
 * Decoder context
 */
typedef struct BinkContext {
    AVCodecContext *avctx;
    BlockDSPContext bdsp;
    op_pixels_func put_pixels_tab;
    BinkDSPContext binkdsp;
    AVFrame        *last;
    AVFrame         empty;
    int            version;              ///< internal Bink file version
    int            has_alpha;
    int            swap_planes;
    Bundle         bundle[BINK_NB_SRC]; ///< bundles for decoding all data types
    Tree           col_high[16];         ///< trees for decoding high nibble in "colours" data type
    int            col_lastval;          ///< value of last decoded high nibble in "colours" data type
} BinkContext;

/**
 * Bink video block types
 */
enum BlockTypes {
    SKIP_BLOCK = 0, ///< skipped block
    SCALED_BLOCK,   ///< block has size 16x16
    MOTION_BLOCK,   ///< block is copied from previous frame with some offset
    RUN_BLOCK,      ///< block is composed from runs of colours with custom scan order
    RESIDUE_BLOCK,  ///< motion block with some difference added
    INTRA_BLOCK,    ///< intra DCT block
    FILL_BLOCK,     ///< block is filled with single colour
    INTER_BLOCK,    ///< motion block with DCT applied to the difference
    PATTERN_BLOCK,  ///< block is filled with two colours following custom pattern
    RAW_BLOCK,      ///< uncoded 8x8 block
};

/**
 * Initialize length in all bundles.
 *
 * @param c     decoder context
 * @param width plane width
 * @param bw    plane width in 8x8 blocks
 */
static void init_lengths(BinkContext *c, int width, int bw)
{
    width = FFALIGN(width, 8);

    c->bundle[BINK_SRC_BLOCK_TYPES].len = av_log2((width >> 3) + 511) + 1;

    c->bundle[BINK_SRC_SUB_BLOCK_TYPES].len = av_log2((width >> 4) + 511) + 1;

    c->bundle[BINK_SRC_COLORS].len = av_log2(bw*64 + 511) + 1;

    c->bundle[BINK_SRC_INTRA_DC].len =
    c->bundle[BINK_SRC_INTER_DC].len =
    c->bundle[BINK_SRC_X_OFF].len =
    c->bundle[BINK_SRC_Y_OFF].len = av_log2((width >> 3) + 511) + 1;

    c->bundle[BINK_SRC_PATTERN].len = av_log2((bw << 3) + 511) + 1;

    c->bundle[BINK_SRC_RUN].len = av_log2(bw*48 + 511) + 1;
}

/**
 * Allocate memory for bundles.
 *
 * @param c decoder context
 */
static av_cold int init_bundles(BinkContext *c)
{
    int bw, bh, blocks;
    uint8_t *tmp;
    int i;

    bw = (c->avctx->width  + 7) >> 3;
    bh = (c->avctx->height + 7) >> 3;
    blocks = bw * bh;

    tmp = static_cast<uint8_t*>(av_calloc(blocks, 64 * BINK_NB_SRC));
    if (!tmp)
        return AVERROR(ENOMEM);
    for (i = 0; i < BINK_NB_SRC; i++) {
        c->bundle[i].data     = tmp;
        tmp                  += blocks * 64;
        c->bundle[i].data_end = tmp;
    }

    return 0;
}

/**
 * Free memory used by bundles.
 *
 * @param c decoder context
 */
static av_cold void free_bundles(BinkContext *c)
{
    av_freep(&c->bundle[0].data);
}

/**
 * Merge two consequent lists of equal size depending on bits read.
 *
 * @param gb   context for reading bits
 * @param dst  buffer where merged list will be written to
 * @param src  pointer to the head of the first list (the second lists starts at src+size)
 * @param size input lists size
 */
static void merge(GetBitContext *gb, uint8_t *dst, uint8_t *src, int size)
{
    uint8_t *src2 = src + size;
    int size2 = size;

    do {
        if (!get_bits1(gb)) {
            *dst++ = *src++;
            size--;
        } else {
            *dst++ = *src2++;
            size2--;
        }
    } while (size && size2);

    while (size--)
        *dst++ = *src++;
    while (size2--)
        *dst++ = *src2++;
}

/**
 * Read information about Huffman tree used to decode data.
 *
 * @param gb   context for reading bits
 * @param tree pointer for storing tree data
 */
static int read_tree(GetBitContext *gb, Tree *tree)
{
    uint8_t tmp1[16] = { 0 }, tmp2[16], *in = tmp1, *out = tmp2;
    int i, t, len;

    if (get_bits_left(gb) < 4)
        return AVERROR_INVALIDDATA;

    tree->vlc_num = get_bits(gb, 4);
    if (!tree->vlc_num) {
        for (i = 0; i < 16; i++)
            tree->syms[i] = i;
        return 0;
    }
    if (get_bits1(gb)) {
        len = get_bits(gb, 3);
        for (i = 0; i <= len; i++) {
            tree->syms[i] = get_bits(gb, 4);
            tmp1[tree->syms[i]] = 1;
        }
        for (i = 0; i < 16 && len < 16 - 1; i++)
            if (!tmp1[i])
                tree->syms[++len] = i;
    } else {
        len = get_bits(gb, 2);
        for (i = 0; i < 16; i++)
            in[i] = i;
        for (i = 0; i <= len; i++) {
            int size = 1 << i;
            for (t = 0; t < 16; t += size << 1)
                merge(gb, out + t, in + t, size);
            FFSWAP(uint8_t*, in, out);
        }
        memcpy(tree->syms, in, 16);
    }
    return 0;
}

/**
 * Prepare bundle for decoding data.
 *
 * @param gb          context for reading bits
 * @param c           decoder context
 * @param bundle_num  number of the bundle to initialize
 */
static int read_bundle(GetBitContext *gb, BinkContext *c, int bundle_num)
{
    int i;

    if (bundle_num == BINK_SRC_COLORS) {
        for (i = 0; i < 16; i++) {
            int ret = read_tree(gb, &c->col_high[i]);
            if (ret < 0)
                return ret;
        }
        c->col_lastval = 0;
    }
    if (bundle_num != BINK_SRC_INTRA_DC && bundle_num != BINK_SRC_INTER_DC) {
        int ret = read_tree(gb, &c->bundle[bundle_num].tree);
        if (ret < 0)
            return ret;
    }
    c->bundle[bundle_num].cur_dec =
    c->bundle[bundle_num].cur_ptr = c->bundle[bundle_num].data;

    return 0;
}

/**
 * common check before starting decoding bundle data
 *
 * @param gb context for reading bits
 * @param b  bundle
 * @param t  variable where number of elements to decode will be stored
 */
#define CHECK_READ_VAL(gb, b, t) \
    if (!b->cur_dec || (b->cur_dec > b->cur_ptr)) \
        return 0; \
    t = get_bits(gb, b->len); \
    if (!t) { \
        b->cur_dec = NULL; \
        return 0; \
    } \

static int read_runs(AVCodecContext *avctx, GetBitContext *gb, Bundle *b)
{
    int t, v;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end) {
        av_log(avctx, AV_LOG_ERROR, "Run value went out of bounds\n");
        return AVERROR_INVALIDDATA;
    }
    if (get_bits_left(gb) < 1)
        return AVERROR_INVALIDDATA;
    if (get_bits1(gb)) {
        v = get_bits(gb, 4);
        memset(b->cur_dec, v, t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end)
            *b->cur_dec++ = GET_HUFF(gb, b->tree);
    }
    return 0;
}

static int read_motion_values(AVCodecContext *avctx, GetBitContext *gb, Bundle *b)
{
    int t, sign, v;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end) {
        av_log(avctx, AV_LOG_ERROR, "Too many motion values\n");
        return AVERROR_INVALIDDATA;
    }
    if (get_bits_left(gb) < 1)
        return AVERROR_INVALIDDATA;
    if (get_bits1(gb)) {
        v = get_bits(gb, 4);
        if (v) {
            sign = -get_bits1(gb);
            v = (v ^ sign) - sign;
        }
        memset(b->cur_dec, v, t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end) {
            v = GET_HUFF(gb, b->tree);
            if (v) {
                sign = -get_bits1(gb);
                v = (v ^ sign) - sign;
            }
            *b->cur_dec++ = v;
        }
    }
    return 0;
}

static const uint8_t bink_rlelens[4] = { 4, 8, 12, 32 };

static int read_block_types(AVCodecContext *avctx, GetBitContext *gb, Bundle *b)
{
    BinkContext * const c = static_cast<BinkContext*>(avctx->priv_data);
    int t, v;
    int last = 0;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    if (c->version == 'k') {
        t ^= 0xBBu;
        if (t == 0) {
            b->cur_dec = NULL;
            return 0;
        }
    }
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end) {
        av_log(avctx, AV_LOG_ERROR, "Too many block type values\n");
        return AVERROR_INVALIDDATA;
    }
    if (get_bits_left(gb) < 1)
        return AVERROR_INVALIDDATA;
    if (get_bits1(gb)) {
        v = get_bits(gb, 4);
        memset(b->cur_dec, v, t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end) {
            v = GET_HUFF(gb, b->tree);
            if (v < 12) {
                last = v;
                *b->cur_dec++ = v;
            } else {
                int run = bink_rlelens[v - 12];

                if (dec_end - b->cur_dec < run)
                    return AVERROR_INVALIDDATA;
                memset(b->cur_dec, last, run);
                b->cur_dec += run;
            }
        }
    }
    return 0;
}

static int read_patterns(AVCodecContext *avctx, GetBitContext *gb, Bundle *b)
{
    int t, v;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end) {
        av_log(avctx, AV_LOG_ERROR, "Too many pattern values\n");
        return AVERROR_INVALIDDATA;
    }
    while (b->cur_dec < dec_end) {
        if (get_bits_left(gb) < 2)
            return AVERROR_INVALIDDATA;
        v  = GET_HUFF(gb, b->tree);
        v |= GET_HUFF(gb, b->tree) << 4;
        *b->cur_dec++ = v;
    }

    return 0;
}

static int read_colors(GetBitContext *gb, Bundle *b, BinkContext *c)
{
    int t, sign, v;
    const uint8_t *dec_end;

    CHECK_READ_VAL(gb, b, t);
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end) {
        av_log(c->avctx, AV_LOG_ERROR, "Too many color values\n");
        return AVERROR_INVALIDDATA;
    }
    if (get_bits_left(gb) < 1)
        return AVERROR_INVALIDDATA;
    if (get_bits1(gb)) {
        c->col_lastval = GET_HUFF(gb, c->col_high[c->col_lastval]);
        v = GET_HUFF(gb, b->tree);
        v = (c->col_lastval << 4) | v;
        if (c->version < 'i') {
            sign = ((int8_t) v) >> 7;
            v = ((v & 0x7F) ^ sign) - sign;
            v += 0x80;
        }
        memset(b->cur_dec, v, t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end) {
            if (get_bits_left(gb) < 2)
                return AVERROR_INVALIDDATA;
            c->col_lastval = GET_HUFF(gb, c->col_high[c->col_lastval]);
            v = GET_HUFF(gb, b->tree);
            v = (c->col_lastval << 4) | v;
            if (c->version < 'i') {
                sign = ((int8_t) v) >> 7;
                v = ((v & 0x7F) ^ sign) - sign;
                v += 0x80;
            }
            *b->cur_dec++ = v;
        }
    }
    return 0;
}

/** number of bits used to store first DC value in bundle */
#define DC_START_BITS 11

static int read_dcs(AVCodecContext *avctx, GetBitContext *gb, Bundle *b,
                    int start_bits, int has_sign)
{
    int i, j, len, len2, bsize, sign, v, v2;
    int16_t *dst     = (int16_t*)b->cur_dec;
    int16_t *dst_end = (int16_t*)b->data_end;

    CHECK_READ_VAL(gb, b, len);
    if (get_bits_left(gb) < start_bits - has_sign)
        return AVERROR_INVALIDDATA;
    v = get_bits(gb, start_bits - has_sign);
    if (v && has_sign) {
        sign = -get_bits1(gb);
        v = (v ^ sign) - sign;
    }
    if (dst_end - dst < 1)
        return AVERROR_INVALIDDATA;
    *dst++ = v;
    len--;
    for (i = 0; i < len; i += 8) {
        len2 = FFMIN(len - i, 8);
        if (dst_end - dst < len2)
            return AVERROR_INVALIDDATA;
        bsize = get_bits(gb, 4);
        if (bsize) {
            for (j = 0; j < len2; j++) {
                v2 = get_bits(gb, bsize);
                if (v2) {
                    sign = -get_bits1(gb);
                    v2 = (v2 ^ sign) - sign;
                }
                v += v2;
                *dst++ = v;
                if (v < -32768 || v > 32767) {
                    av_log(avctx, AV_LOG_ERROR, "DC value went out of bounds: %d\n", v);
                    return AVERROR_INVALIDDATA;
                }
            }
        } else {
            for (j = 0; j < len2; j++)
                *dst++ = v;
        }
    }

    b->cur_dec = (uint8_t*)dst;
    return 0;
}

/**
 * Retrieve next value from bundle.
 *
 * @param c      decoder context
 * @param bundle bundle number
 */
static inline int get_value(BinkContext *c, int bundle)
{
    int ret;

    if (bundle < BINK_SRC_X_OFF || bundle == BINK_SRC_RUN)
        return *c->bundle[bundle].cur_ptr++;
    if (bundle == BINK_SRC_X_OFF || bundle == BINK_SRC_Y_OFF)
        return (int8_t)*c->bundle[bundle].cur_ptr++;
    ret = *(int16_t*)c->bundle[bundle].cur_ptr;
    c->bundle[bundle].cur_ptr += 2;
    return ret;
}

/**
 * Read 8x8 block of DCT coefficients.
 *
 * @param gb       context for reading bits
 * @param block    place for storing coefficients
 * @param scan     scan order table
 * @param quant_matrices quantization matrices
 * @return 0 for success, negative value in other cases
 */
static int read_dct_coeffs(BinkContext *c, GetBitContext *gb, int32_t block[64],
                           const uint8_t *scan, int *coef_count_,
                           int coef_idx[64], int q)
{
    int coef_list[128];
    int mode_list[128];
    int i, t, bits, ccoef, mode, sign;
    int list_start = 64, list_end = 64, list_pos;
    int coef_count = 0;
    int quant_idx;

    if (get_bits_left(gb) < 4)
        return AVERROR_INVALIDDATA;

    coef_list[list_end] = 4;  mode_list[list_end++] = 0;
    coef_list[list_end] = 24; mode_list[list_end++] = 0;
    coef_list[list_end] = 44; mode_list[list_end++] = 0;
    coef_list[list_end] = 1;  mode_list[list_end++] = 3;
    coef_list[list_end] = 2;  mode_list[list_end++] = 3;
    coef_list[list_end] = 3;  mode_list[list_end++] = 3;

    for (bits = get_bits(gb, 4) - 1; bits >= 0; bits--) {
        list_pos = list_start;
        while (list_pos < list_end) {
            if (!(mode_list[list_pos] | coef_list[list_pos]) || !get_bits1(gb)) {
                list_pos++;
                continue;
            }
            ccoef = coef_list[list_pos];
            mode  = mode_list[list_pos];
            switch (mode) {
            case 0:
                coef_list[list_pos] = ccoef + 4;
                mode_list[list_pos] = 1;
                [[fallthrough]];
            case 2:
                if (mode == 2) {
                    coef_list[list_pos]   = 0;
                    mode_list[list_pos++] = 0;
                }
                for (i = 0; i < 4; i++, ccoef++) {
                    if (get_bits1(gb)) {
                        coef_list[--list_start] = ccoef;
                        mode_list[  list_start] = 3;
                    } else {
                        if (!bits) {
                            t = 1 - (get_bits1(gb) << 1);
                        } else {
                            t = get_bits(gb, bits) | 1 << bits;
                            sign = -get_bits1(gb);
                            t = (t ^ sign) - sign;
                        }
                        block[scan[ccoef]] = t;
                        coef_idx[coef_count++] = ccoef;
                    }
                }
                break;
            case 1:
                mode_list[list_pos] = 2;
                for (i = 0; i < 3; i++) {
                    ccoef += 4;
                    coef_list[list_end]   = ccoef;
                    mode_list[list_end++] = 2;
                }
                break;
            case 3:
                if (!bits) {
                    t = 1 - (get_bits1(gb) << 1);
                } else {
                    t = get_bits(gb, bits) | 1 << bits;
                    sign = -get_bits1(gb);
                    t = (t ^ sign) - sign;
                }
                block[scan[ccoef]] = t;
                coef_idx[coef_count++] = ccoef;
                coef_list[list_pos]   = 0;
                mode_list[list_pos++] = 0;
                break;
            }
        }
    }

    if (q == -1) {
        quant_idx = get_bits(gb, 4);
    } else {
        quant_idx = q;
        if (quant_idx < 0 || quant_idx > 15) {
            av_log(c->avctx, AV_LOG_ERROR, "quant_index %d out of range\n", quant_idx);
            return AVERROR_INVALIDDATA;
        }
    }

    *coef_count_ = coef_count;

    return quant_idx;
}

static void unquantize_dct_coeffs(int32_t block[64], const int32_t quant[64],
                                  int coef_count, int coef_idx[64],
                                  const uint8_t *scan)
{
    int i;
    block[0] = (int)(block[0] * quant[0]) >> 11;
    for (i = 0; i < coef_count; i++) {
        int idx = coef_idx[i];
        block[scan[idx]] = (int)(block[scan[idx]] * quant[idx]) >> 11;
    }
}

/**
 * Read 8x8 block with residue after motion compensation.
 *
 * @param gb          context for reading bits
 * @param block       place to store read data
 * @param masks_count number of masks to decode
 * @return 0 on success, negative value in other cases
 */
static int read_residue(GetBitContext *gb, int16_t block[64], int masks_count)
{
    int coef_list[128];
    int mode_list[128];
    int i, sign, mask, ccoef, mode;
    int list_start = 64, list_end = 64, list_pos;
    int nz_coeff[64];
    int nz_coeff_count = 0;

    coef_list[list_end] =  4; mode_list[list_end++] = 0;
    coef_list[list_end] = 24; mode_list[list_end++] = 0;
    coef_list[list_end] = 44; mode_list[list_end++] = 0;
    coef_list[list_end] =  0; mode_list[list_end++] = 2;

    for (mask = 1 << get_bits(gb, 3); mask; mask >>= 1) {
        for (i = 0; i < nz_coeff_count; i++) {
            if (!get_bits1(gb))
                continue;
            if (block[nz_coeff[i]] < 0)
                block[nz_coeff[i]] -= mask;
            else
                block[nz_coeff[i]] += mask;
            masks_count--;
            if (masks_count < 0)
                return 0;
        }
        list_pos = list_start;
        while (list_pos < list_end) {
            if (!(coef_list[list_pos] | mode_list[list_pos]) || !get_bits1(gb)) {
                list_pos++;
                continue;
            }
            ccoef = coef_list[list_pos];
            mode  = mode_list[list_pos];
            switch (mode) {
            case 0:
                coef_list[list_pos] = ccoef + 4;
                mode_list[list_pos] = 1;
                [[fallthrough]];
            case 2:
                if (mode == 2) {
                    coef_list[list_pos]   = 0;
                    mode_list[list_pos++] = 0;
                }
                for (i = 0; i < 4; i++, ccoef++) {
                    if (get_bits1(gb)) {
                        coef_list[--list_start] = ccoef;
                        mode_list[  list_start] = 3;
                    } else {
                        nz_coeff[nz_coeff_count++] = bink_scan[ccoef];
                        sign = -get_bits1(gb);
                        block[bink_scan[ccoef]] = (mask ^ sign) - sign;
                        masks_count--;
                        if (masks_count < 0)
                            return 0;
                    }
                }
                break;
            case 1:
                mode_list[list_pos] = 2;
                for (i = 0; i < 3; i++) {
                    ccoef += 4;
                    coef_list[list_end]   = ccoef;
                    mode_list[list_end++] = 2;
                }
                break;
            case 3:
                nz_coeff[nz_coeff_count++] = bink_scan[ccoef];
                sign = -get_bits1(gb);
                block[bink_scan[ccoef]] = (mask ^ sign) - sign;
                coef_list[list_pos]   = 0;
                mode_list[list_pos++] = 0;
                masks_count--;
                if (masks_count < 0)
                    return 0;
                break;
            }
        }
    }

    return 0;
}

static int bink_put_pixels(BinkContext *c,
                           uint8_t *dst, uint8_t *prev, int stride,
                           uint8_t *ref_start,
                           uint8_t *ref_end)
{
    int xoff     = get_value(c, BINK_SRC_X_OFF);
    int yoff     = get_value(c, BINK_SRC_Y_OFF);
    uint8_t *ref = prev + xoff + yoff * stride;
    if (ref < ref_start || ref > ref_end) {
        av_log(c->avctx, AV_LOG_ERROR, "Copy out of bounds @%d, %d\n",
               xoff, yoff);
        return AVERROR_INVALIDDATA;
    }
    c->put_pixels_tab(dst, ref, stride, 8);

    return 0;
}

static int bink_decode_plane(BinkContext *c, AVFrame *frame, GetBitContext *gb,
                             int plane_idx, int is_chroma)
{
    int blk, ret;
    int i, j, bx, by;
    uint8_t *dst, *prev, *ref_start, *ref_end;
    int v, col[2];
    const uint8_t *scan;
    LOCAL_ALIGNED_32(int16_t, block, [64]);
    LOCAL_ALIGNED_16(uint8_t, ublock, [64]);
    LOCAL_ALIGNED_16(int32_t, dctblock, [64]);
    int coordmap[64], quant_idx, coef_count, coef_idx[64];

    const int stride = frame->linesize[plane_idx];
    int bw = is_chroma ? (c->avctx->width  + 15) >> 4 : (c->avctx->width  + 7) >> 3;
    int bh = is_chroma ? (c->avctx->height + 15) >> 4 : (c->avctx->height + 7) >> 3;
    int width = c->avctx->width >> is_chroma;
    int height = c->avctx->height >> is_chroma;

    if (c->version == 'k' && get_bits1(gb)) {
        int fill = get_bits(gb, 8);

        dst = frame->data[plane_idx];

        for (i = 0; i < height; i++)
            memset(dst + i * stride, fill, width);
        goto end;
    }

    init_lengths(c, FFMAX(width, 8), bw);
    for (i = 0; i < BINK_NB_SRC; i++) {
        ret = read_bundle(gb, c, i);
        if (ret < 0)
            return ret;
    }

    ref_start = c->last->data[plane_idx] ? c->last->data[plane_idx]
                                         : frame->data[plane_idx];
    ref_end   = ref_start
                + (bw - 1 + c->last->linesize[plane_idx] * (bh - 1)) * 8;

    for (i = 0; i < 64; i++)
        coordmap[i] = (i & 7) + (i >> 3) * stride;

    for (by = 0; by < bh; by++) {
        if ((ret = read_block_types(c->avctx, gb, &c->bundle[BINK_SRC_BLOCK_TYPES])) < 0)
            return ret;
        if ((ret = read_block_types(c->avctx, gb, &c->bundle[BINK_SRC_SUB_BLOCK_TYPES])) < 0)
            return ret;
        if ((ret = read_colors(gb, &c->bundle[BINK_SRC_COLORS], c)) < 0)
            return ret;
        if ((ret = read_patterns(c->avctx, gb, &c->bundle[BINK_SRC_PATTERN])) < 0)
            return ret;
        if ((ret = read_motion_values(c->avctx, gb, &c->bundle[BINK_SRC_X_OFF])) < 0)
            return ret;
        if ((ret = read_motion_values(c->avctx, gb, &c->bundle[BINK_SRC_Y_OFF])) < 0)
            return ret;
        if ((ret = read_dcs(c->avctx, gb, &c->bundle[BINK_SRC_INTRA_DC], DC_START_BITS, 0)) < 0)
            return ret;
        if ((ret = read_dcs(c->avctx, gb, &c->bundle[BINK_SRC_INTER_DC], DC_START_BITS, 1)) < 0)
            return ret;
        if ((ret = read_runs(c->avctx, gb, &c->bundle[BINK_SRC_RUN])) < 0)
            return ret;

        dst  = frame->data[plane_idx]  + 8*by*stride;
        prev = (c->last->data[plane_idx] ? c->last->data[plane_idx]
                                         : frame->data[plane_idx]) + 8*by*stride;
        for (bx = 0; bx < bw; bx++, dst += 8, prev += 8) {
            blk = get_value(c, BINK_SRC_BLOCK_TYPES);
            // 16x16 block type on odd line means part of the already decoded block, so skip it
            if (((by & 1) || (bx & 1)) && blk == SCALED_BLOCK) {
                bx++;
                dst  += 8;
                prev += 8;
                continue;
            }
            switch (blk) {
            case SKIP_BLOCK:
                c->put_pixels_tab(dst, prev, stride, 8);
                break;
            case SCALED_BLOCK:
                blk = get_value(c, BINK_SRC_SUB_BLOCK_TYPES);
                switch (blk) {
                case RUN_BLOCK:
                    if (get_bits_left(gb) < 4)
                        return AVERROR_INVALIDDATA;
                    scan = bink_patterns[get_bits(gb, 4)];
                    i = 0;
                    do {
                        int run = get_value(c, BINK_SRC_RUN) + 1;

                        i += run;
                        if (i > 64) {
                            av_log(c->avctx, AV_LOG_ERROR, "Run went out of bounds\n");
                            return AVERROR_INVALIDDATA;
                        }
                        if (get_bits1(gb)) {
                            v = get_value(c, BINK_SRC_COLORS);
                            for (j = 0; j < run; j++)
                                ublock[*scan++] = v;
                        } else {
                            for (j = 0; j < run; j++)
                                ublock[*scan++] = get_value(c, BINK_SRC_COLORS);
                        }
                    } while (i < 63);
                    if (i == 63)
                        ublock[*scan++] = get_value(c, BINK_SRC_COLORS);
                    break;
                case INTRA_BLOCK:
                    memset(dctblock, 0, sizeof(*dctblock) * 64);
                    dctblock[0] = get_value(c, BINK_SRC_INTRA_DC);
                    if ((quant_idx = read_dct_coeffs(c, gb, dctblock, bink_scan, &coef_count, coef_idx, -1)) < 0)
                        return quant_idx;
                    unquantize_dct_coeffs(dctblock, bink_intra_quant[quant_idx], coef_count, coef_idx, bink_scan);
                    c->binkdsp.idct_put(ublock, 8, dctblock);
                    break;
                case FILL_BLOCK:
                    v = get_value(c, BINK_SRC_COLORS);
                    c->bdsp.fill_block_tab[0](dst, v, stride, 16);
                    break;
                case PATTERN_BLOCK:
                    for (i = 0; i < 2; i++)
                        col[i] = get_value(c, BINK_SRC_COLORS);
                    for (j = 0; j < 8; j++) {
                        v = get_value(c, BINK_SRC_PATTERN);
                        for (i = 0; i < 8; i++, v >>= 1)
                            ublock[i + j*8] = col[v & 1];
                    }
                    break;
                case RAW_BLOCK:
                    for (j = 0; j < 8; j++)
                        for (i = 0; i < 8; i++)
                            ublock[i + j*8] = get_value(c, BINK_SRC_COLORS);
                    break;
                default:
                    av_log(c->avctx, AV_LOG_ERROR, "Incorrect 16x16 block type %d\n", blk);
                    return AVERROR_INVALIDDATA;
                }
                if (blk != FILL_BLOCK)
                c->binkdsp.scale_block(ublock, dst, stride);
                bx++;
                dst  += 8;
                prev += 8;
                break;
            case MOTION_BLOCK:
                ret = bink_put_pixels(c, dst, prev, stride,
                                      ref_start, ref_end);
                if (ret < 0)
                    return ret;
                break;
            case RUN_BLOCK:
                scan = bink_patterns[get_bits(gb, 4)];
                i = 0;
                do {
                    int run = get_value(c, BINK_SRC_RUN) + 1;

                    i += run;
                    if (i > 64) {
                        av_log(c->avctx, AV_LOG_ERROR, "Run went out of bounds\n");
                        return AVERROR_INVALIDDATA;
                    }
                    if (get_bits1(gb)) {
                        v = get_value(c, BINK_SRC_COLORS);
                        for (j = 0; j < run; j++)
                            dst[coordmap[*scan++]] = v;
                    } else {
                        for (j = 0; j < run; j++)
                            dst[coordmap[*scan++]] = get_value(c, BINK_SRC_COLORS);
                    }
                } while (i < 63);
                if (i == 63)
                    dst[coordmap[*scan++]] = get_value(c, BINK_SRC_COLORS);
                break;
            case RESIDUE_BLOCK:
                ret = bink_put_pixels(c, dst, prev, stride,
                                      ref_start, ref_end);
                if (ret < 0)
                    return ret;
                c->bdsp.clear_block(block);
                v = get_bits(gb, 7);
                read_residue(gb, block, v);
                c->binkdsp.add_pixels8(dst, block, stride);
                break;
            case INTRA_BLOCK:
                memset(dctblock, 0, sizeof(*dctblock) * 64);
                dctblock[0] = get_value(c, BINK_SRC_INTRA_DC);
                if ((quant_idx = read_dct_coeffs(c, gb, dctblock, bink_scan, &coef_count, coef_idx, -1)) < 0)
                    return quant_idx;
                unquantize_dct_coeffs(dctblock, bink_intra_quant[quant_idx], coef_count, coef_idx, bink_scan);
                c->binkdsp.idct_put(dst, stride, dctblock);
                break;
            case FILL_BLOCK:
                v = get_value(c, BINK_SRC_COLORS);
                c->bdsp.fill_block_tab[1](dst, v, stride, 8);
                break;
            case INTER_BLOCK:
                ret = bink_put_pixels(c, dst, prev, stride,
                                      ref_start, ref_end);
                if (ret < 0)
                    return ret;
                memset(dctblock, 0, sizeof(*dctblock) * 64);
                dctblock[0] = get_value(c, BINK_SRC_INTER_DC);
                if ((quant_idx = read_dct_coeffs(c, gb, dctblock, bink_scan, &coef_count, coef_idx, -1)) < 0)
                    return quant_idx;
                unquantize_dct_coeffs(dctblock, bink_inter_quant[quant_idx], coef_count, coef_idx, bink_scan);
                c->binkdsp.idct_add(dst, stride, dctblock);
                break;
            case PATTERN_BLOCK:
                for (i = 0; i < 2; i++)
                    col[i] = get_value(c, BINK_SRC_COLORS);
                for (i = 0; i < 8; i++) {
                    v = get_value(c, BINK_SRC_PATTERN);
                    for (j = 0; j < 8; j++, v >>= 1)
                        dst[i*stride + j] = col[v & 1];
                }
                break;
            case RAW_BLOCK:
                for (i = 0; i < 8; i++)
                    memcpy(dst + i*stride, c->bundle[BINK_SRC_COLORS].cur_ptr + i*8, 8);
                c->bundle[BINK_SRC_COLORS].cur_ptr += 64;
                break;
            default:
                av_log(c->avctx, AV_LOG_ERROR, "Unknown block type %d\n", blk);
                return AVERROR_INVALIDDATA;
            }
        }
    }

end:
    if (get_bits_count(gb) & 0x1F) //next plane data starts at 32-bit boundary
        skip_bits_long(gb, 32 - (get_bits_count(gb) & 0x1F));

    return 0;
}

static int decode_frame(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *pkt)
{
    BinkContext * const c = static_cast<BinkContext*>(avctx->priv_data);
    AVFrame *frame = static_cast<AVFrame*>(data);
    GetBitContext gb;
    int plane, plane_idx, ret;
    int bits_count = pkt->size << 3;

    init_get_bits(&gb, pkt->data, bits_count);
    if (c->has_alpha) {
        if (c->version >= 'i')
            skip_bits_long(&gb, 32);
        if ((ret = bink_decode_plane(c, frame, &gb, 3, 0)) < 0)
            return ret;
    }
    if (c->version >= 'i')
        skip_bits_long(&gb, 32);

    for (plane = 0; plane < 3; plane++) {
        plane_idx = (!plane || !c->swap_planes) ? plane : (plane ^ 3);
        if ((ret = bink_decode_plane(c, frame, &gb, plane_idx, !!plane)) < 0)
            return ret;
        if (get_bits_count(&gb) >= bits_count)
            break;
    }
    emms_c();

    if (gb.failed)
        return AVERROR_INVALIDDATA;
    c->last = frame;

    *got_frame = 1;

    /* always report that the buffer was completely consumed */
    return pkt->size;
}

static av_cold int decode_init(AVCodecContext *avctx)
{
    BinkContext * const c = static_cast<BinkContext*>(avctx->priv_data);
    HpelDSPContext hdsp;
    int ret;
    int flags;

    c->version = avctx->codec_tag >> 24;
    if (avctx->extradata_size < 4) {
        av_log(avctx, AV_LOG_ERROR, "Extradata missing or too short\n");
        return AVERROR_INVALIDDATA;
    }
    flags = AV_RL32(avctx->extradata);
    c->has_alpha = flags & BINK_FLAG_ALPHA;
    c->swap_planes = c->version >= 'h';
    c->avctx = avctx;

    c->last = &c->empty;

    ff_blockdsp_init(&c->bdsp, avctx);
    ff_hpeldsp_init(&hdsp, avctx->flags);
    c->put_pixels_tab = hdsp.put_pixels_tab[1][0];
    ff_binkdsp_init(&c->binkdsp);

    if ((ret = init_bundles(c)) < 0)
        return ret;

    return 0;
}

static av_cold int decode_end(AVCodecContext *avctx)
{
    BinkContext * const c = static_cast<BinkContext*>(avctx->priv_data);

    free_bundles(c);
    return 0;
}

namespace
{
struct DecoderContext
{
    AVCodecContext av;
    BinkContext codec;
    AVFrame frames[2];
    std::uint8_t flags[4]{};
};

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}
}
namespace sl_open::media
{
bool bink_video_init(
    BinkVideoDecoder& decoder,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t tag,
    std::uint32_t flags)
{
    bink_video_shutdown(decoder);
    if (width == 0 || width > 640 || height == 0 || height > 480
        || (tag & 0x00ffffff) != 0x004b4942
        || (tag >> 24 != 'f' && tag >> 24 != 'g' && tag >> 24 != 'i')
        || flags != 0)
    {
        return false;
    }

    auto* context = new (std::nothrow) DecoderContext{};
    if (context == nullptr)
    {
        return false;
    }

    const std::uint32_t y_stride = align_up(width, 16);
    const std::uint32_t y_rows = align_up(height, 16);
    const std::uint32_t chroma_stride = align_up((width + 1) / 2, 16);
    const std::uint32_t chroma_rows = align_up((height + 1) / 2, 8);
    const std::size_t y_bytes =
        static_cast<std::size_t>(y_stride) * y_rows;
    const std::size_t chroma_bytes =
        static_cast<std::size_t>(chroma_stride) * chroma_rows;
    const std::size_t frame_bytes = y_bytes + chroma_bytes * 2;
    if (!decoder.pixels.allocate(frame_bytes * 2))
    {
        delete context;
        return false;
    }
    std::memset(decoder.pixels.data, 0, decoder.pixels.size);

    for (std::uint32_t index = 0; index < 2; ++index)
    {
        std::uint8_t* pixels = decoder.pixels.data + frame_bytes * index;
        context->frames[index].data[0] = pixels;
        context->frames[index].data[1] = pixels + y_bytes;
        context->frames[index].data[2] = pixels + y_bytes + chroma_bytes;
        context->frames[index].linesize[0] = static_cast<int>(y_stride);
        context->frames[index].linesize[1] =
            static_cast<int>(chroma_stride);
        context->frames[index].linesize[2] =
            static_cast<int>(chroma_stride);
    }

    context->av.priv_data = &context->codec;
    context->av.extradata = context->flags;
    context->av.extradata_size = sizeof(context->flags);
    context->av.width = static_cast<int>(width);
    context->av.height = static_cast<int>(height);
    context->av.codec_tag = tag;
    if (decode_init(&context->av) < 0)
    {
        decoder.pixels.reset();
        delete context;
        return false;
    }

    decoder.context = context;
    decoder.frame.width = width;
    decoder.frame.height = height;
    decoder.frame.strides[0] = y_stride;
    decoder.frame.strides[1] = chroma_stride;
    decoder.frame.strides[2] = chroma_stride;
    return true;
}

bool bink_video_decode(
    BinkVideoDecoder& decoder,
    const std::uint8_t* packet,
    std::size_t packet_size)
{
    if (decoder.context == nullptr || packet == nullptr
        || packet_size == 0 || packet_size > INT_MAX)
    {
        return false;
    }
    auto& context = *static_cast<DecoderContext*>(decoder.context);
    AVFrame& output = context.frames[decoder.frame_index & 1];
    AVPacket input{
        const_cast<std::uint8_t*>(packet),
        static_cast<int>(packet_size),
    };
    int got_frame = 0;
    if (decode_frame(&context.av, &output, &got_frame, &input) < 0
        || got_frame == 0)
    {
        return false;
    }
    for (int plane = 0; plane < 3; ++plane)
    {
        decoder.frame.planes[plane] = output.data[plane];
    }
    ++decoder.frame_index;
    return true;
}

void bink_video_shutdown(BinkVideoDecoder& decoder)
{
    if (decoder.context != nullptr)
    {
        auto* context = static_cast<DecoderContext*>(decoder.context);
        decode_end(&context->av);
        delete context;
    }
    decoder.pixels.reset();
    decoder.context = nullptr;
    decoder.frame = {};
    decoder.frame_index = 0;
}
}
