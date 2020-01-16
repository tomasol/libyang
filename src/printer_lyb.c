/**
 * @file printer_lyb.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief LLLYB printer for libyang data structure
 *
 * Copyright (c) 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#ifdef __APPLE__
# include <libkern/OSByteOrder.h>
# define htole64(x) OSSwapHostToLittleInt64(x)
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
# include <sys/endian.h>
#elif defined(__sun__)
# include <endian.h>
# include <sys/byteorder.h>
# if defined(_BIG_ENDIAN)
#  define le64toh(x) BSWAP_64(x)
#  define htole64(x) le64toh(x)
# else
#  define le64toh(x) (x)
#  define htole64(x) (x)
# endif
#else
# include <endian.h>
#endif

#include "common.h"
#include "printer.h"
#include "tree_schema.h"
#include "tree_data.h"
#include "resolve.h"
#include "tree_internal.h"

static int
lllyb_hash_equal_cb(void *UNUSED(val1_p), void *UNUSED(val2_p), int UNUSED(mod), void *UNUSED(cb_data))
{
    /* for this purpose, if hash matches, the value does also, we do not want 2 values to have the same hash */
    return 1;
}

static int
lllyb_ptr_equal_cb(void *val1_p, void *val2_p, int UNUSED(mod), void *UNUSED(cb_data))
{
    struct lllys_node *val1 = *(struct lllys_node **)val1_p;
    struct lllys_node *val2 = *(struct lllys_node **)val2_p;

    if (val1 == val2) {
        return 1;
    }
    return 0;
}

/* check that sibling collision hash i is safe to insert into ht
 * return: 0 - no whole hash sequence collision, 1 - whole hash sequence collision, -1 - fatal error
 */
static int
lllyb_hash_sequence_check(struct hash_table *ht, struct lllys_node *sibling, int ht_col_id, int compare_col_id)
{
    int j;
    struct lllys_node **col_node;

    /* get the first node inserted with last hash col ID ht_col_id */
    if (lllyht_find(ht, &sibling, lllyb_hash(sibling, ht_col_id), (void **)&col_node)) {
        /* there is none. valid situation */
        return 0;
    }

    lllyht_set_cb(ht, lllyb_ptr_equal_cb);
    do {
        for (j = compare_col_id; j > -1; --j) {
            if (lllyb_hash(sibling, j) != lllyb_hash(*col_node, j)) {
                /* one non-colliding hash */
                break;
            }
        }
        if (j == -1) {
            /* all whole hash sequences of nodes inserted with last hash col ID compare_col_id collide */
            lllyht_set_cb(ht, lllyb_hash_equal_cb);
            return 1;
        }

        /* get next node inserted with last hash col ID ht_col_id */
    } while (!lllyht_find_next(ht, col_node, lllyb_hash(*col_node, ht_col_id), (void **)&col_node));

    lllyht_set_cb(ht, lllyb_hash_equal_cb);
    return 0;
}

#ifndef NDEBUG

static int
lllyb_check_augment_collision(struct hash_table *ht, struct lllys_node *aug1, struct lllys_node *aug2)
{
    struct lllys_node *iter1 = NULL, *iter2 = NULL;
    int i, coliding = 0;
    values_equal_cb cb = NULL;
    LLLYB_HASH hash1, hash2;

    /* go through combination of all nodes and check if coliding hash is used */
    while ((iter1 = (struct lllys_node *)lllys_getnext(iter1, aug1, aug1->module, 0))) {
        iter2 = NULL;
        while ((iter2 = (struct lllys_node *)lllys_getnext(iter2, aug2, aug2->module, 0))) {
            coliding = 0;
            for (i = 0; i < LLLYB_HASH_BITS; i++) {
                hash1 = lllyb_hash(iter1, i);
                hash2 = lllyb_hash(iter2, i);
                LLLY_CHECK_ERR_RETURN(!hash1 || !hash2, LOGINT(aug1->module->ctx), 0);

                if (hash1 == hash2) {
                    coliding++;
                    /* if one of values with coliding hash is in hash table, we have a problem */
                    cb = lllyht_set_cb(ht, lllyb_ptr_equal_cb);
                    if ((lllyht_find(ht, &iter1, hash1, NULL) == 0) || (lllyht_find(ht, &iter2, hash2, NULL) == 0)) {
                        LOGWRN(aug1->module->ctx, "Augmentations from modules \"%s\" and \"%s\" have fatal hash collision.",
                               iter1->module->name, iter2->module->name);
                        LOGWRN(aug1->module->ctx, "It will cause no errors if module \"%s\" is always loaded before \"%s\".",
                               iter1->module->name, iter2->module->name);
                        lllyht_set_cb(ht, cb);
                        return 1;
                    }
                    lllyht_set_cb(ht, cb);
                }
            }
            LLLY_CHECK_ERR_RETURN(coliding == LLLYB_HASH_BITS, LOGINT(aug1->module->ctx), 1);
        }
    }

    /* no used hashes with collision found */
    return 0;
}

static void
lllyb_check_augments(struct lllys_node *parent, struct hash_table *ht)
{
    struct lllys_node *sibling = NULL, **augs = NULL;
    void *ret;
    int augs_size = 1, augs_found = 0, i, j, found;
    struct lllys_module *mod;

    assert(parent);
    mod = lllys_node_module(parent);

    augs = malloc(sizeof sibling * augs_size);
    LLLY_CHECK_ERR_RETURN(!augs, LOGMEM(mod->ctx), );

    while ((sibling = (struct lllys_node *)lllys_getnext(sibling, parent, NULL, 0))) {
        /* build array of all augments from different modules */
        if (sibling->parent->nodetype == LLLYS_AUGMENT && lllys_node_module(sibling->parent) != mod) {
            found = 0;
            for (i = 0; i < augs_found; i++) {
                if (lllys_node_module(augs[i]) == lllys_node_module(sibling)) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (augs_size == augs_found) {
                    augs_size *= 2;
                    ret = realloc(augs, sizeof sibling * augs_size);
                    if (!ret) {
                        LOGMEM(mod->ctx);
                        free(augs);
                        return;
                    }
                    augs = ret;
                }
                augs[augs_found] = sibling;
                augs_found++;
            }
        }
    }
    /* check collisions for every pair */
    for (i = 0; i < augs_found; i++) {
        for (j = i + 1; j < augs_found; j++) {
            if (lllyb_check_augment_collision(ht, augs[i]->parent, augs[j]->parent)) {
                free(augs);
                return;
            }
        }
    }
    free(augs);
    return;
}

#endif

static struct hash_table *
lllyb_hash_siblings(struct lllys_node *sibling, const struct lllys_module **models, int mod_count)
{
    struct hash_table *ht;
    struct lllys_node *parent;
    const struct lllys_module *mod;
    int i, j;
#ifndef NDEBUG
    int aug_col = 0;
    const struct lllys_module *aug_mod = NULL;
#endif

    ht = lllyht_new(1, sizeof(struct lllys_node *), lllyb_hash_equal_cb, NULL, 1);
    LLLY_CHECK_ERR_RETURN(!ht, LOGMEM(sibling->module->ctx), NULL);

    for (parent = lllys_parent(sibling);
         parent && (parent->nodetype & (LLLYS_USES | LLLYS_CHOICE | LLLYS_CASE));
         parent = lllys_parent(parent));
    mod = lllys_node_module(sibling);

    sibling = NULL;
    /* ignore features so that their state does not affect hashes */
    while ((sibling = (struct lllys_node *)lllys_getnext(sibling, parent, mod, LLLYS_GETNEXT_NOSTATECHECK))) {
        if (models && !lllyb_has_schema_model(sibling, models, mod_count)) {
            /* ignore models not present during printing */
            continue;
        }

#ifndef NDEBUG
        if (sibling->parent && sibling->parent->nodetype == LLLYS_AUGMENT && lllys_node_module(sibling->parent) != mod) {
            if (aug_mod && aug_mod != lllys_node_module(sibling->parent)) {
                aug_col = 1;
            }
            aug_mod = lllys_node_module(sibling);
        }
#endif

        /* find the first non-colliding hash (or specifically non-colliding hash sequence) */
        for (i = 0; i < LLLYB_HASH_BITS; ++i) {
            /* check that we are not colliding with nodes inserted with a lower collision ID than ours */
            for (j = i - 1; j > -1; --j) {
                if (lllyb_hash_sequence_check(ht, sibling, j, i)) {
                    break;
                }
            }
            if (j > -1) {
                /* some check failed, we must use a higher collision ID */
                continue;
            }

            /* try to insert node with the current collision ID */
            if (!lllyht_insert_with_resize_cb(ht, &sibling, lllyb_hash(sibling, i), lllyb_ptr_equal_cb, NULL)) {
                /* success, no collision */
                break;
            }

            /* make sure we really cannot insert it with this hash col ID (meaning the whole hash sequence is colliding) */
            if (i && !lllyb_hash_sequence_check(ht, sibling, i, i)) {
                /* it can be inserted after all, even though there is already a node with the same last collision ID */
                lllyht_set_cb(ht, lllyb_ptr_equal_cb);
                if (lllyht_insert(ht, &sibling, lllyb_hash(sibling, i), NULL)) {
                    lllyht_set_cb(ht, lllyb_hash_equal_cb);
                    LOGINT(sibling->module->ctx);
                    lllyht_free(ht);
                    return NULL;
                }
                lllyht_set_cb(ht, lllyb_hash_equal_cb);
                break;
            }
            /* there is still another colliding schema node with the same hash sequence, try higher collision ID */
        }

        if (i == LLLYB_HASH_BITS) {
            /* wow */
            LOGINT(sibling->module->ctx);
            lllyht_free(ht);
            return NULL;
        }
    }

#ifndef NDEBUG
    if (aug_col) {
        lllyb_check_augments(parent, ht);
    }
#endif

    /* change val equal callback so that the HT is usable for finding value hashes */
    lllyht_set_cb(ht, lllyb_ptr_equal_cb);

    return ht;
}

static LLLYB_HASH
lllyb_hash_find(struct hash_table *ht, struct lllys_node *node)
{
    LLLYB_HASH hash;
    uint32_t i;

    for (i = 0; i < LLLYB_HASH_BITS; ++i) {
        hash = lllyb_hash(node, i);
        if (!hash) {
            LOGINT(node->module->ctx);
            return 0;
        }

        if (!lllyht_find(ht, &node, hash, NULL)) {
            /* success, no collision */
            break;
        }
    }
    /* cannot happen, we already calculated the hash */
    if (i == LLLYB_HASH_BITS) {
        LOGINT(node->module->ctx);
        return 0;
    }

    return hash;
}

/* writing function handles writing size information */
static int
lllyb_write(struct lllyout *out, const uint8_t *buf, size_t count, struct lllyb_state *lllybs)
{
    int ret = 0, i, full_chunk_i;
    size_t r, to_write;
    uint8_t meta_buf[LLLYB_META_BYTES];

    assert(out && lllybs);

    while (1) {
        /* check for full data chunks */
        to_write = count;
        full_chunk_i = -1;
        for (i = 0; i < lllybs->used; ++i) {
            /* we want the innermost chunks resolved first, so replace previous full chunks */
            if (lllybs->written[i] + to_write >= LLLYB_SIZE_MAX) {
                /* full chunk, do not write more than allowed */
                to_write = LLLYB_SIZE_MAX - lllybs->written[i];
                full_chunk_i = i;
            }
        }

        if ((full_chunk_i == -1) && !count) {
            break;
        }

        /* we are actually writing some data, not just finishing another chunk */
        if (to_write) {
            r = llly_write(out, (char *)buf, to_write);
            if (r < to_write) {
                return -1;
            }

            for (i = 0; i < lllybs->used; ++i) {
                /* increase all written counters */
                lllybs->written[i] += r;
                assert(lllybs->written[i] <= LLLYB_SIZE_MAX);
            }
            /* decrease count/buf */
            count -= r;
            buf += r;

            ret += r;
        }

        if (full_chunk_i > -1) {
            /* write the meta information (inner chunk count and chunk size) */
            meta_buf[0] = lllybs->written[full_chunk_i] & 0xFF;
            meta_buf[1] = lllybs->inner_chunks[full_chunk_i] & 0xFF;

            r = llly_write_skipped(out, lllybs->position[full_chunk_i], (char *)meta_buf, LLLYB_META_BYTES);
            if (r < LLLYB_META_BYTES) {
                return -1;
            }

            /* zero written and inner chunks */
            lllybs->written[full_chunk_i] = 0;
            lllybs->inner_chunks[full_chunk_i] = 0;

            /* skip space for another chunk size */
            r = llly_write_skip(out, LLLYB_META_BYTES, &lllybs->position[full_chunk_i]);
            if (r < LLLYB_META_BYTES) {
                return -1;
            }

            ret += r;

            /* increase inner chunk count */
            for (i = 0; i < full_chunk_i; ++i) {
                if (lllybs->inner_chunks[i] == LLLYB_INCHUNK_MAX) {
                    LOGINT(lllybs->ctx);
                    return -1;
                }
                ++lllybs->inner_chunks[i];
            }
        }
    }

    return ret;
}

static int
lllyb_write_stop_subtree(struct lllyout *out, struct lllyb_state *lllybs)
{
    int r;
    uint8_t meta_buf[LLLYB_META_BYTES];

    /* write the meta chunk information */
    meta_buf[0] = lllybs->written[lllybs->used - 1] & 0xFF;
    meta_buf[1] = lllybs->inner_chunks[lllybs->used - 1] & 0xFF;

    r = llly_write_skipped(out, lllybs->position[lllybs->used - 1], (char *)&meta_buf, LLLYB_META_BYTES);
    if (r < LLLYB_META_BYTES) {
        return -1;
    }

    --lllybs->used;
    return 0;
}

static int
lllyb_write_start_subtree(struct lllyout *out, struct lllyb_state *lllybs)
{
    int i;

    if (lllybs->used == lllybs->size) {
        lllybs->size += LLLYB_STATE_STEP;
        lllybs->written = llly_realloc(lllybs->written, lllybs->size * sizeof *lllybs->written);
        lllybs->position = llly_realloc(lllybs->position, lllybs->size * sizeof *lllybs->position);
        lllybs->inner_chunks = llly_realloc(lllybs->inner_chunks, lllybs->size * sizeof *lllybs->inner_chunks);
        LLLY_CHECK_ERR_RETURN(!lllybs->written || !lllybs->position || !lllybs->inner_chunks, LOGMEM(lllybs->ctx), -1);
    }

    ++lllybs->used;
    lllybs->written[lllybs->used - 1] = 0;
    lllybs->inner_chunks[lllybs->used - 1] = 0;

    /* another inner chunk */
    for (i = 0; i < lllybs->used - 1; ++i) {
        if (lllybs->inner_chunks[i] == LLLYB_INCHUNK_MAX) {
            LOGINT(lllybs->ctx);
            return -1;
        }
        ++lllybs->inner_chunks[i];
    }

    return llly_write_skip(out, LLLYB_META_BYTES, &lllybs->position[lllybs->used - 1]);
}

static int
lllyb_write_number(uint64_t num, size_t bytes, struct lllyout *out, struct lllyb_state *lllybs)
{
    /* correct byte order */
    num = htole64(num);

    return lllyb_write(out, (uint8_t *)&num, bytes, lllybs);
}

static int
lllyb_write_enum(uint32_t enum_idx, uint32_t count, struct lllyout *out, struct lllyb_state *lllybs)
{
    size_t bytes;

    assert(enum_idx < count);

    if (count < (1 << 8)) {
        bytes = 1;
    } else if (count < (1 << 16)) {
        bytes = 2;
    } else if (count < (1 << 24)) {
        bytes = 3;
    } else {
        bytes = 4;
    }

    return lllyb_write_number(enum_idx, bytes, out, lllybs);
}

static int
lllyb_write_string(const char *str, size_t str_len, int with_length, struct lllyout *out, struct lllyb_state *lllybs)
{
    int r, ret = 0;

    if (!str_len) {
        str_len = strlen(str);
    }

    if (with_length) {
        /* print length on 2 bytes */
        if (str_len > UINT16_MAX) {
            LOGINT(lllybs->ctx);
            return -1;
        }
        ret += (r = lllyb_write_number(str_len, 2, out, lllybs));
        if (r < 0) {
            return -1;
        }
    }

    ret += (r = lllyb_write(out, (const uint8_t *)str, str_len, lllybs));
    if (r < 0) {
        return -1;
    }

    return ret;
}

static int
lllyb_print_model(struct lllyout *out, const struct lllys_module *mod, struct lllyb_state *lllybs)
{
    int r, ret = 0;
    uint16_t revision;

    /* model name length and model name */
    ret += (r = lllyb_write_string(mod->name, 0, 1, out, lllybs));
    if (r < 0) {
        return -1;
    }

    /* model revision as XXXX XXXX XXXX XXXX (2B) (year is offset from 2000)
     *                   YYYY YYYM MMMD DDDD */
    revision = 0;
    if (mod->rev_size) {
        r = atoi(mod->rev[0].date);
        r -= 2000;
        r <<= 9;

        revision |= r;

        r = atoi(mod->rev[0].date + 5);
        r <<= 5;

        revision |= r;

        r = atoi(mod->rev[0].date + 8);

        revision |= r;
    }
    ret += (r = lllyb_write_number(revision, sizeof revision, out, lllybs));
    if (r < 0) {
        return -1;
    }

    return ret;
}

static int
is_added_model(const struct lllys_module **models, size_t mod_count, const struct lllys_module *mod)
{
    size_t i;

    for (i = 0; i < mod_count; ++i) {
        if (models[i] == mod) {
            return 1;
        }
    }

    return 0;
}

static void
add_model(const struct lllys_module ***models, size_t *mod_count, const struct lllys_module *mod)
{
    if (is_added_model(*models, *mod_count, mod)) {
        return;
    }

    *models = llly_realloc(*models, ++(*mod_count) * sizeof **models);
    (*models)[*mod_count - 1] = mod;
}

static int
lllyb_print_data_models(struct lllyout *out, const struct lllyd_node *root, struct lllyb_state *lllybs)
{
    int ret = 0;
    const struct lllys_module **models = NULL, *mod;
    const struct lllys_submodule *submod;
    const struct lllyd_node *node;
    size_t mod_count = 0;
    uint32_t idx = 0, i, j;

    /* first, collect all data node modules */
    LLLY_TREE_FOR(root, node) {
        mod = lllyd_node_module(node);
        add_model(&models, &mod_count, mod);
    }

    if (root) {
        /* then add all models augmenting or deviating the used models */
        idx = llly_ctx_internal_modules_count(root->schema->module->ctx);
        while ((mod = llly_ctx_get_module_iter(root->schema->module->ctx, &idx))) {
            if (!mod->implemented) {
next_mod:
                continue;
            }

            for (i = 0; i < mod->deviation_size; ++i) {
                if (mod->deviation[i].orig_node && is_added_model(models, mod_count, lllys_node_module(mod->deviation[i].orig_node))) {
                    add_model(&models, &mod_count, mod);
                    goto next_mod;
                }
            }
            for (i = 0; i < mod->augment_size; ++i) {
                if (is_added_model(models, mod_count, lllys_node_module(mod->augment[i].target))) {
                    add_model(&models, &mod_count, mod);
                    goto next_mod;
                }
            }

            /* submodules */
            for (j = 0; j < mod->inc_size; ++j) {
                submod = mod->inc[j].submodule;

                for (i = 0; i < submod->deviation_size; ++i) {
                    if (submod->deviation[i].orig_node && is_added_model(models, mod_count, lllys_node_module(submod->deviation[i].orig_node))) {
                        add_model(&models, &mod_count, mod);
                        goto next_mod;
                    }
                }
                for (i = 0; i < submod->augment_size; ++i) {
                    if (is_added_model(models, mod_count, lllys_node_module(submod->augment[i].target))) {
                        add_model(&models, &mod_count, mod);
                        goto next_mod;
                    }
                }
            }
        }
    }

    /* now write module count on 2 bytes */
    ret += lllyb_write_number(mod_count, 2, out, lllybs);

    /* and all the used models */
    for (i = 0; i < mod_count; ++i) {
        ret += lllyb_print_model(out, models[i], lllybs);
    }

    free(models);
    return ret;
}

static int
lllyb_print_magic_number(struct lllyout *out)
{
    uint32_t magic_number;

    /* 'l', 'y', 'b' - 0x6c7962 */
    ((char *)&magic_number)[0] = 'l';
    ((char *)&magic_number)[1] = 'y';
    ((char *)&magic_number)[2] = 'b';

    return llly_write(out, (char *)&magic_number, 3);
}

static int
lllyb_print_header(struct lllyout *out)
{
    int ret = 0;
    uint8_t byte = 0;

    /* TODO version, some other flags? */
    ret += llly_write(out, (char *)&byte, sizeof byte);

    return ret;
}

static int
lllyb_print_anydata(struct lllyd_node_anydata *anydata, struct lllyout *out, struct lllyb_state *lllybs)
{
    int ret = 0, len;
    char *buf;

    if (anydata->value_type == LLLYD_ANYDATA_XML) {
        /* transform XML into CONSTSTRING */
        lllyxml_print_mem(&buf, anydata->value.xml, LLLYXML_PRINT_SIBLINGS);
        lllyxml_free(anydata->schema->module->ctx, anydata->value.xml);

        anydata->value_type = LLLYD_ANYDATA_CONSTSTRING;
        anydata->value.str = lllydict_insert_zc(anydata->schema->module->ctx, buf);
    } else if (anydata->value_type == LLLYD_ANYDATA_DATATREE) {
        /* print data tree into LLLYB */
        lllyd_print_mem(&buf, anydata->value.tree, LLLYD_LYB, LLLYP_WITHSIBLINGS);
        lllyd_free_withsiblings(anydata->value.tree);

        anydata->value_type = LLLYD_ANYDATA_LYB;
        anydata->value.mem = buf;
    } else if (anydata->value_type & LLLYD_ANYDATA_STRING) {
        /* dynamic value, only used for input */
        LOGERR(lllybs->ctx, LLLY_EINT, "Unsupported anydata value type to print.");
        return -1;
    }

    /* first byte is type */
    ret += lllyb_write(out, (uint8_t *)&anydata->value_type, sizeof anydata->value_type, lllybs);

    /* followed by the content */
    if (anydata->value_type == LLLYD_ANYDATA_LYB) {
        len = lllyd_lyb_data_length(anydata->value.mem);
        if (len > -1) {
            ret += lllyb_write_string(anydata->value.str, (size_t)len, 0, out, lllybs);
        } else {
            ret = len;
        }
    } else {
        ret += lllyb_write_string(anydata->value.str, 0, 0, out, lllybs);
    }

    return ret;
}

static int
lllyb_print_value(const struct lllys_type *type, const char *value_str, lllyd_val value, LLLY_DATA_TYPE value_type,
                uint8_t value_flags, uint8_t dflt, struct lllyout *out, struct lllyb_state *lllybs)
{
    int ret = 0;
    uint8_t byte = 0;
    size_t count, i, bits_i;
    LLLY_DATA_TYPE dtype;

    /* value type byte - ABCD DDDD
     *
     * A - dflt flag
     * B - user type flag
     * C - unres flag
     * D (5b) - data type value
     */
    if (dflt) {
        byte |= 0x80;
    }
    if (value_flags & LLLY_VALUE_USER) {
        byte |= 0x40;
    }
    if (value_flags & LLLY_VALUE_UNRES) {
        byte |= 0x20;
    }

    /* we have only 5b available, must be enough */
    assert((value_type & 0x1f) == value_type);

    /* find actual type */
    while (type->base == LLLY_TYPE_LEAFREF) {
        type = &type->info.lref.target->type;
    }

    if ((value_flags & LLLY_VALUE_USER) || (type->base == LLLY_TYPE_UNION)) {
        value_type = LLLY_TYPE_STRING;
    } else while (value_type == LLLY_TYPE_LEAFREF) {
        assert(!(value_flags & LLLY_VALUE_UNRES));

        /* update value_type and value to that of the target */
        value_type = ((struct lllyd_node_leaf_list *)value.leafref)->value_type;
        value = ((struct lllyd_node_leaf_list *)value.leafref)->value;
    }

    /* store the value type */
    byte |= value_type & 0x1f;

    /* write value type byte */
    ret += lllyb_write(out, &byte, sizeof byte, lllybs);

    /* print value itself */
    if (value_flags & LLLY_VALUE_USER) {
        dtype = LLLY_TYPE_STRING;
    } else {
        dtype = value_type;
    }
    switch (dtype) {
    case LLLY_TYPE_BINARY:
    case LLLY_TYPE_INST:
    case LLLY_TYPE_STRING:
    case LLLY_TYPE_UNION:
    case LLLY_TYPE_IDENT:
    case LLLY_TYPE_UNKNOWN:
        /* store string */
        ret += lllyb_write_string(value_str, 0, 0, out, lllybs);
        break;
    case LLLY_TYPE_BITS:
        /* find the correct structure */
        for (; !type->info.bits.count; type = &type->der->type);

        /* store a bitfield */
        bits_i = 0;

        for (count = type->info.bits.count / 8; count; --count) {
            /* will be a full byte */
            for (byte = 0, i = 0; i < 8; ++i) {
                if (value.bit[bits_i + i]) {
                    byte |= (1 << i);
                }
            }
            ret += lllyb_write(out, &byte, sizeof byte, lllybs);
            bits_i += 8;
        }

        /* store the remainder */
        if (type->info.bits.count % 8) {
            for (byte = 0, i = 0; i < type->info.bits.count % 8; ++i) {
                if (value.bit[bits_i + i]) {
                    byte |= (1 << i);
                }
            }
            ret += lllyb_write(out, &byte, sizeof byte, lllybs);
        }
        break;
    case LLLY_TYPE_BOOL:
        /* store the whole byte */
        byte = 0;
        if (value.bln) {
            byte = 1;
        }
        ret += lllyb_write(out, &byte, sizeof byte, lllybs);
        break;
    case LLLY_TYPE_EMPTY:
        /* nothing to store */
        break;
    case LLLY_TYPE_ENUM:
        /* find the correct structure */
        for (; !type->info.enums.count; type = &type->der->type);

        /* store the enum index (save bytes if possible) */
        i = value.enm - type->info.enums.enm;
        ret += lllyb_write_enum(i, type->info.enums.count, out, lllybs);
        break;
    case LLLY_TYPE_INT8:
    case LLLY_TYPE_UINT8:
        ret += lllyb_write_number(value.uint8, 1, out, lllybs);
        break;
    case LLLY_TYPE_INT16:
    case LLLY_TYPE_UINT16:
        ret += lllyb_write_number(value.uint16, 2, out, lllybs);
        break;
    case LLLY_TYPE_INT32:
    case LLLY_TYPE_UINT32:
        ret += lllyb_write_number(value.uint32, 4, out, lllybs);
        break;
    case LLLY_TYPE_DEC64:
    case LLLY_TYPE_INT64:
    case LLLY_TYPE_UINT64:
        ret += lllyb_write_number(value.uint64, 8, out, lllybs);
        break;
    default:
        return 0;
    }

    return ret;
}

static int
lllyb_print_attributes(struct lllyout *out, struct lllyd_attr *attr, struct lllyb_state *lllybs)
{
    int r, ret = 0;
    uint8_t count;
    struct lllyd_attr *iter;
    struct lllys_type **type;

    /* count attributes */
    for (count = 0, iter = attr; iter; ++count, iter = iter->next) {
        if (count == UINT8_MAX) {
            LOGERR(lllybs->ctx, LLLY_EINT, "Maximum supported number of data node attributes is %u.", UINT8_MAX);
            return -1;
        }
    }

    /* write number of attributes on 1 byte */
    ret += (r = lllyb_write(out, &count, 1, lllybs));
    if (r < 0) {
        return -1;
    }

    /* write all the attributes */
    LLLY_TREE_FOR(attr, iter) {
        /* each attribute is a subtree */
        ret += (r = lllyb_write_start_subtree(out, lllybs));
        if (r < 0) {
            return -1;
        }

        /* model */
        ret += (r = lllyb_print_model(out, iter->annotation->module, lllybs));
        if (r < 0) {
            return -1;
        }

        /* annotation name with length */
        ret += (r = lllyb_write_string(iter->annotation->arg_value, 0, 1, out, lllybs));
        if (r < 0) {
            return -1;
        }

        /* get the type */
        type = (struct lllys_type **)lllys_ext_complex_get_substmt(LLLY_STMT_TYPE, iter->annotation, NULL);
        if (!type || !(*type)) {
            return -1;
        }

        /* attribute value */
        ret += (r = lllyb_print_value(*type, iter->value_str, iter->value, iter->value_type, iter->value_flags, 0, out, lllybs));
        if (r < 0) {
            return -1;
        }

        /* finish attribute subtree */
        ret += (r = lllyb_write_stop_subtree(out, lllybs));
        if (r < 0) {
            return -1;
        }
    }

    return ret;
}

static int
lllyb_print_schema_hash(struct lllyout *out, struct lllys_node *schema, struct hash_table **sibling_ht, struct lllyb_state *lllybs)
{
    int r, ret = 0;
    void *mem;
    uint32_t i;
    LLLYB_HASH hash;
    struct lllys_node *first_sibling, *parent;

    /* create whole sibling HT if not already created and saved */
    if (!*sibling_ht) {
        /* get first schema data sibling (or input/output) */
        for (parent = lllys_parent(schema);
             parent && (parent->nodetype & (LLLYS_USES | LLLYS_CASE | LLLYS_CHOICE));
             parent = lllys_parent(parent));

        first_sibling = (struct lllys_node *)lllys_getnext(NULL, parent, lllys_node_module(schema), 0);
        for (r = 0; r < lllybs->sib_ht_count; ++r) {
            if (lllybs->sib_ht[r].first_sibling == first_sibling) {
                /* we have already created a hash table for these siblings */
                *sibling_ht = lllybs->sib_ht[r].ht;
                break;
            }
        }

        if (!*sibling_ht) {
            /* we must create sibling hash table */
            *sibling_ht = lllyb_hash_siblings(first_sibling, NULL, 0);
            if (!*sibling_ht) {
                return -1;
            }

            /* and save it */
            ++lllybs->sib_ht_count;
            mem = realloc(lllybs->sib_ht, lllybs->sib_ht_count * sizeof *lllybs->sib_ht);
            LLLY_CHECK_ERR_RETURN(!mem, LOGMEM(lllybs->ctx), -1);
            lllybs->sib_ht = mem;

            lllybs->sib_ht[lllybs->sib_ht_count - 1].first_sibling = first_sibling;
            lllybs->sib_ht[lllybs->sib_ht_count - 1].ht = *sibling_ht;
        }
    }

    /* get our hash */
    hash = lllyb_hash_find(*sibling_ht, schema);
    if (!hash) {
        return -1;
    }

    /* write the hash */
    ret += (r = lllyb_write(out, &hash, sizeof hash, lllybs));
    if (r < 0) {
        return -1;
    }

    if (hash & LLLYB_HASH_COLLISION_ID) {
        /* no collision for this hash, we are done */
        return ret;
    }

    /* written hash was a collision, write also all the preceding hashes */
    for (i = 0; !(hash & (LLLYB_HASH_COLLISION_ID >> i)); ++i);

    for (; i; --i) {
        hash = lllyb_hash(schema, i - 1);
        if (!hash) {
            return -1;
        }
        assert(hash & (LLLYB_HASH_COLLISION_ID >> (i - 1)));

        ret += (r = lllyb_write(out, &hash, sizeof hash, lllybs));
        if (r < 0) {
            return -1;
        }
    }

    return ret;
}

static int
lllyb_print_subtree(struct lllyout *out, const struct lllyd_node *node, struct hash_table **sibling_ht, struct lllyb_state *lllybs,
                  int top_level)
{
    int r, ret = 0;
    struct lllyd_node_leaf_list *leaf;
    struct hash_table *child_ht = NULL;

    /* register a new subtree */
    ret += (r = lllyb_write_start_subtree(out, lllybs));
    if (r < 0) {
        return -1;
    }

    /*
     * write the node information
     */
    if (top_level) {
        /* write model info first */
        ret += (r = lllyb_print_model(out, lllyd_node_module(node), lllybs));
        if (r < 0) {
            return -1;
        }
    }

    ret += (r = lllyb_print_schema_hash(out, node->schema, sibling_ht, lllybs));
    if (r < 0) {
        return -1;
    }

    ret += (r = lllyb_print_attributes(out, node->attr, lllybs));
    if (r < 0) {
        return -1;
    }

    /* write node content */
    switch (node->schema->nodetype) {
    case LLLYS_CONTAINER:
    case LLLYS_LIST:
    case LLLYS_NOTIF:
    case LLLYS_RPC:
    case LLLYS_ACTION:
        /* nothing to write */
        break;
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
        leaf = (struct lllyd_node_leaf_list *)node;
        ret += (r = lllyb_print_value(&((struct lllys_node_leaf *)leaf->schema)->type, leaf->value_str, leaf->value,
                                    leaf->value_type, leaf->value_flags, leaf->dflt, out, lllybs));
        if (r < 0) {
            return -1;
        }
        break;
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        ret += (r = lllyb_print_anydata((struct lllyd_node_anydata *)node, out, lllybs));
        if (r < 0) {
            return -1;
        }
        break;
    default:
        return -1;
    }

    /* recursively write all the descendants */
    r = 0;
    if (node->schema->nodetype & (LLLYS_CONTAINER | LLLYS_LIST | LLLYS_NOTIF | LLLYS_RPC | LLLYS_ACTION)) {
        LLLY_TREE_FOR(node->child, node) {
            ret += (r = lllyb_print_subtree(out, node, &child_ht, lllybs, 0));
            if (r < 0) {
                break;
            }
        }
    }
    if (r < 0) {
        return -1;
    }

    /* finish this subtree */
    ret += (r = lllyb_write_stop_subtree(out, lllybs));
    if (r < 0) {
        return -1;
    }

    return ret;
}

int
lllyb_print_data(struct lllyout *out, const struct lllyd_node *root, int options)
{
    int r, ret = 0, rc = EXIT_SUCCESS;
    uint8_t zero = 0;
    struct hash_table *top_sibling_ht = NULL;
    const struct lllys_module *prev_mod = NULL;
    struct lllys_node *parent;
    struct lllyb_state lllybs;

    memset(&lllybs, 0, sizeof lllybs);

    if (root) {
        lllybs.ctx = lllyd_node_module(root)->ctx;

        for (parent = lllys_parent(root->schema); parent && (parent->nodetype == LLLYS_USES); parent = lllys_parent(parent));
        if (parent && (parent->nodetype != LLLYS_EXT)) {
            LOGERR(lllybs.ctx, LLLY_EINVAL, "LLLYB printer supports only printing top-level nodes.");
            return EXIT_FAILURE;
        }
    }

    /* LLLYB magic number */
    ret += (r = lllyb_print_magic_number(out));
    if (r < 0) {
        rc = EXIT_FAILURE;
        goto finish;
    }

    /* LLLYB header */
    ret += (r = lllyb_print_header(out));
    if (r < 0) {
        rc = EXIT_FAILURE;
        goto finish;
    }

    /* all used models */
    ret += (r = lllyb_print_data_models(out, root, &lllybs));
    if (r < 0) {
        rc = EXIT_FAILURE;
        goto finish;
    }

    LLLY_TREE_FOR(root, root) {
        /* do not reuse sibling hash tables from different modules */
        if (lllyd_node_module(root) != prev_mod) {
            top_sibling_ht = NULL;
            prev_mod = lllyd_node_module(root);
        }

        ret += (r = lllyb_print_subtree(out, root, &top_sibling_ht, &lllybs, 1));
        if (r < 0) {
            rc = EXIT_FAILURE;
            goto finish;
        }

        if (!(options & LLLYP_WITHSIBLINGS)) {
            break;
        }
    }

    /* ending zero byte */
    ret += (r = lllyb_write(out, &zero, sizeof zero, &lllybs));
    if (r < 0) {
        rc = EXIT_FAILURE;
    }

finish:
    free(lllybs.written);
    free(lllybs.position);
    free(lllybs.inner_chunks);
    for (r = 0; r < lllybs.sib_ht_count; ++r) {
        lllyht_free(lllybs.sib_ht[r].ht);
    }
    free(lllybs.sib_ht);

    return rc;
}
