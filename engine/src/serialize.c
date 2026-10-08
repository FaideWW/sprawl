#include "serialize.h"
#include "engine.h"
#include "renderer.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HEADER_SIZE 48
#define STROKE_SIZE 72
#define POINT_SIZE 12
#define MAX_RECORD_SIZE 72

#define SPRAWL_DOC_VERSION 1

/**
*   serialized document layout:
*   --- 
*
*   header   magic "SPRL" (4) | version u16 | reserved u16 | doc_id u64*2 | stroke_count u32 | point_count u32 | background f32*4
*   strokes  stroke_count * fixed-size record, in z order:
*            session u64 | seq u32 | z u64 | undo_len u32 | origin f64*2 | scale f64 | color f32*4 | radius f32 | point_count u32
*   points   point_count * (x f32 | y f32 | p f32), concatenated in stroke order
*/

static uint8_t *write_bytes(uint8_t *p, const void *src, size_t n) {
    memcpy(p, src, n);
    return p + n;
}

static uint8_t *write_u16(uint8_t *p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
    return p+2;
}

static uint8_t *write_u32(uint8_t *p, uint32_t v) {
    p = write_u16(p, v);
    p = write_u16(p, v >> 16);
    return p;
}

static uint8_t *write_u64(uint8_t *p, uint64_t v) {
    p = write_u32(p, v);
    p = write_u32(p, v >> 32);
    return p;
}

static uint8_t *write_f32(uint8_t *p, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return write_u32(p, u);
}   

static uint8_t *write_f64(uint8_t *p, double f) {
    uint64_t u;
    memcpy(&u, &f, 8);
    return write_u64(p, u);
}   

static const uint8_t *read_bytes(const uint8_t *p, void *dest, size_t n) {
    if (dest != NULL) {
        memcpy(dest, p, n);
    }
    return p + n;
}

static const uint8_t *read_u16(const uint8_t *p, uint16_t *v) {
    if (v != NULL) {
        *v = ((uint16_t)p[1] << 8) + p[0];
    }
    return p+2;
}

static const uint8_t *read_u32(const uint8_t *p, uint32_t *v) {
    uint16_t v0, v1;
    p = read_u16(p, &v0);
    p = read_u16(p, &v1);
    if (v != NULL) {
        *v = ((uint32_t)v1 << 16) + v0;
    }
    return p;
}

static const uint8_t *read_u64(const uint8_t *p, uint64_t *v) {
    uint32_t v0, v1;
    p = read_u32(p, &v0);
    p = read_u32(p, &v1);
    if (v != NULL) {
        *v = ((uint64_t)v1 << 32) + v0;
    }
    return p;
}

static const uint8_t *read_f32(const uint8_t *p, float *f) {
    uint32_t u;
    p = read_u32(p, &u);
    if (f != NULL) {
        memcpy(f, &u, 4);
    }
    return p;
}   

static const uint8_t *read_f64(const uint8_t *p, double *f) {
    uint64_t u;
    p = read_u64(p, &u);
    if (f != NULL) {
        memcpy(f, &u, 8);
    }
    return p;
}   

void doc_encoder_begin(sprawl_doc_encoder *enc, const sprawl_document *doc) {
    enc->doc = doc;
    enc->stroke_count = doc->stroke_count;
    uint32_t point_count = 0;
    for (uint32_t i = 0; i < doc->stroke_count; i++) {
        point_count += doc->strokes[i].points_count;
    }
    enc->point_count = point_count;
    enc->stage = SprawlCodecStage_Header;
    enc->strokes_written = 0;
    enc->point_stroke_cursor = 0;
    enc->point_point_cursor = 0;
}

static uint8_t *write_stroke(uint8_t *p, const sprawl_stroke *stroke) {
    const uint8_t *start = p;
    p = write_u64(p, stroke->id.session);
    p = write_u32(p, stroke->id.seq);
    p = write_u64(p, stroke->z);
    p = write_u32(p, stroke->undo_len);
    p = write_f64(p, stroke->origin[0]);
    p = write_f64(p, stroke->origin[1]);
    p = write_f64(p, stroke->scale);
    p = write_f32(p, stroke->color[0]);
    p = write_f32(p, stroke->color[1]);
    p = write_f32(p, stroke->color[2]);
    p = write_f32(p, stroke->color[3]);
    p = write_f32(p, stroke->radius);
    p = write_u32(p, stroke->points_count);
    
    assert(p - start == STROKE_SIZE);
    return p;
}

static uint8_t *write_point(uint8_t *p, const sprawl_point *point) {
    const uint8_t *start = p;
    p = write_f32(p, point->x);
    p = write_f32(p, point->y);
    p = write_f32(p, point->p);
    
    assert(p - start == POINT_SIZE);
    return p;
}

uint32_t doc_encoder_next(sprawl_doc_encoder *enc, uint8_t *buf, uint32_t cap) {
    assert(cap >= MAX_RECORD_SIZE);
    uint8_t *p = buf;
    const uint8_t *end = buf + cap;
    const sprawl_document *d = enc->doc;

    for (;;) {
        switch (enc->stage) {
            case SprawlCodecStage_Header: {
                if (end - p < HEADER_SIZE) return p - buf;
                uint8_t *start = p;
                p = write_bytes(p, "SPRL", 4);

                p = write_u16(p, SPRAWL_DOC_VERSION);
                p = write_u16(p, 0);

                p = write_u64(p, enc->doc->id[0]);
                p = write_u64(p, enc->doc->id[1]);

                p = write_u32(p, enc->stroke_count);
                p = write_u32(p, enc->point_count);

                p = write_f32(p, enc->doc->background[0]);
                p = write_f32(p, enc->doc->background[1]);
                p = write_f32(p, enc->doc->background[2]);
                p = write_f32(p, enc->doc->background[3]);
                assert (p - start == HEADER_SIZE);
                enc->stage = SprawlCodecStage_Strokes;
            } break;
            case SprawlCodecStage_Strokes: {
                if (enc->strokes_written == enc->stroke_count) {
                    enc->stage = SprawlCodecStage_Points;
                    break;
                }
                if (end - p < STROKE_SIZE) return p - buf;
                p = write_stroke(p, &d->strokes[enc->strokes_written++]);
            } break;
            case SprawlCodecStage_Points: {
                if (enc->point_stroke_cursor == enc->stroke_count) {
                    enc->stage = SprawlCodecStage_Done;
                    break;
                }
                const sprawl_stroke *s = &d->strokes[enc->point_stroke_cursor];
                if (enc->point_point_cursor == s->points_count) {
                    enc->point_stroke_cursor++;
                    enc->point_point_cursor = 0;
                    break;
                }
                if (end - p < POINT_SIZE) return p - buf;
                p = write_point(p, &d->points[s->first_point + enc->point_point_cursor++]);
            } break;
            case SprawlCodecStage_Done: {
                return p - buf;
            } break;
        }
    }
}

void doc_decoder_begin(sprawl_doc_decoder *dec, sprawl_document *doc, uint64_t total_bytes) {
    dec->doc = doc;
    dec->total_bytes = total_bytes;
    dec->strokes_read = 0;
    dec->points_head = 0;
    dec->point_stroke_cursor = 0;
    dec->point_point_cursor = 0;
    dec->stage = SprawlCodecStage_Header;
    dec->error = SprawlDecodeError_Success;
}

static const uint8_t *read_stroke(const uint8_t *p, sprawl_stroke *stroke, uint32_t *points_head) {
    const uint8_t *start = p;
    p = read_u64(p, &stroke->id.session);
    p = read_u32(p, &stroke->id.seq);
    p = read_u64(p, &stroke->z);
    p = read_u32(p, &stroke->undo_len);
    p = read_f64(p, &stroke->origin[0]);
    p = read_f64(p, &stroke->origin[1]);
    p = read_f64(p, &stroke->scale);
    p = read_f32(p, &stroke->color[0]);
    p = read_f32(p, &stroke->color[1]);
    p = read_f32(p, &stroke->color[2]);
    p = read_f32(p, &stroke->color[3]);
    p = read_f32(p, &stroke->radius);
    p = read_u32(p, &stroke->points_count);

    stroke->first_point = *points_head;
    
    assert(p - start == STROKE_SIZE);
    return p;
}

static SprawlDecodeError validate_stroke(sprawl_stroke *s) {
    for (int i = 0; i < 4; i++) {
        if (!(s->color[i] >= 0.0 && s->color[i] <= 1.0)) {
            fprintf(stderr, "stroke (%llu,%u).color (%u=%f)\n",s->id.session, s->id.seq, i, s->color[i]);
            return SprawlDecodeError_InvalidValue;
        }
    }

    if (s->scale <= 0.0 || s->radius < 0.0) {
        fprintf(stderr, "stroke (%llu,%u).scale (%f)\n",s->id.session, s->id.seq, s->scale);
        fprintf(stderr, "stroke (%llu,%u).radius (%f)\n",s->id.session, s->id.seq, s->radius);
        return SprawlDecodeError_InvalidValue;
    }

    if (!isfinite(s->origin[0]) || 
        !isfinite(s->origin[1]) || 
        !isfinite(s->scale) || 
        !isfinite(s->radius)) {
        fprintf(stderr, "stroke (%llu,%u).origin (%f,%f)\n",s->id.session, s->id.seq, s->origin[0], s->origin[1]);
        fprintf(stderr, "stroke (%llu,%u).scale (%f,%f)\n",s->id.session, s->id.seq, s->origin[0], s->scale);
        fprintf(stderr, "stroke (%llu,%u).radius (%f,%f)\n",s->id.session, s->id.seq, s->origin[0], s->radius);
        return SprawlDecodeError_InvalidValue;
    }

    return SprawlDecodeError_Success;
}

static SprawlDecodeError validate_point(sprawl_point *p) {
    if (p->p < 0.0 || p->p > 1.0) {
        return SprawlDecodeError_InvalidValue;
        fprintf(stderr, "point.p (%f)\n",p->p);
    }

    if (!isfinite(p->x) || 
        !isfinite(p->y) || 
        !isfinite(p->p)) {
        fprintf(stderr, "point.x (%f)\n",p->x);
        fprintf(stderr, "point.y (%f)\n",p->y);
        fprintf(stderr, "point.p (%f)\n",p->p);
        return SprawlDecodeError_InvalidValue;
    }

    return SprawlDecodeError_Success;
}

static const uint8_t *read_point(const uint8_t *p, sprawl_point *point) {
    const uint8_t *start = p;
    p = read_f32(p, &point->x);
    p = read_f32(p, &point->y);
    p = read_f32(p, &point->p);

    assert(p - start == POINT_SIZE);
    return p;
}

uint32_t doc_decoder_next(sprawl_doc_decoder *dec, const uint8_t *buf, const uint32_t cap) {
    if (dec->error != SprawlDecodeError_Success) return 0;
    const uint8_t *p = buf;
    const uint8_t *end = buf + cap;
    sprawl_document *d = dec->doc;

    for (;;) {
        switch (dec->stage) {
            case SprawlCodecStage_Header: {
                if (end - p < HEADER_SIZE) return p - buf;
                const uint8_t *start = p;

                uint8_t magic_buf[4];
                p = read_bytes(p, magic_buf, 4);

                if (memcmp((const char *)magic_buf, "SPRL", 4) != 0) {
                    dec->error = SprawlDecodeError_NotASPRL;
                    return p - buf;
                }
                
                uint16_t doc_version;
                p = read_u16(p, &doc_version);

                if (doc_version != SPRAWL_DOC_VERSION) {
                    dec->error = SprawlDecodeError_UnsupportedVersion;
                    return p - buf;
                }

                // reserved bytes are unused, so just advance past them
                p = read_u16(p, NULL);

                p = read_u64(p, &dec->doc->id[0]);
                p = read_u64(p, &dec->doc->id[1]);
                
                p = read_u32(p, &dec->doc->stroke_count);
                if (dec->doc->stroke_count > MAX_STROKES) {
                    dec->error = SprawlDecodeError_TooManyEntries;
                    return p - buf;
                }
                
                p = read_u32(p, &dec->doc->point_count);
                if (dec->doc->point_count > MAX_POINTS) {
                    dec->error = SprawlDecodeError_TooManyEntries;
                    return p - buf;
                }

                p = read_f32(p, &dec->doc->background[0]);
                p = read_f32(p, &dec->doc->background[1]);
                p = read_f32(p, &dec->doc->background[2]);
                p = read_f32(p, &dec->doc->background[3]);

                for (int i = 0; i < 4; i++) {
                    if (!(dec->doc->background[i] >= 0.0 && dec->doc->background[i] <= 1.0)) {
                        fprintf(stderr, "bg (%u=%f)\n", i, dec->doc->background[i]);
                        dec->error = SprawlDecodeError_InvalidValue;
                        return p - buf;
                    }
                }

                uint64_t expected = HEADER_SIZE + 
                    ((uint64_t)dec->doc->stroke_count * STROKE_SIZE) +
                    ((uint64_t)dec->doc->point_count * POINT_SIZE);

                if (expected != dec->total_bytes) {
                    dec->error = SprawlDecodeError_SizeMismatch;
                    return p - buf;
                }

                assert(p - start == HEADER_SIZE);
                dec->stage = SprawlCodecStage_Strokes;
            } break;
            case SprawlCodecStage_Strokes: {
                if (dec->strokes_read == dec->doc->stroke_count) {
                    if (dec->points_head != dec->doc->point_count) {
                        dec->error = SprawlDecodeError_CountMismatch;
                        return p - buf;
                    }
                    dec->stage = SprawlCodecStage_Points;
                    break;
                }
                if (end - p < STROKE_SIZE) return p - buf;
                p = read_stroke(p, &d->strokes[dec->strokes_read++], &dec->points_head);
                sprawl_stroke *s = &d->strokes[dec->strokes_read - 1];
                SprawlDecodeError err = validate_stroke(s);
                if (err != SprawlDecodeError_Success) {
                    dec->error = err;
                    return p - buf;
                }

                if (dec->strokes_read > 1) {
                    if (s->z < dec->last_stroke_z) {
                        dec->error = SprawlDecodeError_OutOfOrder;
                        return p - buf;
                    } else if (s->z == dec->last_stroke_z) {
                        int cmp_result = sprawl_compare_id(s->id, dec->last_stroke_id);
                        if (cmp_result < 0) {
                            dec->error = SprawlDecodeError_OutOfOrder;
                            return p - buf;
                        } else if (cmp_result == 0) {
                            dec->error = SprawlDecodeError_DuplicateStrokeIds;
                            return p - buf;
                        }
                    }
                }

                if (s->points_count > MAX_POINTS - dec->points_head) {
                    dec->error = SprawlDecodeError_TooManyEntries;
                    return p - buf;
                }
            
                dec->points_head += s->points_count;
                dec->last_stroke_z = s->z;
                dec->last_stroke_id = s->id;
            } break;
            case SprawlCodecStage_Points: {
                if (dec->point_stroke_cursor == dec->doc->stroke_count) {
                    dec->stage = SprawlCodecStage_Done;
                    break;
                }
                const sprawl_stroke *s = &d->strokes[dec->point_stroke_cursor];
                if (dec->point_point_cursor == s->points_count) {
                    dec->point_stroke_cursor++;
                    dec->point_point_cursor = 0;
                    break;
                }
                if (end - p < POINT_SIZE) return p - buf;
                p = read_point(p, &d->points[s->first_point + dec->point_point_cursor++]);
                SprawlDecodeError err = validate_point(&d->points[s->first_point + dec->point_point_cursor - 1]);
                if (err != SprawlDecodeError_Success) {
                    dec->error = err;
                    return p - buf;
                }
            } break;
            case SprawlCodecStage_Done: {
                return p - buf;
            } break;
        }
    }
    return 0;
}

// returns whether a document was successfully decoded
bool doc_decoder_end(sprawl_doc_decoder *dec) {
    return (
        dec->error == SprawlDecodeError_Success &&
        dec->stage == SprawlCodecStage_Done &&
        dec->strokes_read == dec->doc->stroke_count && 
        dec->points_head == dec->doc->point_count
    );
}

SprawlDecodeError doc_decoder_error(sprawl_doc_decoder *dec) {
    return dec->error;
}

