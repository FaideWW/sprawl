#include <stdint.h>
#include <string.h>

#include "store.h"
#include "btree.h"
#include "bytes.h"
#include "freelist.h"
#include "io.h"
#include "page.h"
#include "page_cache.h"
#include "profile.h"
#include "result.h"

#define SB_FORMAT_VERSION(n) get_u16le((n) + SB_OFFSET_VERSION)
#define SB_SEQ_NUM(n)        get_u64le((n) + SB_OFFSET_SEQNUM)
#define SB_TABLE_COUNT(n)    (*((n) + SB_OFFSET_TABLECOUNT))
#define SB_PAGE_COUNT(n)     get_u64le((n) + SB_OFFSET_PAGECOUNT)

static SprawlResult superblock_decode(const uint8_t *page, uint64_t page_num, sprawl_superblock *out) {
    SprawlResult r = page_checksum_verify(page);
    if (r == SprawlResult_OK) r = page_header_verify(page, page_num, PageKind_Superblock, 0, UINT64_MAX);
    if (r != SprawlResult_OK) return r;
    
    if (SB_FORMAT_VERSION(page) != SPRAWL_DOCUMENT_VERSION) return SprawlResult_Corrupt;
    if (PAGE_GENERATION(page) != SB_SEQ_NUM(page)) return SprawlResult_Corrupt;
    if (SB_TABLE_COUNT(page) > STORE_TABLES_MAX) return SprawlResult_Corrupt;
    if (SB_SEQ_NUM(page) % 2 != page_num) return SprawlResult_Corrupt;

    // reserved bytes must be 0
    for (uint32_t i = SB_OFFSET_VERSION + 2; i < SB_OFFSET_DOCID; i++) {
        if (*(page + i) != 0) return SprawlResult_Corrupt;
    }

    for (uint32_t i = SB_OFFSET_TABLECOUNT + 1; i < SB_OFFSET_TABLES; i++) {
        if (*(page + i) != 0) return SprawlResult_Corrupt;
    }

    out->seq_num = SB_SEQ_NUM(page);
    out->page_count = SB_PAGE_COUNT(page);
    memcpy(out->doc_id, page + SB_OFFSET_DOCID, 16);
    out->freelist_head.page = get_u64le(page + SB_OFFSET_FLHEAD);
    out->freelist_head.remaining = get_u32le(page + SB_OFFSET_FLHEAD + 8);
    out->table_count = *(page + SB_OFFSET_TABLECOUNT);
    for (uint32_t i = 0; i < out->table_count; i++) {
        const uint8_t *t_in = page + SB_OFFSET_TABLES + (i * SB_TABLE_SIZE);
        superblock_table_desc *t_out = &out->tables[i];
        t_out->root = get_u64le(t_in);
        t_out->id = *(t_in + 8);
        t_out->height = *(t_in + 9);
        t_out->key_size = get_u16le(t_in + 10);
        t_out->value_size = get_u16le(t_in + 12);
        
        if (t_out->height > TREE_HEIGHT_MAX) return SprawlResult_Corrupt;
        if ((t_out->root == 0) != (t_out->height == 0)) return SprawlResult_Corrupt;
        if (t_out->height > 0 && (2 > t_out->root || t_out->root >= out->page_count)) return SprawlResult_Corrupt;

        for (uint32_t j = 14; j < SB_TABLE_SIZE; j++) {
            if (*(t_in + j) != 0) return SprawlResult_Corrupt;
        }
    }

    if (out->page_count < 2) return SprawlResult_Corrupt;
    if (!fl_cursor_verify(out->freelist_head, out->page_count)) return SprawlResult_Corrupt;

    return SprawlResult_OK;
}

static void superblock_encode(const sprawl_superblock *in, uint8_t *page_out) {
    memset(page_out, 0, PAGE_SIZE_BYTES);
    sprawl_page_header h = {
        .kind = PageKind_Superblock,
        .page_num = in->seq_num % 2,
        .generation = in->seq_num,
    };
    page_header_write(page_out, &h);

    // superblock
    put_u16le(page_out + SB_OFFSET_VERSION, SPRAWL_DOCUMENT_VERSION);
    memcpy(page_out + SB_OFFSET_DOCID, in->doc_id, 16);
    put_u64le(page_out + SB_OFFSET_SEQNUM, in->seq_num);
    put_u64le(page_out + SB_OFFSET_PAGECOUNT, in->page_count);
    put_u64le(page_out + SB_OFFSET_FLHEAD, in->freelist_head.page);
    put_u32le(page_out + SB_OFFSET_FLHEAD + 8, in->freelist_head.remaining);
    *(page_out + SB_OFFSET_TABLECOUNT) = in->table_count;

    for (uint32_t i = 0; i < in->table_count; i++) {
        const superblock_table_desc *t_in = &in->tables[i];
        uint8_t *t_out = page_out + SB_OFFSET_TABLES + (i * SB_TABLE_SIZE);
        put_u64le(t_out, t_in->root);
        *(t_out + 8) = t_in->id;
        *(t_out + 9) = t_in->height;
        put_u16le(t_out + 10, t_in->key_size);
        put_u16le(t_out + 12, t_in->value_size);
    }

    page_checksum_write(page_out);
}

SprawlResult store_create(sprawl_store *s, sprawl_cache *c, const sprawl_table *tables, uint8_t table_count, 
                          const uint8_t doc_id[16]) {
    CHECK(table_count <= STORE_TABLES_MAX);
    if (s->state == StoreState_Empty) {
        s->c = c;
        memcpy(s->doc_id, doc_id, 16);

        s->table_count = table_count;
        for (uint32_t i = 0; i < table_count; i++) {
            sprawl_tree *t = &s->trees[i];
            t->table = &tables[i];

            t->root = 0;
            t->height = 0;
        }
        SprawlResult r = freelist_init(&s->f, s->c, (fl_cursor){0,0}, 2);
        CHECK(r == SprawlResult_OK);

        s->seq_num = 0;
        s->c->generation = s->seq_num + 1;
        s->state = StoreState_Ready;
    }

    return store_persist(s);
}

SprawlResult store_open(sprawl_store *s, sprawl_cache *c, const sprawl_table *tables, uint8_t table_count) {
    CHECK(s->state == StoreState_Empty || 
          (s->state >= StoreState_OpenPage0 && s->state <= StoreState_OpenFlush));
    CHECK(table_count <= STORE_TABLES_MAX);
    sprawl_superblock sb_temp = {0};
    if (s->state == StoreState_Empty) s->state = StoreState_OpenPage0;
    if (s->state == StoreState_OpenPage0) {
        SprawlResult r = cache_raw(c, IoOp_Read, 0, s->sb);
        if (r != SprawlResult_OK) return r;
        if (superblock_decode(s->sb, 0, &sb_temp) == SprawlResult_OK) {
            s->committed = sb_temp;
            s->has_committed = true;
        }
        s->state = StoreState_OpenPage1;
    }

    if (s->state == StoreState_OpenPage1) {
        SprawlResult r = cache_raw(c, IoOp_Read, 1, s->sb);
        if (r != SprawlResult_OK) return r;
        if (superblock_decode(s->sb, 1, &sb_temp) == SprawlResult_OK) {

            if (!s->has_committed) {
                s->committed = sb_temp;
                s->has_committed = true;
            } else {
                if (memcmp(s->committed.doc_id, sb_temp.doc_id, sizeof(sb_temp.doc_id)) != 0) return SprawlResult_Corrupt;
                if (s->committed.seq_num < sb_temp.seq_num) s->committed = sb_temp;
            }
        }

        if (!s->has_committed) return SprawlResult_Corrupt;

        if (s->committed.table_count != table_count) return SprawlResult_Corrupt;

        for (uint32_t i = 0; i < table_count; i++) {
            superblock_table_desc t = s->committed.tables[i];
            if (t.id != tables[i].id) return SprawlResult_Corrupt;
            if (t.key_size != tables[i].key_size) return SprawlResult_Corrupt;
            if (t.value_size != tables[i].value_size) return SprawlResult_Corrupt;
            s->trees[i] = (sprawl_tree){
                .height = t.height,
                .root = t.root,
                .table = &tables[i],
            };
        }

        s->table_count = s->committed.table_count;
        memcpy(s->doc_id, s->committed.doc_id, 16);
        s->seq_num = s->committed.seq_num;
        
        s->c = c;
        r = freelist_init(&s->f, s->c, s->committed.freelist_head, s->committed.page_count);
        if (r != SprawlResult_OK) return r;
        s->c->generation = s->committed.seq_num + 1;
        s->state = StoreState_OpenFlush;
    }

    if (s->state == StoreState_OpenFlush) {
        SprawlResult r = cache_flush(s->c);
        if (r != SprawlResult_OK) return r;
        s->state = StoreState_Ready;
    }
    
    return SprawlResult_OK;
}

static SprawlResult persist_fail(sprawl_store *s, SprawlResult r) {
    s->state = StoreState_Failed;
    s->error = r;
    if (s->c->in_flight > 0) return SprawlResult_Pending;
    return r;
}

static void superblock_fill(sprawl_store *s, sprawl_superblock *sb) {
    memset(sb, 0, sizeof(sprawl_superblock));
    sb->seq_num = s->seq_num + 1;
    sb->page_count = s->f.page_count;
    sb->freelist_head = freelist_head(&s->f);
    sb->table_count = s->table_count;
    memcpy(sb->doc_id, s->doc_id, 16);
    for (uint32_t i = 0; i < s->table_count; i++) {
        sb->tables[i] = (superblock_table_desc){
            .root = s->trees[i].root,
            .height = s->trees[i].height,
            .id = s->trees[i].table->id,
            .key_size = s->trees[i].table->key_size,
            .value_size = s->trees[i].table->value_size,
        };
    }
}

SprawlResult store_persist(sprawl_store *s) {
    if (s->state == StoreState_Failed) { 
        if (s->c->in_flight > 0) return SprawlResult_Pending;
        return s->error; 
    }
    CHECK(s->state == StoreState_Ready || 
          (s->state >= StoreState_PersistPages && s->state <= StoreState_PersistFlush2));
    CHECK(s->c->reserved == 0);
    CHECK(s->c->generation == s->seq_num + 1);
    if (s->state == StoreState_Ready) {
        s->state = StoreState_PersistPages;
    }

    if (s->state == StoreState_PersistPages) {
        SprawlResult r = cache_write_dirty(s->c);
        if (r == SprawlResult_Pending) return r;
        if (r != SprawlResult_OK) return persist_fail(s, r);

        s->state = StoreState_PersistFlush1;
    }

    if (s->state == StoreState_PersistFlush1) {
        SprawlResult r = cache_flush(s->c);
        if (r == SprawlResult_Pending) return r;
        if (r != SprawlResult_OK) return persist_fail(s, r);
        
        superblock_fill(s, &s->next);
        
        superblock_encode(&s->next, s->sb);
        s->state = StoreState_PersistSuperblock;
    }

    if (s->state == StoreState_PersistSuperblock) {
        SprawlResult r = cache_raw(s->c, IoOp_Write, (s->seq_num + 1) % 2, s->sb);
        if (r == SprawlResult_Pending) return r;
        if (r != SprawlResult_OK) return persist_fail(s, r);

        s->state = StoreState_PersistFlush2;
    }

    if (s->state == StoreState_PersistFlush2) {
        SprawlResult r = cache_flush(s->c);
        if (r == SprawlResult_Pending) return r;
        if (r != SprawlResult_OK) return persist_fail(s, r);

        s->seq_num++;
        s->c->generation++;
        s->committed = s->next;
        freelist_advance(&s->f);
        s->state = StoreState_Ready;
    }

    return SprawlResult_OK;
}

sprawl_alloc store_alloc(sprawl_store *s) {
    return freelist_alloc(&s->f);
}

void txn_begin(sprawl_store *s) {
    CHECK(s->state == StoreState_Ready);
    CHECK(s->committed.page_count == s->f.page_count);
    fl_cursor flc = freelist_head(&s->f);
    CHECK(flc.page == s->committed.freelist_head.page && flc.remaining == s->committed.freelist_head.remaining);
    
    for (uint32_t i = 0; i < s->table_count; i++) {
        CHECK(s->trees[i].root == s->committed.tables[i].root);
        CHECK(s->trees[i].height == s->committed.tables[i].height);
    }

    s->state = StoreState_InTransaction;
}

void txn_commit(sprawl_store *s) {
    CHECK(s->state == StoreState_InTransaction);
    s->state = StoreState_Ready;
}

SprawlResult txn_abort(sprawl_store *s) {
    CHECK(s->state == StoreState_InTransaction);
    if (s->c->in_flight > 0) return SprawlResult_Pending;

    cache_drop_all(s->c);

    for (uint32_t i = 0; i < s->committed.table_count; i++) {
        superblock_table_desc t = s->committed.tables[i];
        s->trees[i].root = t.root;
        s->trees[i].height = t.height;
    }

    CHECK(freelist_init(&s->f, s->c, s->committed.freelist_head, s->committed.page_count) == SprawlResult_OK);

    s->state = StoreState_Ready;
    return SprawlResult_OK;
}
