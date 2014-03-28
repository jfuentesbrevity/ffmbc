/*
 * VC3/DNxHD encoder
 * Copyright (c) 2007 Baptiste Coudurier <baptiste dot coudurier at smartjog dot com>
 * Copyright (c) 2011 MirriAd Ltd
 *
 * VC-3 encoder funded by the British Broadcasting Corporation
 * 10 bit support added by MirriAd Ltd, Joseph Artsimovich <joseph@mirriad.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation;
 * version 2 of the License.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

//#define DEBUG
#define RC_VARIANCE 1 // use variance or ssd for fast rc

#include "libavutil/opt.h"
#include "avcodec.h"
#include "dnxhdenc.h"

#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[]={
    {"nitris_compat", "encode with Avid Nitris compatibility", offsetof(DNXHDEncContext, nitris_compat), FF_OPT_TYPE_INT, {.dbl = 0}, 0, 1, VE},
    {"qmax", "max video quantizer scale", offsetof(DNXHDEncContext, qmax), FF_OPT_TYPE_INT, {.dbl = 0}, 0, 1024, VE},
{NULL}
};
static const AVClass class = { "dnxhd", av_default_item_name, options, LIBAVUTIL_VERSION_INT };

#define QUANT_BIAS_SHIFT 8
#define QMAT_SHIFT_MMX 16
#define QMAT_SHIFT 18

static int dnxhd_dct_quantize(DNXHDEncContext *ctx, DCTELEM *block, int qscale)
{
    const uint8_t *scantable= ctx->scantable.scantable;
    const int *qmat = ctx->cur_qmatrix[qscale];
    int last_non_zero = 0;
    int bias = ctx->quant_bias << (QMAT_SHIFT - QUANT_BIAS_SHIFT);
    unsigned threshold1 = (1<<QMAT_SHIFT) - bias - 1;
    unsigned threshold2 = (threshold1<<1);

    ctx->dsp.fdct(block);

    if (ctx->cid_table->bit_depth == 8)
        block[0] = (block[0] + 4) >> 3;
    else
        block[0] = (block[0] + 2) >> 2;

    for (int i = 1; i < 64; ++i) {
        int j = scantable[i];
        int level = block[j] * qmat[ff_zigzag_direct[i]];
        if ((unsigned)(level+threshold1) > threshold2) {
            if (level > 0) {
                level = (bias + level)>>QMAT_SHIFT;
                block[j] = level;
            } else {
                level = (bias - level)>>QMAT_SHIFT;
                block[j] = -level;
            }
            last_non_zero = i;
        } else {
            block[j] = 0;
        }
    }

    /* we need this permutation so that we correct the IDCT, we only permute the !=0 elements */
    if (ctx->dsp.idct_permutation_type != FF_NO_IDCT_PERM)
        ff_block_permute(block, ctx->dsp.idct_permutation, scantable, last_non_zero);

    return last_non_zero;
}

#define LAMBDA_FRAC_BITS 10

static void dnxhd_get_pixels_8x4_sym_8(DCTELEM *restrict block, const uint8_t *pixels, int line_size)
{
    int i;
    for (i = 0; i < 4; i++) {
        block[0] = pixels[0]; block[1] = pixels[1];
        block[2] = pixels[2]; block[3] = pixels[3];
        block[4] = pixels[4]; block[5] = pixels[5];
        block[6] = pixels[6]; block[7] = pixels[7];
        pixels += line_size;
        block += 8;
    }
    memcpy(block,      block -  8, sizeof(*block) * 8);
    memcpy(block +  8, block - 16, sizeof(*block) * 8);
    memcpy(block + 16, block - 24, sizeof(*block) * 8);
    memcpy(block + 24, block - 32, sizeof(*block) * 8);
}

static void dnxhd_get_pixels_8x4_sym_10(DCTELEM *restrict block, const uint8_t *pixels, int line_size)
{
    int i;

    block += 32;

    for (i = 0; i < 4; i++) {
        memcpy(block + i     * 8, pixels + i * line_size, 8 * sizeof(*block));
        memcpy(block - (i+1) * 8, pixels + i * line_size, 8 * sizeof(*block));
    }
}

static int dnxhd_init_vlc(DNXHDEncContext *ctx)
{
    int i, j, level, run;
    int max_level = 1<<(ctx->cid_table->bit_depth+2);

    FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->vlc_codes, max_level*4*sizeof(*ctx->vlc_codes), fail);
    FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->vlc_bits , max_level*4*sizeof(*ctx->vlc_bits ), fail);
    FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->run_codes, 63*2                               , fail);
    FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->run_bits , 63                                 , fail);

    ctx->vlc_codes += max_level*2;
    ctx->vlc_bits  += max_level*2;
    for (level = -max_level; level < max_level; level++) {
        for (run = 0; run < 2; run++) {
            int index = (level<<1)|run;
            int sign, offset = 0, alevel = level;

            MASK_ABS(sign, alevel);
            if (alevel > 64) {
                offset = (alevel-1)>>6;
                alevel -= offset<<6;
            }
            for (j = 0; j < 257; j++) {
                if (ctx->cid_table->ac_level[j] >> 1 == alevel &&
                    (!offset || (ctx->cid_table->ac_flags[j] & 1) && offset) &&
                    (!run    || (ctx->cid_table->ac_flags[j] & 2) && run)) {
                    assert(!ctx->vlc_codes[index]);
                    if (alevel) {
                        ctx->vlc_codes[index] = (ctx->cid_table->ac_codes[j]<<1)|(sign&1);
                        ctx->vlc_bits [index] = ctx->cid_table->ac_bits[j]+1;
                    } else {
                        ctx->vlc_codes[index] = ctx->cid_table->ac_codes[j];
                        ctx->vlc_bits [index] = ctx->cid_table->ac_bits [j];
                    }
                    break;
                }
            }
            assert(!alevel || j < 257);
            if (offset) {
                ctx->vlc_codes[index] = (ctx->vlc_codes[index]<<ctx->cid_table->index_bits)|offset;
                ctx->vlc_bits [index]+= ctx->cid_table->index_bits;
            }
        }
    }
    for (i = 0; i < 62; i++) {
        int run = ctx->cid_table->run[i];
        assert(run < 63);
        ctx->run_codes[run] = ctx->cid_table->run_codes[i];
        ctx->run_bits [run] = ctx->cid_table->run_bits[i];
    }
    return 0;
 fail:
    return -1;
}

static int dnxhd_init_qmat(DNXHDEncContext *ctx, int lbias, int cbias)
{
    int64_t num = ctx->cid_table->bit_depth == 8 ? 4 : 2;
    int q, i;

    if (!ctx->qmax)
        ctx->avctx->qmax = ctx->qmax = ctx->avctx->mb_decision == FF_MB_DECISION_RD ? 31 : 1024;

    FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->qmatrix_l,   (ctx->qmax+1) * 64 *     sizeof(int),      fail);
    FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->qmatrix_c,   (ctx->qmax+1) * 64 *     sizeof(int),      fail);
    FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->qmatrix_l16, (ctx->qmax+1) * 64 * 2 * sizeof(uint16_t), fail);
    FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->qmatrix_c16, (ctx->qmax+1) * 64 * 2 * sizeof(uint16_t), fail);

    for (q = 1; q <= ctx->qmax; q++) {
        for (i = 1; i < 64; i++) {
            const int bias = ctx->quant_bias;
            ctx->qmatrix_l[q][i] = (num << QMAT_SHIFT) / (q * ctx->cid_table->luma_weight[i]);
            ctx->qmatrix_c[q][i] = (num << QMAT_SHIFT) / (q * ctx->cid_table->chroma_weight[i]);

            ctx->qmatrix_l16[q][0][i]= (num << QMAT_SHIFT_MMX) / (q * ctx->cid_table->luma_weight[i]);
            ctx->qmatrix_l16[q][1][i] = ROUNDED_DIV(bias<<(16-QUANT_BIAS_SHIFT), ctx->qmatrix_l16[q][0][i]);
            ctx->qmatrix_c16[q][0][i]= (num << QMAT_SHIFT_MMX) / (q * ctx->cid_table->chroma_weight[i]);
            ctx->qmatrix_c16[q][1][i] = ROUNDED_DIV(bias<<(16-QUANT_BIAS_SHIFT), ctx->qmatrix_c16[q][0][i]);
        }
    }

    return 0;
 fail:
    return -1;
}

static int dnxhd_init_rc(DNXHDEncContext *ctx)
{
    FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->mb_rc, 8160*(ctx->qmax+1)*sizeof(RCEntry), fail);
    if (ctx->avctx->mb_decision != FF_MB_DECISION_RD)
        FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->mb_cmp, ctx->mb_num*sizeof(RCCMPEntry), fail);

    ctx->frame_bits = (ctx->cid_table->coding_unit_size - 640 - 4 - ctx->min_padding) * 8;
    ctx->qscale = 1;
    ctx->lambda = 2<<LAMBDA_FRAC_BITS; // qscale 2
    return 0;
 fail:
    return -1;
}

static int dnxhd_encode_init(AVCodecContext *avctx)
{
    DNXHDEncContext *ctx = avctx->priv_data;
    int i, index, bit_depth;

    switch (avctx->pix_fmt) {
    case PIX_FMT_YUV422P:
    case PIX_FMT_YUVA422P:
        bit_depth = 8;
        break;
    case PIX_FMT_YUV422P10:
        bit_depth = 10;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "pixel format is incompatible with DNxHD\n");
        return -1;
    }

    if (!((avctx->width == 1920 && avctx->height == 1080) ||
          avctx->width == 1280 && avctx->height == 720)) {
        av_log(avctx, AV_LOG_ERROR, "video resolution not supported, use 1920x1080 or 1280x720\n");
        return -1;
    }
    if (avctx->width == 1280 && avctx->height == 720 && avctx->flags & CODEC_FLAG_INTERLACED_DCT) {
        av_log(avctx, AV_LOG_ERROR, "1280x720 interlaced is not supported\n");
        return -1;
    }
    ctx->cid = ff_dnxhd_find_cid(avctx, bit_depth);
    if (!ctx->cid) {
        av_log(avctx, AV_LOG_ERROR, "could not find encoding profile\n");
        if (avctx->pix_fmt == PIX_FMT_YUV422P || avctx->pix_fmt == PIX_FMT_YUVA422P) {
            av_log(avctx, AV_LOG_INFO, "available bitrates in Mb/s for 8bits:\n");
            av_log(avctx, AV_LOG_INFO, "1080p24: 36, 115, 175\n");
            av_log(avctx, AV_LOG_INFO, "1080p25: 36, 120, 185\n");
            av_log(avctx, AV_LOG_INFO, "1080p30: 45, 145, 220\n");
            av_log(avctx, AV_LOG_INFO, "1080p50: 75, 240, 365\n");
            av_log(avctx, AV_LOG_INFO, "1080p60: 90, 290, 440\n");
            av_log(avctx, AV_LOG_INFO, "1080i25: 120, 185\n");
            av_log(avctx, AV_LOG_INFO, "1080i30: 145, 220\n");
            av_log(avctx, AV_LOG_INFO, "720p24: 60, 90\n");
            av_log(avctx, AV_LOG_INFO, "720p25: 60, 90\n");
            av_log(avctx, AV_LOG_INFO, "720p30: 75, 110\n");
            av_log(avctx, AV_LOG_INFO, "720p50: 120, 185\n");
            av_log(avctx, AV_LOG_INFO, "720p60: 145, 220\n");
        } else {
            av_log(avctx, AV_LOG_INFO, "available bitrates in Mb/s for 10bits:\n");
            av_log(avctx, AV_LOG_INFO, "1080p24: 175\n");
            av_log(avctx, AV_LOG_INFO, "1080p25: 185\n");
            av_log(avctx, AV_LOG_INFO, "1080p30: 220\n");
            av_log(avctx, AV_LOG_INFO, "1080p50: 365\n");
            av_log(avctx, AV_LOG_INFO, "1080p60: 440\n");
            av_log(avctx, AV_LOG_INFO, "1080i25: 185\n");
            av_log(avctx, AV_LOG_INFO, "1080i30: 220\n");
            av_log(avctx, AV_LOG_INFO, "720p24: 90\n");
            av_log(avctx, AV_LOG_INFO, "720p25: 90\n");
            av_log(avctx, AV_LOG_INFO, "720p30: 110\n");
            av_log(avctx, AV_LOG_INFO, "720p50: 185\n");
            av_log(avctx, AV_LOG_INFO, "720p60: 220\n");
        }
        return -1;
    }
    av_log(avctx, AV_LOG_DEBUG, "cid %d\n", ctx->cid);

    index = ff_dnxhd_get_cid_table(ctx->cid);
    ctx->cid_table = &ff_dnxhd_cid_table[index];

    ctx->avctx = avctx;

    avctx->bits_per_raw_sample = ctx->cid_table->bit_depth;

    if (avctx->dct_algo != FF_DCT_INT && avctx->dct_algo != FF_DCT_AUTO) {
        av_log(avctx, AV_LOG_ERROR, "error, dct algorithm not supported\n");
        return -1;
    }
    if (avctx->idct_algo != FF_IDCT_SIMPLE && avctx->idct_algo != FF_IDCT_AUTO) {
        av_log(avctx, AV_LOG_ERROR, "error, idct algorithm not supported\n");
        return -1;
    }
    dsputil_init(&ctx->dsp, avctx);

    ff_init_scantable(ctx->dsp.idct_permutation, &ctx->scantable, ff_zigzag_direct);

    if (!ctx->dct_quantize)
        ctx->dct_quantize = dnxhd_dct_quantize;

    if (ctx->cid_table->bit_depth == 10) {
       ctx->get_pixels_8x4_sym = dnxhd_get_pixels_8x4_sym_10;
       ctx->block_width_l2 = 4;
    } else {
       ctx->get_pixels_8x4_sym = dnxhd_get_pixels_8x4_sym_8;
       ctx->block_width_l2 = 3;
    }

#if HAVE_MMX
    ff_dnxhd_init_mmx(ctx);
#endif

    ctx->mb_height = (avctx->height + 15) / 16;
    ctx->mb_width  = (avctx->width  + 15) / 16;

    if (avctx->flags & CODEC_FLAG_INTERLACED_DCT) {
        ctx->interlaced = 1;
        ctx->mb_height /= 2;
    }

    avctx->bit_rate = ctx->cid_table->coding_unit_size * 8LL * (1<<ctx->interlaced) *
        avctx->time_base.den / avctx->time_base.num;

    ctx->mb_num = ctx->mb_height * ctx->mb_width;

    ctx->quant_bias = 3<<(QUANT_BIAS_SHIFT-3); //(a + x*3/8)/x
    if (avctx->intra_quant_bias != FF_DEFAULT_QUANT_BIAS)
        ctx->quant_bias = avctx->intra_quant_bias;
    if (dnxhd_init_qmat(ctx, ctx->quant_bias, 0) < 0) // XXX tune lbias/cbias
        return -1;

    // Avid Nitris hardware decoder requires a minimum amount of padding in the coding unit payload
    if (ctx->nitris_compat)
        ctx->min_padding = 1600;

    if (dnxhd_init_vlc(ctx) < 0)
        return -1;
    if (dnxhd_init_rc(ctx) < 0)
        return -1;

    FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->slice_size, ctx->mb_height*sizeof(uint32_t), fail);
    FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->slice_offs, ctx->mb_height*sizeof(uint32_t), fail);
    FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->mb_bits,    ctx->mb_num   *sizeof(uint16_t), fail);
    FF_ALLOCZ_OR_GOTO(ctx->avctx, ctx->mb_qscale,  ctx->mb_num   *sizeof(uint8_t),  fail);

    ctx->frame.key_frame = 1;
    ctx->frame.pict_type = AV_PICTURE_TYPE_I;
    ctx->avctx->coded_frame = &ctx->frame;

    if (avctx->thread_count <= 0) {
        av_log(avctx, AV_LOG_ERROR, "error, invalid thread count\n");
        return -1;
    }

    ctx->thread = av_malloc(avctx->thread_count*sizeof(*ctx->thread));
    if (!ctx->thread) {
        av_log(avctx, AV_LOG_ERROR, "could not allocate thread contexts\n");
        return -1;
    }

    ctx->thread[0] = ctx;
    for (i = 1; i < avctx->thread_count; i++) {
        ctx->thread[i] =  av_malloc(sizeof(DNXHDEncContext));
        memcpy(ctx->thread[i], ctx, sizeof(DNXHDEncContext));
    }

    return 0;
 fail: //for FF_ALLOCZ_OR_GOTO
    return -1;
}

static int dnxhd_write_header(AVCodecContext *avctx, uint8_t *buf)
{
    DNXHDEncContext *ctx = avctx->priv_data;
    const uint8_t header_prefix[5] = { 0x00,0x00,0x02,0x80,0x01 };

    memset(buf, 0, 640);

    memcpy(buf, header_prefix, 5);
    buf[5] = ctx->interlaced ? ctx->cur_field+2 : 0x01;
    buf[6] = 0x80; // crc flag off
    buf[7] = 0xa0; // reserved
    AV_WB16(buf + 0x18, avctx->height>>ctx->interlaced); // ALPF
    AV_WB16(buf + 0x1a, avctx->width);  // SPL
    AV_WB16(buf + 0x1d, avctx->height>>ctx->interlaced); // NAL

    buf[0x21] = ctx->cid_table->bit_depth == 10 ? 0x58 : 0x38;
    buf[0x22] = 0x88 + (ctx->interlaced<<2);
    AV_WB32(buf + 0x28, ctx->cid); // CID
    buf[0x2c] = ctx->interlaced ? 0 : 0x80;

    buf[0x5f] = 0x01; // UDL

    buf[0x167] = 0x02; // reserved
    AV_WB16(buf + 0x16a, ctx->mb_height * 4 + 4); // MSIPS
    buf[0x16d] = ctx->mb_height; // Ns
    buf[0x16f] = 0x10; // reserved

    ctx->msip = buf + 0x170;
    return 0;
}

static av_always_inline void dnxhd_encode_dc(DNXHDEncContext *ctx, int diff)
{
    int nbits;
    if (diff < 0) {
        nbits = av_log2_16bit(-2*diff);
        diff--;
    } else {
        nbits = av_log2_16bit(2*diff);
    }
    put_bits(&ctx->pb, ctx->cid_table->dc_bits[nbits] + nbits,
             (ctx->cid_table->dc_codes[nbits]<<nbits) + (diff & ((1 << nbits) - 1)));
}

static av_always_inline void dnxhd_encode_block(DNXHDEncContext *ctx, DCTELEM *block, int last_index, int n)
{
    int last_non_zero = 0;
    int slevel, i, j;

    dnxhd_encode_dc(ctx, block[0] - ctx->last_dc[n]);
    ctx->last_dc[n] = block[0];

    for (i = 1; i <= last_index; i++) {
        j = ctx->scantable.permutated[i];
        slevel = block[j];
        if (slevel) {
            int run_level = i - last_non_zero - 1;
            int rlevel = (slevel<<1)|!!run_level;
            put_bits(&ctx->pb, ctx->vlc_bits[rlevel], ctx->vlc_codes[rlevel]);
            if (run_level)
                put_bits(&ctx->pb, ctx->run_bits[run_level], ctx->run_codes[run_level]);
            last_non_zero = i;
        }
    }
    put_bits(&ctx->pb, ctx->vlc_bits[0], ctx->vlc_codes[0]); // EOB
}

static av_always_inline void dnxhd_unquantize_c(DNXHDEncContext *ctx, DCTELEM *block, int n, int qscale, int last_index)
{
    const uint8_t *weight_matrix;
    int level;
    int i;

    weight_matrix = (n&2) ? ctx->cid_table->chroma_weight : ctx->cid_table->luma_weight;

    for (i = 1; i <= last_index; i++) {
        int j = ctx->scantable.permutated[i];
        level = block[j];
        if (level) {
            if (level < 0) {
                level = (1-2*level) * qscale * weight_matrix[j];
                if (ctx->cid_table->bit_depth == 10) {
                    if (weight_matrix[j] != 8)
                        level += 8;
                    level >>= 4;
                } else {
                    if (weight_matrix[j] != 32)
                        level += 32;
                    level >>= 6;
                }
                level = -level;
            } else {
                level = (2*level+1) * qscale * weight_matrix[j];
                if (ctx->cid_table->bit_depth == 10) {
                    if (weight_matrix[j] != 8)
                        level += 8;
                    level >>= 4;
                } else {
                    if (weight_matrix[j] != 32)
                        level += 32;
                    level >>= 6;
                }
            }
            block[j] = level;
        }
    }
}

static av_always_inline int dnxhd_ssd_block(DCTELEM *qblock, DCTELEM *block)
{
    int score = 0;
    int i;
    for (i = 0; i < 64; i++)
        score += (block[i] - qblock[i]) * (block[i] - qblock[i]);
    return score;
}

static av_always_inline int dnxhd_calc_ac_bits(DNXHDEncContext *ctx, DCTELEM *block, int last_index)
{
    int last_non_zero = 0;
    int bits = 0;
    int i, j, level;
    for (i = 1; i <= last_index; i++) {
        j = ctx->scantable.permutated[i];
        level = block[j];
        if (level) {
            int run_level = i - last_non_zero - 1;
            bits += ctx->vlc_bits[(level<<1)|!!run_level]+ctx->run_bits[run_level];
            last_non_zero = i;
        }
    }
    return bits;
}

static av_always_inline void dnxhd_get_blocks(DNXHDEncContext *ctx, int mb_x, int mb_y)
{
    const int bs = ctx->block_width_l2;
    const int bw = 1 << bs;
    const uint8_t *ptr_y = ctx->thread[0]->src[0] + ((mb_y << 4) * ctx->linesize)   + (mb_x << bs+1);
    const uint8_t *ptr_u = ctx->thread[0]->src[1] + ((mb_y << 4) * ctx->uvlinesize) + (mb_x << bs);
    const uint8_t *ptr_v = ctx->thread[0]->src[2] + ((mb_y << 4) * ctx->uvlinesize) + (mb_x << bs);
    DSPContext *dsp = &ctx->dsp;

    dsp->get_pixels(ctx->blocks[0], ptr_y,      ctx->linesize);
    dsp->get_pixels(ctx->blocks[1], ptr_y + bw, ctx->linesize);
    dsp->get_pixels(ctx->blocks[2], ptr_u,      ctx->uvlinesize);
    dsp->get_pixels(ctx->blocks[3], ptr_v,      ctx->uvlinesize);

    if (mb_y+1 == ctx->mb_height && ctx->avctx->height == 1080) {
        if (ctx->interlaced) {
            ctx->get_pixels_8x4_sym(ctx->blocks[4], ptr_y + ctx->dct_y_offset,      ctx->linesize);
            ctx->get_pixels_8x4_sym(ctx->blocks[5], ptr_y + ctx->dct_y_offset + bw, ctx->linesize);
            ctx->get_pixels_8x4_sym(ctx->blocks[6], ptr_u + ctx->dct_uv_offset,     ctx->uvlinesize);
            ctx->get_pixels_8x4_sym(ctx->blocks[7], ptr_v + ctx->dct_uv_offset,     ctx->uvlinesize);
        } else {
            dsp->clear_block(ctx->blocks[4]);
            dsp->clear_block(ctx->blocks[5]);
            dsp->clear_block(ctx->blocks[6]);
            dsp->clear_block(ctx->blocks[7]);
        }
    } else {
        dsp->get_pixels(ctx->blocks[4], ptr_y + ctx->dct_y_offset,      ctx->linesize);
        dsp->get_pixels(ctx->blocks[5], ptr_y + ctx->dct_y_offset + bw, ctx->linesize);
        dsp->get_pixels(ctx->blocks[6], ptr_u + ctx->dct_uv_offset,     ctx->uvlinesize);
        dsp->get_pixels(ctx->blocks[7], ptr_v + ctx->dct_uv_offset,     ctx->uvlinesize);
    }
}

static av_always_inline int dnxhd_switch_matrix(DNXHDEncContext *ctx, int i)
{
    if (i&2) {
        ctx->cur_qmatrix16 = ctx->qmatrix_c16;
        ctx->cur_qmatrix   = ctx->qmatrix_c;
        return 1 + (i&1);
    } else {
        ctx->cur_qmatrix16 = ctx->qmatrix_l16;
        ctx->cur_qmatrix   = ctx->qmatrix_l;
        return 0;
    }
}

static int dnxhd_calc_bits_thread(AVCodecContext *avctx, void *arg, int jobnr, int threadnr)
{
    DNXHDEncContext *ctx = avctx->priv_data;
    int mb_y = jobnr, mb_x;
    int qscale = ctx->qscale;
    LOCAL_ALIGNED_16(DCTELEM, block, [64]);
    ctx = ctx->thread[threadnr];

    ctx->last_dc[0] =
    ctx->last_dc[1] =
    ctx->last_dc[2] = 1 << (ctx->cid_table->bit_depth + 2);

    for (mb_x = 0; mb_x < ctx->mb_width; mb_x++) {
        unsigned mb = mb_y * ctx->mb_width + mb_x;
        int ssd     = 0;
        int ac_bits = 0;
        int dc_bits = 0;
        int i;

        dnxhd_get_blocks(ctx, mb_x, mb_y);

        for (i = 0; i < 8; i++) {
            DCTELEM *src_block = ctx->blocks[i];
            int nbits, diff, last_index;
            int n = dnxhd_switch_matrix(ctx, i);

            memcpy(block, src_block, 64*sizeof(*block));
            last_index = ctx->dct_quantize(ctx, block, qscale);
            ac_bits += dnxhd_calc_ac_bits(ctx, block, last_index);

            diff = block[0] - ctx->last_dc[n];
            if (diff < 0) nbits = av_log2_16bit(-2*diff);
            else          nbits = av_log2_16bit( 2*diff);

            assert(nbits < ctx->cid_table->bit_depth + 4);
            dc_bits += ctx->cid_table->dc_bits[nbits] + nbits;

            ctx->last_dc[n] = block[0];

            if (avctx->mb_decision == FF_MB_DECISION_RD || !RC_VARIANCE) {
                dnxhd_unquantize_c(ctx, block, i, qscale, last_index);
                ctx->dsp.idct(block);
                ssd += dnxhd_ssd_block(block, src_block);
            }
        }
        ctx->mb_rc[qscale][mb].ssd = ssd;
        ctx->mb_rc[qscale][mb].bits = ac_bits+dc_bits+12+8*ctx->vlc_bits[0];
    }
    return 0;
}

static int dnxhd_encode_thread(AVCodecContext *avctx, void *arg, int jobnr, int threadnr)
{
    DNXHDEncContext *ctx = avctx->priv_data;
    int mb_y = jobnr, mb_x;
    ctx = ctx->thread[threadnr];
    init_put_bits(&ctx->pb, (uint8_t *)arg + 640 + ctx->slice_offs[jobnr], ctx->slice_size[jobnr]);

    ctx->last_dc[0] =
    ctx->last_dc[1] =
    ctx->last_dc[2] = 1 << (ctx->cid_table->bit_depth + 2);
    for (mb_x = 0; mb_x < ctx->mb_width; mb_x++) {
        unsigned mb = mb_y * ctx->mb_width + mb_x;
        int qscale = ctx->mb_qscale[mb];
        int i;

        put_bits(&ctx->pb, 12, qscale<<1);

        dnxhd_get_blocks(ctx, mb_x, mb_y);

        for (i = 0; i < 8; i++) {
            DCTELEM *block = ctx->blocks[i];
            int last_index;
            int n = dnxhd_switch_matrix(ctx, i);
            last_index = ctx->dct_quantize(ctx, block, qscale);
            //START_TIMER;
            dnxhd_encode_block(ctx, block, last_index, n);
            //STOP_TIMER("encode_block");
        }
    }
    if (put_bits_count(&ctx->pb)&31)
        put_bits(&ctx->pb, 32-(put_bits_count(&ctx->pb)&31), 0);
    flush_put_bits(&ctx->pb);
    return 0;
}

static void dnxhd_setup_threads_slices(DNXHDEncContext *ctx)
{
    int mb_y, mb_x;
    int offset = 0;
    for (mb_y = 0; mb_y < ctx->mb_height; mb_y++) {
        int thread_size;
        ctx->slice_offs[mb_y] = offset;
        ctx->slice_size[mb_y] = 0;
        for (mb_x = 0; mb_x < ctx->mb_width; mb_x++) {
            unsigned mb = mb_y * ctx->mb_width + mb_x;
            ctx->slice_size[mb_y] += ctx->mb_bits[mb];
        }
        ctx->slice_size[mb_y] = (ctx->slice_size[mb_y]+31)&~31;
        ctx->slice_size[mb_y] >>= 3;
        thread_size = ctx->slice_size[mb_y];
        offset += thread_size;
    }
}

static int dnxhd_mb_var_thread(AVCodecContext *avctx, void *arg, int jobnr, int threadnr)
{
    DNXHDEncContext *ctx = avctx->priv_data;
    int mb_y = jobnr, mb_x;
    ctx = ctx->thread[threadnr];
    if (ctx->cid_table->bit_depth == 8) {
        uint8_t *pix = ctx->thread[0]->src[0] + (mb_y<<4) * ctx->linesize;
        for (mb_x = 0; mb_x < ctx->mb_width; ++mb_x, pix += 16) {
            unsigned mb = mb_y * ctx->mb_width + mb_x;
            int sum = ctx->dsp.pix_sum(pix, ctx->linesize);
            int varc = (ctx->dsp.pix_norm1(pix, ctx->linesize) -
                        (((unsigned)(sum*sum+128))>>8)+128)>>8;
            ctx->mb_cmp[mb].value = varc;
            ctx->mb_cmp[mb].mb = mb;
        }
    } else { // 10-bit
        for (mb_x = 0; mb_x < ctx->mb_width; ++mb_x) {
            uint8_t *pix = ctx->thread[0]->src[0] + (mb_y<<4) * ctx->linesize + (mb_x << 5);
            unsigned mb = mb_y * ctx->mb_width + mb_x;
            int sum = 0, sqsum = 0;
            for (int i = 0; i < 16; ++i) {
                for (int j = 0; j < 16; ++j) {
                    const int sample = ((uint16_t*)pix)[j];
                    sum += sample;
                    sqsum += sample * sample;
                }
                pix += ctx->linesize;
            }
            ctx->mb_cmp[mb].value = (sqsum - (((uint64_t)sum*sum+128)>>8)+128)>>8;
            ctx->mb_cmp[mb].mb = mb;
        }
    }
    return 0;
}

static int dnxhd_encode_rdo(AVCodecContext *avctx, DNXHDEncContext *ctx)
{
    int lambda, up_step, down_step;
    int last_lower = INT_MAX, last_higher = 0;
    int x, y, q;

    for (q = 1; q <= ctx->qmax; q++) {
        ctx->qscale = q;
        avctx->execute2(avctx, dnxhd_calc_bits_thread, NULL, NULL, ctx->mb_height);
    }
    up_step = down_step = 2<<LAMBDA_FRAC_BITS;
    lambda = ctx->lambda;

    for (;;) {
        int bits = 0;
        int end = 0;
        if (lambda == last_higher) {
            lambda++;
            end = 1; // need to set final qscales/bits
        }
        for (y = 0; y < ctx->mb_height; y++) {
            for (x = 0; x < ctx->mb_width; x++) {
                unsigned min = UINT_MAX;
                int qscale = 1;
                int mb = y*ctx->mb_width+x;
                for (q = 1; q <= ctx->qmax; q++) {
                    unsigned score = ctx->mb_rc[q][mb].bits*lambda+(ctx->mb_rc[q][mb].ssd<<LAMBDA_FRAC_BITS);
                    if (score < min) {
                        min = score;
                        qscale = q;
                    }
                }
                bits += ctx->mb_rc[qscale][mb].bits;
                ctx->mb_qscale[mb] = qscale;
                ctx->mb_bits[mb] = ctx->mb_rc[qscale][mb].bits;
            }
            bits = (bits+31)&~31; // padding
            if (bits > ctx->frame_bits)
                break;
        }
        //av_dlog(ctx->avctx, "lambda %d, up %u, down %u, bits %d, frame %d\n",
        //        lambda, last_higher, last_lower, bits, ctx->frame_bits);
        if (end) {
            if (bits > ctx->frame_bits)
                return -1;
            break;
        }
        if (bits < ctx->frame_bits) {
            last_lower = FFMIN(lambda, last_lower);
            if (last_higher != 0)
                lambda = (lambda+last_higher)>>1;
            else
                lambda -= down_step;
            down_step *= 5; // XXX tune ?
            up_step = 1<<LAMBDA_FRAC_BITS;
            lambda = FFMAX(1, lambda);
            if (lambda == last_lower)
                break;
        } else {
            last_higher = FFMAX(lambda, last_higher);
            if (last_lower != INT_MAX)
                lambda = (lambda+last_lower)>>1;
            else if ((int64_t)lambda + up_step > INT_MAX)
                return -1;
            else
                lambda += up_step;
            up_step = FFMIN((int64_t)up_step*5, INT_MAX);
            down_step = 1<<LAMBDA_FRAC_BITS;
        }
    }
    //av_dlog(ctx->avctx, "out lambda %d\n", lambda);
    ctx->lambda = lambda;
    return 0;
}

static int dnxhd_find_qscale(DNXHDEncContext *ctx)
{
    int bits = 0;
    int up_step = 1;
    int down_step = 1;
    int last_higher = 0;
    int last_lower = INT_MAX;
    int qscale;
    int x, y;

    qscale = ctx->qscale;
    for (;;) {
        bits = 0;
        ctx->qscale = qscale;
        // XXX avoid recalculating bits
        ctx->avctx->execute2(ctx->avctx, dnxhd_calc_bits_thread, NULL, NULL, ctx->mb_height);
        for (y = 0; y < ctx->mb_height; y++) {
            for (x = 0; x < ctx->mb_width; x++)
                bits += ctx->mb_rc[qscale][y*ctx->mb_width+x].bits;
            bits = (bits+31)&~31; // padding
            if (bits > ctx->frame_bits)
                break;
        }
        //av_dlog(ctx->avctx, "%d, qscale %d, bits %d, frame %d, higher %d, lower %d\n",
        //        ctx->avctx->frame_number, qscale, bits, ctx->frame_bits, last_higher, last_lower);
        if (bits < ctx->frame_bits) {
            if (qscale == 1)
                return 1;
            if (last_higher == qscale - 1) {
                qscale = last_higher;
                break;
            }
            last_lower = FFMIN(qscale, last_lower);
            if (last_higher != 0)
                qscale = (qscale+last_higher)>>1;
            else
                qscale -= down_step++;
            if (qscale < 1)
                qscale = 1;
            up_step = 1;
        } else {
            if (last_lower == qscale + 1)
                break;
            last_higher = FFMAX(qscale, last_higher);
            if (last_lower != INT_MAX)
                qscale = (qscale+last_lower)>>1;
            else
                qscale += up_step++;
            down_step = 1;
            if (qscale > ctx->qmax)
                return -1;
        }
    }
    //av_dlog(ctx->avctx, "out qscale %d\n", qscale);
    ctx->qscale = qscale;
    return 0;
}

#define BUCKET_BITS 8
#define RADIX_PASSES 4
#define NBUCKETS (1 << BUCKET_BITS)

static inline int get_bucket(int value, int shift)
{
    value >>= shift;
    value &= NBUCKETS - 1;
    return NBUCKETS - 1 - value;
}

static void radix_count(const RCCMPEntry *data, int size, int buckets[RADIX_PASSES][NBUCKETS])
{
    int i, j;
    memset(buckets, 0, sizeof(buckets[0][0]) * RADIX_PASSES * NBUCKETS);
    for (i = 0; i < size; i++) {
        int v = data[i].value;
        for (j = 0; j < RADIX_PASSES; j++) {
            buckets[j][get_bucket(v, 0)]++;
            v >>= BUCKET_BITS;
        }
        assert(!v);
    }
    for (j = 0; j < RADIX_PASSES; j++) {
        int offset = size;
        for (i = NBUCKETS - 1; i >= 0; i--)
            buckets[j][i] = offset -= buckets[j][i];
        assert(!buckets[j][0]);
    }
}

static void radix_sort_pass(RCCMPEntry *dst, const RCCMPEntry *data, int size, int buckets[NBUCKETS], int pass)
{
    int shift = pass * BUCKET_BITS;
    int i;
    for (i = 0; i < size; i++) {
        int v = get_bucket(data[i].value, shift);
        int pos = buckets[v]++;
        dst[pos] = data[i];
    }
}

static void radix_sort(RCCMPEntry *data, int size)
{
    int buckets[RADIX_PASSES][NBUCKETS];
    RCCMPEntry *tmp = av_malloc(sizeof(*tmp) * size);
    radix_count(data, size, buckets);
    radix_sort_pass(tmp, data, size, buckets[0], 0);
    radix_sort_pass(data, tmp, size, buckets[1], 1);
    if (buckets[2][NBUCKETS - 1] || buckets[3][NBUCKETS - 1]) {
        radix_sort_pass(tmp, data, size, buckets[2], 2);
        radix_sort_pass(data, tmp, size, buckets[3], 3);
    }
    av_free(tmp);
}

static int dnxhd_encode_fast(AVCodecContext *avctx, DNXHDEncContext *ctx, int alpha)
{
    int max_bits = 0;
    int ret, x, y;
    if ((ret = dnxhd_find_qscale(ctx)) < 0)
        return -1;
    for (y = 0; y < ctx->mb_height; y++) {
        for (x = 0; x < ctx->mb_width; x++) {
            int mb = y*ctx->mb_width+x;
            int delta_bits;
            ctx->mb_qscale[mb] = ctx->qscale;
            ctx->mb_bits[mb] = ctx->mb_rc[ctx->qscale][mb].bits;
            max_bits += ctx->mb_rc[ctx->qscale][mb].bits;
            if (!RC_VARIANCE) {
                delta_bits = ctx->mb_rc[ctx->qscale][mb].bits-ctx->mb_rc[ctx->qscale+1][mb].bits;
                ctx->mb_cmp[mb].mb = mb;
                ctx->mb_cmp[mb].value = delta_bits ?
                    ((ctx->mb_rc[ctx->qscale][mb].ssd-ctx->mb_rc[ctx->qscale+1][mb].ssd)*100)/delta_bits
                    : INT_MIN; //avoid increasing qscale
            }
        }
        max_bits += 31; //worst padding
    }
    if (!ret) {
        if (RC_VARIANCE)
            avctx->execute2(avctx, dnxhd_mb_var_thread, NULL, NULL, alpha>0?ctx->mb_height-1:ctx->mb_height);
        radix_sort(ctx->mb_cmp, ctx->mb_num);
        for (x = 0; x < ctx->mb_num && max_bits > ctx->frame_bits; x++) {
            int mb = ctx->mb_cmp[x].mb;
            max_bits -= ctx->mb_rc[ctx->qscale][mb].bits - ctx->mb_rc[ctx->qscale+1][mb].bits;
            ctx->mb_qscale[mb] = ctx->qscale+1;
            ctx->mb_bits[mb] = ctx->mb_rc[ctx->qscale+1][mb].bits;
        }
    }
    return 0;
}

static void dnxhd_load_picture(DNXHDEncContext *ctx, const AVFrame *frame)
{
    int i;

    for (i = 0; i < 3; i++) {
        ctx->frame.data[i]     = frame->data[i];
        ctx->frame.linesize[i] = frame->linesize[i];
    }

    if(frame->data[3] != NULL)
    {
        ctx->frame.data[3]     = frame->data[3];
        ctx->frame.linesize[3] = frame->linesize[3];
    }

    for (i = 0; i < ctx->avctx->thread_count; i++) {
        ctx->thread[i]->linesize    = ctx->frame.linesize[0]<<ctx->interlaced;
        ctx->thread[i]->uvlinesize  = ctx->frame.linesize[1]<<ctx->interlaced;
        ctx->thread[i]->dct_y_offset  = ctx->linesize  *8;
        ctx->thread[i]->dct_uv_offset = ctx->uvlinesize*8;
    }

    ctx->frame.interlaced_frame = frame->interlaced_frame;
    ctx->cur_field = frame->interlaced_frame && !frame->top_field_first;
}

static int dnxhd_encode_picture(AVCodecContext *avctx, unsigned char *buf, int buf_size, void *data)
{
    DNXHDEncContext *ctx = avctx->priv_data;
    int first_field = 1;
    int offset, i, ret;
    int alphaPresent = 0;

    if (buf_size < ctx->cid_table->frame_size) {
        av_log(avctx, AV_LOG_ERROR, "output buffer is too small to compress picture\n");
        return -1;
    }

    dnxhd_load_picture(ctx, data);

 encode_coding_unit:
    if(alphaPresent)
    {
        ctx->src[0] = ctx->frame.data[3];
        memset(ctx->src[1], 128, ctx->frame.linesize[1]*avctx->height);
        memset(ctx->src[2], 128, ctx->frame.linesize[2]*avctx->height);
        if (ctx->interlaced && ctx->cur_field)
        {
            ctx->src[0] += ctx->frame.linesize[3];
            ctx->src[1] += ctx->frame.linesize[1];
            ctx->src[2] += ctx->frame.linesize[2];
        }
    }
    else
    for (i = 0; i < 3; i++) {
        ctx->src[i] = ctx->frame.data[i];
        if (ctx->interlaced && ctx->cur_field)
            ctx->src[i] += ctx->frame.linesize[i];
    }

    dnxhd_write_header(avctx, buf);

    if (avctx->mb_decision == FF_MB_DECISION_RD)
        ret = dnxhd_encode_rdo(avctx, ctx);
    else
        ret = dnxhd_encode_fast(avctx, ctx, alphaPresent);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "picture could not fit ratecontrol constraints, increase qmax\n");
        return -1;
    }

    dnxhd_setup_threads_slices(ctx);

    offset = 0;
    for (i = 0; i < ctx->mb_height; i++) {
        AV_WB32(ctx->msip + i * 4, offset);
        offset += ctx->slice_size[i];
        assert(!(ctx->slice_size[i] & 3));
    }

    avctx->execute2(avctx, dnxhd_encode_thread, buf, NULL, ctx->mb_height);

    assert(640 + offset + 4 <= ctx->cid_table->coding_unit_size);
    memset(buf + 640 + offset, 0, ctx->cid_table->coding_unit_size - 4 - offset - 640);

    AV_WB32(buf + ctx->cid_table->coding_unit_size - 4, 0x600DC0DE); // EOF

    if (ctx->interlaced && first_field) {
        first_field     = 0;
        ctx->cur_field ^= 1;
        buf      += ctx->cid_table->coding_unit_size;
        buf_size -= ctx->cid_table->coding_unit_size;
        goto encode_coding_unit;
    }

    if(!alphaPresent && ctx->frame.data[3] != NULL && ctx->frame.linesize[3] != 0)
    {
        alphaPresent = 1;
        first_field     = 1;
        ctx->cur_field = 0;
        buf      += ctx->cid_table->coding_unit_size;
        buf_size -= ctx->cid_table->coding_unit_size;
        goto encode_coding_unit;
    }

    ctx->frame.quality = ctx->qscale*FF_QP2LAMBDA;

    if(alphaPresent)
    {
        AV_WB32(buf + ctx->cid_table->coding_unit_size, 0x00094000); // EOF WITH ALPHA VALUES
        return (2*ctx->cid_table->frame_size+4);
    }
    else
        return ctx->cid_table->frame_size;
}

static int dnxhd_encode_end(AVCodecContext *avctx)
{
    DNXHDEncContext *ctx = avctx->priv_data;
    int max_level = 1<<(ctx->cid_table->bit_depth+2);
    int i;

    av_free(ctx->vlc_codes-max_level*2);
    av_free(ctx->vlc_bits -max_level*2);
    av_freep(&ctx->run_codes);
    av_freep(&ctx->run_bits);

    av_freep(&ctx->mb_bits);
    av_freep(&ctx->mb_qscale);
    av_freep(&ctx->mb_rc);
    av_freep(&ctx->mb_cmp);
    av_freep(&ctx->slice_size);
    av_freep(&ctx->slice_offs);

    av_freep(&ctx->qmatrix_c);
    av_freep(&ctx->qmatrix_l);
    av_freep(&ctx->qmatrix_c16);
    av_freep(&ctx->qmatrix_l16);

    for (i = 1; i < avctx->thread_count; i++)
        av_freep(&ctx->thread[i]);

    return 0;
}

AVCodec ff_dnxhd_encoder = {
    "dnxhd",
    AVMEDIA_TYPE_VIDEO,
    CODEC_ID_DNXHD,
    sizeof(DNXHDEncContext),
    dnxhd_encode_init,
    dnxhd_encode_picture,
    dnxhd_encode_end,
    .capabilities = CODEC_CAP_SLICE_THREADS,
    .pix_fmts = (const enum PixelFormat[]){PIX_FMT_YUV422P, PIX_FMT_YUVA422P, PIX_FMT_YUV422P10, PIX_FMT_NONE},
    .long_name = NULL_IF_CONFIG_SMALL("VC3/DNxHD"),
    .priv_class = &class,
};
