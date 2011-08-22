/*
 * copyright (c) 2009 Michael Niedermayer
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

#ifndef AVFORMAT_METADATA_H
#define AVFORMAT_METADATA_H

/**
 * @file
 * internal metadata API header
 * see avformat.h or the public API!
 */


#include "avformat.h"

struct AVMetadata{
    int count;
    AVMetadataTag *elems;
};

struct AVMetadataConv{
    const char *native;
    const char *generic;
};
#if !FF_API_OLD_METADATA
typedef struct AVMetadataConv AVMetadataConv;
#endif

#if FF_API_OLD_METADATA
void ff_metadata_demux_compat(AVFormatContext *s);
void ff_metadata_mux_compat(AVFormatContext *s);
#endif

void ff_metadata_conv(AVMetadata **pm, const AVMetadataConv *d_conv,
                                       const AVMetadataConv *s_conv);
void ff_metadata_conv2(AVMetadata **dst, AVMetadata **pm,
                       const AVMetadataConv *d_conv,
                       const AVMetadataConv *s_conv);
void ff_metadata_conv_ctx(AVFormatContext *ctx, const AVMetadataConv *d_conv,
                                                const AVMetadataConv *s_conv);

#endif /* AVFORMAT_METADATA_H */