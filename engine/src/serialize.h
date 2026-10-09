#ifndef SPRAWL_SERIALIZE_H_
#define SPRAWL_SERIALIZE_H_

#include <stddef.h>
#include <stdint.h>
#include "engine.h"

#define HEADER_SIZE 304
#define STROKE_SIZE 72
#define POINT_SIZE 12
#define MAX_RECORD_SIZE 304

#define SPRAWL_DOC_VERSION 1

typedef enum {
    SprawlCodecStage_Header = 0,
    SprawlCodecStage_Strokes = 1,
    SprawlCodecStage_Points = 2,
    SprawlCodecStage_Done = 3,
} SprawlCodecStage;

typedef enum {
    SprawlDecodeError_Success = 0,
    SprawlDecodeError_NotASPRL = 1,
    SprawlDecodeError_UnsupportedVersion = 2,
    SprawlDecodeError_TooManyEntries = 3,
    SprawlDecodeError_CountMismatch = 4,
    SprawlDecodeError_InvalidValue = 5,
    SprawlDecodeError_OutOfOrder = 6,
    SprawlDecodeError_DuplicateStrokeIds = 7,
    SprawlDecodeError_SizeMismatch = 8,
} SprawlDecodeError;

typedef struct {
    const sprawl_document *doc;
    uint32_t stroke_count;
    uint32_t point_count;
    SprawlCodecStage stage;
    uint32_t strokes_written;
    uint32_t point_stroke_cursor;
    uint32_t point_point_cursor;
} sprawl_doc_encoder;

typedef struct {
    sprawl_document *doc;
    uint64_t total_bytes;
    uint32_t strokes_read;
    uint32_t points_head;
    uint32_t point_stroke_cursor;
    uint32_t point_point_cursor;
    uint64_t last_stroke_z;
    sprawl_id last_stroke_id;
    SprawlCodecStage stage;
    SprawlDecodeError error;
} sprawl_doc_decoder;

typedef struct {
    uint64_t id[2];
    uint32_t stroke_count;
    uint32_t point_count;
    float background[4];
    uint8_t title_len;
    uint8_t title[MAX_TITLE_LEN];
} sprawl_doc_header;

void doc_encoder_begin(sprawl_doc_encoder *enc, const sprawl_document *doc);
uint32_t doc_encoder_next(sprawl_doc_encoder *enc, uint8_t *buf, uint32_t cap);

void doc_decoder_begin(sprawl_doc_decoder *dec, sprawl_document *doc, uint64_t read_total);
SprawlDecodeError doc_read_header(const uint8_t *buf, const uint64_t total_bytes, sprawl_doc_header *out);
uint32_t doc_decoder_next(sprawl_doc_decoder *dec, const uint8_t *buf, const uint32_t cap);
bool doc_decoder_end(sprawl_doc_decoder *dec);
SprawlDecodeError doc_decoder_error(sprawl_doc_decoder *dec);

#endif
