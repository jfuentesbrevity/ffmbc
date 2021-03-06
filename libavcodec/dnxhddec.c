/*
 * VC3/DNxHD decoder.
 * Copyright (c) 2007 SmartJog S.A., Baptiste Coudurier <baptiste dot coudurier at smartjog dot com>
 * Copyright (c) 2011 MirriAd Ltd
 *
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

//#define TRACE
//#define DEBUG

#include "libavutil/imgutils.h"
#include "avcodec.h"
#include "get_bits.h"
#include "dnxhddata.h"
#include "dsputil.h"
#include "thread.h"

typedef struct DNXHDContext {
    AVCodecContext *avctx;
    AVFrame picture;
    GetBitContext gb;
    int cid;                            ///< compression id
    unsigned int width, height;
    unsigned int mb_width, mb_height;
    uint32_t mb_scan_index[68];         /* max for 1080p */
    int cur_field;                      ///< current interlaced field
    VLC ac_vlc, dc_vlc, run_vlc;
    int last_dc[3];
    DSPContext dsp;
    DECLARE_ALIGNED(16, DCTELEM, blocks)[8][64];
    uint8_t scan[64];
    const CIDEntry *cid_table;
    void (*decode_dct_block)(struct DNXHDContext *ctx, DCTELEM *block,
                             int n, int qscale);
    int last_qscale;
    int luma_scale[64];
    int chroma_scale[64];
} DNXHDContext;

#define DNXHD_VLC_BITS 9
#define DNXHD_DC_VLC_BITS 7

static void dnxhd_decode_dct_block_8(DNXHDContext *ctx, DCTELEM *block, int n, int qscale);
static void dnxhd_decode_dct_block_10(DNXHDContext *ctx, DCTELEM *block, int n, int qscale);

static void permute(uint8_t *dst, const uint8_t *src, const uint8_t permutation[64])
{
    int i;
    for (i = 0; i < 64; i++)
        dst[i] = permutation[src[i]];
}

static av_cold int dnxhd_decode_init(AVCodecContext *avctx)
{
    DNXHDContext *ctx = avctx->priv_data;

    ctx->avctx = avctx;
    avctx->coded_frame = &ctx->picture;
    avcodec_get_frame_defaults(&ctx->picture);
    ctx->picture.type = AV_PICTURE_TYPE_I;
    ctx->picture.key_frame = 1;
    return 0;
}

static int dnxhd_init_vlc(DNXHDContext *ctx, int cid)
{
    if (cid != ctx->cid) {
        int index;

        if ((index = ff_dnxhd_get_cid_table(cid)) < 0) {
            av_log(ctx->avctx, AV_LOG_ERROR, "unsupported cid %d\n", cid);
            return -1;
        }
        ctx->cid_table = &ff_dnxhd_cid_table[index];

        free_vlc(&ctx->ac_vlc);
        free_vlc(&ctx->dc_vlc);
        free_vlc(&ctx->run_vlc);

        init_vlc(&ctx->ac_vlc, DNXHD_VLC_BITS, 257,
                 ctx->cid_table->ac_bits, 1, 1,
                 ctx->cid_table->ac_codes, 2, 2, 0);
        init_vlc(&ctx->dc_vlc, DNXHD_DC_VLC_BITS, ctx->cid_table->bit_depth + 4,
                 ctx->cid_table->dc_bits, 1, 1,
                 ctx->cid_table->dc_codes, 1, 1, 0);
        init_vlc(&ctx->run_vlc, DNXHD_VLC_BITS, 62,
                 ctx->cid_table->run_bits, 1, 1,
                 ctx->cid_table->run_codes, 2, 2, 0);

        ctx->cid = cid;
    }
    return 0;
}

static int dnxhd_decode_header(DNXHDContext *ctx, const uint8_t *buf, int buf_size, int first_field)
{
    static const uint8_t header_prefix[] = { 0x00, 0x00, 0x02, 0x80, 0x01 };
    int i, cid;

    if (buf_size < 0x280)
        return -1;

    if (memcmp(buf, header_prefix, 5)) {
        av_log(ctx->avctx, AV_LOG_ERROR, "error in header\n");
        return -1;
    }
    if (buf[5] & 2) { /* interlaced */
        ctx->cur_field = buf[5] & 1;
        ctx->picture.interlaced_frame = 1;
        ctx->picture.top_field_first = first_field ^ ctx->cur_field;
        av_log(ctx->avctx, AV_LOG_DEBUG, "interlaced %d, cur field %d\n", buf[5] & 3, ctx->cur_field);
    }

    ctx->height = AV_RB16(buf + 0x18);
    ctx->width  = AV_RB16(buf + 0x1a);

    av_dlog(ctx->avctx, "width %d, heigth %d\n", ctx->width, ctx->height);

    if (buf[0x21] & 0x40) {
        ctx->avctx->pix_fmt = PIX_FMT_YUV422P10;
        ctx->avctx->bits_per_raw_sample = 10;
        ctx->decode_dct_block = dnxhd_decode_dct_block_10;
    } else {
        ctx->avctx->pix_fmt = PIX_FMT_YUV422P;
        ctx->avctx->bits_per_raw_sample = 8;
        ctx->decode_dct_block = dnxhd_decode_dct_block_8;
    }

    dsputil_init(&ctx->dsp, ctx->avctx);

    permute(ctx->scan, ff_zigzag_direct, ctx->dsp.idct_permutation);

    cid = AV_RB32(buf + 0x28);
    av_dlog(ctx->avctx, "compression id %d\n", cid);

    if (dnxhd_init_vlc(ctx, cid) < 0)
        return -1;

    if (buf_size < ctx->cid_table->coding_unit_size) {
        av_log(ctx->avctx, AV_LOG_ERROR, "incorrect frame size\n");
        return -1;
    }

    ctx->mb_width = ctx->width>>4;
    ctx->mb_height = buf[0x16d];

    av_dlog(ctx->avctx, "mb width %d, mb height %d\n", ctx->mb_width, ctx->mb_height);

    if ((ctx->height+15)>>4 == ctx->mb_height && ctx->picture.interlaced_frame)
        ctx->height <<= 1;

    if (ctx->mb_height > 68 ||
        (ctx->mb_height<<ctx->picture.interlaced_frame) > (ctx->height+15)>>4) {
        av_log(ctx->avctx, AV_LOG_ERROR, "mb height too big: %d\n", ctx->mb_height);
        return -1;
    }

    for (i = 0; i < ctx->mb_height; i++) {
        ctx->mb_scan_index[i] = AV_RB32(buf + 0x170 + (i<<2));
        av_dlog(ctx->avctx, "mb scan index %d\n", ctx->mb_scan_index[i]);
        if (buf_size < ctx->mb_scan_index[i] + 0x280) {
            av_log(ctx->avctx, AV_LOG_ERROR, "invalid mb scan index\n");
            return -1;
        }
    }

    return 0;
}

static av_always_inline void dnxhd_decode_dct_block(DNXHDContext *ctx,
                                                    DCTELEM *block, int n,
                                                    int qscale,
                                                    int index_bits,
                                                    int level_bias,
                                                    int level_shift)
{
    int i, j, index1, index2, len, flags;
    int level, component, sign;
    const int *scale;
    const uint8_t *weight_matrix;
    const uint8_t *ac_level = ctx->cid_table->ac_level;
    const uint8_t *ac_flags = ctx->cid_table->ac_flags;
    const int eob_index     = ctx->cid_table->eob_index;
    OPEN_READER(bs, &ctx->gb);

    if (n&2) {
        component = 1 + (n&1);
        scale = ctx->chroma_scale;
        weight_matrix = ctx->cid_table->chroma_weight;
    } else {
        component = 0;
        scale = ctx->luma_scale;
        weight_matrix = ctx->cid_table->luma_weight;
    }

    UPDATE_CACHE(bs, &ctx->gb);
    GET_VLC(len, bs, &ctx->gb, ctx->dc_vlc.table, DNXHD_DC_VLC_BITS, 1);
    if (len) {
        level = GET_CACHE(bs, &ctx->gb);
        LAST_SKIP_BITS(bs, &ctx->gb, len);
        sign  = ~level >> 31;
        level = (NEG_USR32(sign ^ level, len) ^ sign) - sign;
        ctx->last_dc[component] += level;
    }
    block[0] = ctx->last_dc[component];
    //av_log(ctx->avctx, AV_LOG_DEBUG, "dc %d\n", block[0]);

    i = 0;

    UPDATE_CACHE(bs, &ctx->gb);
    GET_VLC(index1, bs, &ctx->gb, ctx->ac_vlc.table,
            DNXHD_VLC_BITS, 2);

    while (index1 != eob_index) {
        level = ac_level[index1];
        flags = ac_flags[index1];

        sign = SHOW_SBITS(bs, &ctx->gb, 1);
        SKIP_BITS(bs, &ctx->gb, 1);

        if (flags & 1) {
            level += SHOW_UBITS(bs, &ctx->gb, index_bits) << 7;
            SKIP_BITS(bs, &ctx->gb, index_bits);
        }

        if (flags & 2) {
            UPDATE_CACHE(bs, &ctx->gb);
            GET_VLC(index2, bs, &ctx->gb, ctx->run_vlc.table,
                    DNXHD_VLC_BITS, 2);
            i += ctx->cid_table->run[index2];
        }

        if (++i > 63) {
            av_log(ctx->avctx, AV_LOG_ERROR, "ac tex damaged %d, %d\n", n, i);
            break;
        }

        j = ff_zigzag_direct[i];
        //av_log(ctx->avctx, AV_LOG_DEBUG, "j %d\n", j);
        //av_log(ctx->avctx, AV_LOG_DEBUG, "level %d, weight %d\n", level, weight_matrix[i]);
        level *= scale[j];
        if (level_bias < 32 || weight_matrix[j] != level_bias)
            level += level_bias;
        level >>= level_shift;

        //av_log(NULL, AV_LOG_DEBUG, "i %d, j %d, end level %d\n", i, j, level);
        block[ctx->scan[i]] = (level^sign) - sign;

        UPDATE_CACHE(bs, &ctx->gb);
        GET_VLC(index1, bs, &ctx->gb, ctx->ac_vlc.table,
                DNXHD_VLC_BITS, 2);
    }

    CLOSE_READER(bs, &ctx->gb);
}

static void dnxhd_decode_dct_block_8(DNXHDContext *ctx, DCTELEM *block,
                                     int n, int qscale)
{
    dnxhd_decode_dct_block(ctx, block, n, qscale, 4, 32, 6);
}

static void dnxhd_decode_dct_block_10(DNXHDContext *ctx, DCTELEM *block,
                                      int n, int qscale)
{
    dnxhd_decode_dct_block(ctx, block, n, qscale, 6, 8, 4);
}

static int dnxhd_decode_macroblock(DNXHDContext *ctx, int x, int y)
{
    int shift1 = ctx->cid_table->bit_depth == 10;
    int dct_linesize_luma   = ctx->picture.linesize[0];
    int dct_linesize_chroma = ctx->picture.linesize[1];
    uint8_t *dest_y, *dest_u, *dest_v;
    int dct_y_offset, dct_x_offset;
    int qscale, i;

    qscale = get_bits(&ctx->gb, 11);
    skip_bits1(&ctx->gb);
    //av_log(ctx->avctx, AV_LOG_DEBUG, "qscale %d\n", qscale);

    if (qscale != ctx->last_qscale) {
        for (i = 0; i < 64; i++) {
            ctx->luma_scale[i]   = qscale * ctx->cid_table->luma_weight[i];
            ctx->chroma_scale[i] = qscale * ctx->cid_table->chroma_weight[i];
        }
        ctx->last_qscale = qscale;
    }

    for (i = 0; i < 8; i++) {
        ctx->dsp.clear_block(ctx->blocks[i]);
        ctx->decode_dct_block(ctx, ctx->blocks[i], i, qscale);
    }

    if (ctx->picture.interlaced_frame) {
        dct_linesize_luma   <<= 1;
        dct_linesize_chroma <<= 1;
    }

    dest_y = ctx->picture.data[0] + ((y * dct_linesize_luma)   << 4) + (x << (4 + shift1));
    dest_u = ctx->picture.data[1] + ((y * dct_linesize_chroma) << 4) + (x << (3 + shift1));
    dest_v = ctx->picture.data[2] + ((y * dct_linesize_chroma) << 4) + (x << (3 + shift1));

    if (ctx->cur_field) {
        dest_y += ctx->picture.linesize[0];
        dest_u += ctx->picture.linesize[1];
        dest_v += ctx->picture.linesize[2];
    }

    dct_y_offset = dct_linesize_luma << 3;
    dct_x_offset = 8 << shift1;
    ctx->dsp.idct_put(dest_y,                               dct_linesize_luma, ctx->blocks[0]);
    ctx->dsp.idct_put(dest_y + dct_x_offset,                dct_linesize_luma, ctx->blocks[1]);
    ctx->dsp.idct_put(dest_y + dct_y_offset,                dct_linesize_luma, ctx->blocks[4]);
    ctx->dsp.idct_put(dest_y + dct_y_offset + dct_x_offset, dct_linesize_luma, ctx->blocks[5]);

    if (!(ctx->avctx->flags & CODEC_FLAG_GRAY)) {
        dct_y_offset = dct_linesize_chroma << 3;
        ctx->dsp.idct_put(dest_u,                dct_linesize_chroma, ctx->blocks[2]);
        ctx->dsp.idct_put(dest_v,                dct_linesize_chroma, ctx->blocks[3]);
        ctx->dsp.idct_put(dest_u + dct_y_offset, dct_linesize_chroma, ctx->blocks[6]);
        ctx->dsp.idct_put(dest_v + dct_y_offset, dct_linesize_chroma, ctx->blocks[7]);
    }

    return 0;
}

static int dnxhd_decode_macroblocks(DNXHDContext *ctx, const uint8_t *buf, int buf_size)
{
    int x, y;
    for (y = 0; y < ctx->mb_height; y++) {
        ctx->last_dc[0] =
        ctx->last_dc[1] =
        ctx->last_dc[2] = 1 << (ctx->cid_table->bit_depth + 2); // for levels +2^(bitdepth-1)
        init_get_bits(&ctx->gb, buf + ctx->mb_scan_index[y], (buf_size - ctx->mb_scan_index[y]) << 3);
        for (x = 0; x < ctx->mb_width; x++) {
            //START_TIMER;
            dnxhd_decode_macroblock(ctx, x, y);
            //STOP_TIMER("decode macroblock");
        }
    }
    return 0;
}

static int dnxhd_decode_frame(AVCodecContext *avctx, void *data, int *data_size,
                              AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    DNXHDContext *ctx = avctx->priv_data;
    AVFrame *picture = data;
    int first_field = 1;
    int ret;
    int orig_buf_size = avpkt->size;
    int alphaPresent = 0;
    uint8_t *y = NULL;
    uint8_t *u = NULL;
    uint8_t *v = NULL;

    av_dlog(avctx, "frame size %d\n", buf_size);

 decode_coding_unit:
    if (dnxhd_decode_header(ctx, buf, buf_size, first_field&0x01) < 0)
        return -1;

    if ((avctx->width || avctx->height) &&
        (ctx->width != avctx->width || ctx->height != avctx->height)) {
        av_log(avctx, AV_LOG_WARNING, "frame size changed: %dx%d -> %dx%d\n",
               avctx->width, avctx->height, ctx->width, ctx->height);
        first_field = 1;
    }

    if (av_image_check_size(ctx->width, ctx->height, 0, avctx))
        return -1;
    avcodec_set_dimensions(avctx, ctx->width, ctx->height);

    if (first_field == 1) {
        if (ctx->picture.data[0])
            ff_thread_release_buffer(avctx, &ctx->picture);
        if ((ret = ff_thread_get_buffer(avctx, &ctx->picture)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
            return ret;
        }
    }

    dnxhd_decode_macroblocks(ctx, buf + 0x280, buf_size - 0x280);
        buf      += ctx->cid_table->coding_unit_size;
        buf_size -= ctx->cid_table->coding_unit_size;

    if (first_field&0x01 && ctx->picture.interlaced_frame) {
        first_field = 0;
        goto decode_coding_unit;
    }
    if( buf_size > 4 )
    {
        y = ctx->picture.data[0];
        u = ctx->picture.data[1];
        v = ctx->picture.data[2];

        ctx->picture.data[0] = av_malloc(ctx->picture.linesize[0]*(ctx->height+40));
        ctx->picture.data[1] = av_malloc(ctx->picture.linesize[1]*(ctx->height+40));
        ctx->picture.data[2] = av_malloc(ctx->picture.linesize[2]*(ctx->height+40));


        first_field = 3;
        goto decode_coding_unit;
    }
    else if( buf_size == 4 )
    {
        memcpy(ctx->picture.data[3], ctx->picture.data[0], ctx->picture.linesize[3]*ctx->height);
        if(ctx->picture.data[0] != NULL)
            av_free(ctx->picture.data[0]);
        if(ctx->picture.data[1] != NULL)
            av_free(ctx->picture.data[1]);
        if(ctx->picture.data[2] != NULL)
            av_free(ctx->picture.data[2]);

        ctx->picture.data[0] = y;
        ctx->picture.data[1] = u;
        ctx->picture.data[2] = v;

        avctx->pix_fmt = PIX_FMT_YUVA422P;

    }

    *picture = ctx->picture;
    *data_size = sizeof(AVPicture);
    return buf_size;
}

static av_cold int dnxhd_decode_close(AVCodecContext *avctx)
{
    DNXHDContext *ctx = avctx->priv_data;

    if (ctx->picture.data[0])
        ff_thread_release_buffer(avctx, &ctx->picture);
    free_vlc(&ctx->ac_vlc);
    free_vlc(&ctx->dc_vlc);
    free_vlc(&ctx->run_vlc);
    return 0;
}

AVCodec ff_dnxhd_decoder = {
    "dnxhd",
    AVMEDIA_TYPE_VIDEO,
    CODEC_ID_DNXHD,
    sizeof(DNXHDContext),
    dnxhd_decode_init,
    NULL,
    dnxhd_decode_close,
    dnxhd_decode_frame,
    CODEC_CAP_DR1 | CODEC_CAP_FRAME_THREADS,
    .long_name = NULL_IF_CONFIG_SMALL("VC3/DNxHD"),
};
