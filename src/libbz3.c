
#include "libbz3.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "cm.h"
#include "common.h"
#include "crc32.h"
#include "libsais.h"
#include "lzp.h"
#include "mtf.h"
#include "rle.h"
#include "srt.h"
#include "txt.h"

#define LZP_DICTIONARY 18
#define LZP_MIN_MATCH 40

struct bz3_state {
    u8 *swap_buffer;
    s32 block_size;
    s32 * sais_array;
    struct srt_state * srt_state;
    struct mtf_state * mtf_state;
    state * cm_state;
    s8 last_error;
};

s8 bz3_last_error(struct bz3_state * state) { return state->last_error; }

const char * bz3_strerror(struct bz3_state * state) {
    switch (state->last_error) {
        case BZ3_OK:
            return "No error";
        case BZ3_ERR_OUT_OF_BOUNDS:
            return "Data index out of bounds";
        case BZ3_ERR_BWT:
            return "Burrows-Wheeler transform failed";
        case BZ3_ERR_CRC:
            return "CRC32 check failed";
        case BZ3_ERR_MALFORMED_HEADER:
            return "Malformed header";
        case BZ3_ERR_TRUNCATED_DATA:
            return "Truncated data";
        case BZ3_ERR_DATA_TOO_BIG:
            return "Too much data";
        default:
            return "Unknown error";
    }
}

struct bz3_state * bz3_new(s32 block_size) {
    struct bz3_state * bz3_state = malloc(sizeof(struct bz3_state));

    if (!bz3_state) {
        return NULL;
    }

    bz3_state->cm_state = malloc(sizeof(state));
    bz3_state->srt_state = malloc(sizeof(struct srt_state));
    bz3_state->mtf_state = malloc(sizeof(struct mtf_state));

    bz3_state->swap_buffer = malloc(block_size + block_size / 4);
    bz3_state->sais_array = malloc(block_size * sizeof(s32) + 16);

    bz3_state->block_size = block_size;

    bz3_state->last_error = BZ3_OK;

    return bz3_state;
}

void bz3_free(struct bz3_state * state) {
    free(state->swap_buffer);
    free(state->sais_array);
    free(state->srt_state);
    free(state->mtf_state);
    free(state->cm_state);
    free(state);
}

#define swap(x, y) { u8 * tmp = x; x = y; y = tmp; }

s32 bz3_encode_block(struct bz3_state * state, u8 * buffer, s32 data_size) {
    u8 * b1 = buffer, * b2 = state->swap_buffer; s32 initial_size = data_size;

    if(data_size > state->block_size) {
        state->last_error = BZ3_ERR_DATA_TOO_BIG;
        return -1;
    }
    
    u32 crc32 = crc32sum(1, b1, data_size);

    // Ignore small blocks. They won't benefit from the entropy coding step.
    if(data_size < 64) {
        ((u32 *) (b1))[0] = htonl(crc32);
        ((s32 *) (b1))[1] = htonl(-1);
        memmove(b1 + 8, b1, data_size);
        return data_size + 8;
    }

    // Back to front:
    // bit 0: text | binary
    // bit 1: lzp | no lzp
    // bit 2: srt | no srt
    // bit 2: mtf | no mtf
    s8 model = is_text(b1, data_size);

    s32 lzp_size;
    if(model) {
        lzp_size = lzp_compress(b1, b2, data_size, LZP_DICTIONARY, LZP_MIN_MATCH);
        if(lzp_size > 0) {
            swap(b1, b2);
            data_size = lzp_size;
            model |= 2;
        }
    } else {
        lzp_size = mrlec(b1, data_size, b2);
        if(lzp_size < data_size) {
            swap(b1, b2);
            data_size = lzp_size;
            model |= 16;
        }
    }

    s32 bwt_idx = libsais_bwt(b1, b2, state->sais_array, data_size, 16, NULL);
    if(bwt_idx < 0) {
        state->last_error = BZ3_ERR_BWT;
        return -1;
    }
    
    // Important: b2 is the input now, b1 is the output.
    // This avoids an expensive memory copy.
    
    s32 srt_size;
    if((model & 1) == 0) {
        if(data_size > MiB(3)) {
            srt_size = srt_encode(state->srt_state, b2, b1, data_size);
            swap(b1, b2);
            data_size = srt_size;
            model |= 4;
        } else {
            mtf_encode(state->mtf_state, b2, b1, data_size);
            swap(b1, b2);
            model |= 8;
        }
    }

    // Compute the amount of overhead dwords.
    s32 overhead = 2; // CRC32 + BWT index
    if((model & 2) || (model & 16)) overhead++; // LZP
    if(model & 4) overhead++; // sorted rank transform

    begin(state->cm_state);
    state->cm_state->out_queue = b1 + overhead * 4 + 1;
    state->cm_state->output_ptr = 0;
    for (s32 i = 0; i < data_size; i++) encode_byte(state->cm_state, b2[i]);
    flush(state->cm_state);
    data_size = state->cm_state->output_ptr;

    // Write the header. Starting with common entries.
    ((u32 *) (b1))[0] = htonl(crc32);
    ((s32 *) (b1))[1] = htonl(bwt_idx);
    b1[8] = model;

    s32 p = 0;
    if((model & 2) || (model & 16)) ((s32 *)(b1 + 9))[p++] = htonl(lzp_size);
    if(model & 4) ((s32 *)(b1 + 9))[p++] = htonl(srt_size);

    state->last_error = BZ3_OK;

    // XXX: Better solution
    if(b1 != buffer)
        memcpy(buffer, b1, data_size + overhead * 4 + 1);

    return data_size + overhead * 4 + 1;
}

s32 bz3_decode_block(struct bz3_state * state, u8 * buffer, s32 data_size, s32 orig_size) {
    // Read the header.
    u32 crc32 = ntohl(((u32 *) buffer)[0]);
    s32 bwt_idx = ntohl(((s32 *) buffer)[1]);

    if(bwt_idx == -1) {
        memmove(buffer, buffer + 8, data_size - 8);
        return data_size - 8;
    }

    if(orig_size > state->block_size) {
        state->last_error = BZ3_ERR_DATA_TOO_BIG;
        return -1;
    }

    s8 model = buffer[8];
    s32 lzp_size = -1, srt_size = -1, p = 0;

    if((model & 2) || (model & 16)) lzp_size = ntohl(((s32 *) (buffer + 9))[p++]);
    if(model & 4) srt_size = ntohl(((s32 *) (buffer + 9))[p++]);

    p += 2;

    data_size -= p * 4 + 1;

    // Decode the data.
    u8 * b1 = buffer, * b2 = state->swap_buffer;

    begin(state->cm_state);
    state->cm_state->in_queue = b1 + p * 4 + 1;
    state->cm_state->input_ptr = 0;
    state->cm_state->input_max = data_size;
    init(state->cm_state);

    s32 size_src;

    if(model & 4)
        size_src = srt_size;
    else if((model & 2) || (model & 16))
        size_src = lzp_size;
    else
        size_src = orig_size;

    for (s32 i = 0; i < size_src; i++)
        b2[i] = decode_byte(state->cm_state);
    swap(b1, b2);

    // Undo SRT
    if(model & 4) {
        size_src = srt_decode(state->srt_state, b1, b2, srt_size);
        swap(b1, b2);
    } else if(model & 8) {
        mtf_decode(state->mtf_state, b1, b2, size_src);
        swap(b1, b2);
    }

    // Undo BWT
    if (libsais_unbwt(b1, b2, state->sais_array, size_src, NULL, bwt_idx) < 0) {
        state->last_error = BZ3_ERR_BWT;
        return -1;
    }
    swap(b1, b2);

    // Undo LZP
    if(model & 2) {
        size_src = lzp_decompress(b1, b2, lzp_size, LZP_DICTIONARY, (model & 1) ? LZP_MIN_MATCH : 2 * LZP_MIN_MATCH);
        swap(b1, b2);
    } else if(model & 16) {
        mrled(b1, b2, orig_size);
        size_src = orig_size;
        swap(b1, b2);
    }

    state->last_error = BZ3_OK;

    // XXX: Better solution
    if(b1 != buffer)
        memcpy(buffer, b1, size_src);
    
    if(crc32 != crc32sum(1, buffer, size_src)) {
        state->last_error = BZ3_ERR_CRC;
        return -1;
    }

    return size_src;
}

#undef swap