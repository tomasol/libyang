/**
 * @file parser_lyb.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief LLLYB data parser for libyang
 *
 * Copyright (c) 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#ifdef __APPLE__
# include <libkern/OSByteOrder.h>
# define le64toh(x) OSSwapLittleToHostInt64(x)
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
# include <sys/endian.h>
#elif defined(__sun__)
# include <endian.h>
# include <sys/byteorder.h>
# if defined(_BIG_ENDIAN)
#  define le64toh(x) BSWAP_64(x)
# else
#  define le64toh(x) (x)
# endif
#else
# include <endian.h>
#endif

#include "libyang.h"
#include "common.h"
#include "context.h"
#include "parser.h"
#include "tree_internal.h"

#define LLLYB_HAVE_READ_GOTO(r, d, go) if (r < 0) goto go; d += r;
#define LLLYB_HAVE_READ_RETURN(r, d, ret) if (r < 0) return ret; d += r;

static int
lllyb_read(const char *data, uint8_t *buf, size_t count, struct lllyb_state *lllybs)
{
    int ret = 0, i, empty_chunk_i;
    size_t to_read;
    uint8_t meta_buf[LLLYB_META_BYTES];

    assert(data && lllybs);

    while (1) {
        /* check for fully-read (empty) data chunks */
        to_read = count;
        empty_chunk_i = -1;
        for (i = 0; i < lllybs->used; ++i) {
            /* we want the innermost chunks resolved first, so replace previous empty chunks,
             * also ignore chunks that are completely finished, there is nothing for us to do */
            if ((lllybs->written[i] <= to_read) && lllybs->position[i]) {
                /* empty chunk, do not read more */
                to_read = lllybs->written[i];
                empty_chunk_i = i;
            }
        }

        if ((empty_chunk_i == -1) && !count) {
            break;
        }

        /* we are actually reading some data, not just finishing another chunk */
        if (to_read) {
            if (buf) {
                memcpy(buf, data + ret, to_read);
            }

            for (i = 0; i < lllybs->used; ++i) {
                /* decrease all written counters */
                lllybs->written[i] -= to_read;
                assert(lllybs->written[i] <= LLLYB_SIZE_MAX);
            }
            /* decrease count/buf */
            count -= to_read;
            if (buf) {
                buf += to_read;
            }

            ret += to_read;
        }

        if (empty_chunk_i > -1) {
            /* read the next chunk meta information */
            memcpy(meta_buf, data + ret, LLLYB_META_BYTES);
            lllybs->written[empty_chunk_i] = meta_buf[0];
            lllybs->inner_chunks[empty_chunk_i] = meta_buf[1];

            /* remember whether there is a following chunk or not */
            lllybs->position[empty_chunk_i] = (lllybs->written[empty_chunk_i] == LLLYB_SIZE_MAX ? 1 : 0);

            ret += LLLYB_META_BYTES;
        }
    }

    return ret;
}

static int
lllyb_read_number(void *num, size_t num_size, size_t bytes, const char *data, struct lllyb_state *lllybs)
{
    int r, ret = 0;
    uint64_t buf = 0;

    ret += (r = lllyb_read(data, (uint8_t *)&buf, bytes, lllybs));
    LLLYB_HAVE_READ_RETURN(r, data, -1);

    /* correct byte order */
    buf = le64toh(buf);

    switch (num_size) {
    case 1:
        *((uint8_t *)num) = buf;
        break;
    case 2:
        *((uint16_t *)num) = buf;
        break;
    case 4:
        *((uint32_t *)num) = buf;
        break;
    case 8:
        *((uint64_t *)num) = buf;
        break;
    default:
        LOGINT(lllybs->ctx);
        return -1;
    }

    return ret;
}

static int
lllyb_read_enum(uint64_t *enum_idx, uint32_t count, const char *data, struct lllyb_state *lllybs)
{
    size_t bytes;

    if (count < (1 << 8)) {
        bytes = 1;
    } else if (count < (1 << 16)) {
        bytes = 2;
    } else if (count < (1 << 24)) {
        bytes = 3;
    } else {
        bytes = 4;
    }

    /* enum is always read into a uint64_t buffer */
    *enum_idx = 0;
    return lllyb_read_number(enum_idx, sizeof *enum_idx, bytes, data, lllybs);
}

static int
lllyb_read_string(const char *data, char **str, int with_length, struct lllyb_state *lllybs)
{
    int next_chunk = 0, r, ret = 0;
    size_t len = 0, cur_len;

    if (with_length) {
        ret += (r = lllyb_read_number(&len, sizeof len, 2, data, lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, error);
    } else {
        /* read until the end of this subtree */
        len = lllybs->written[lllybs->used - 1];
        if (lllybs->position[lllybs->used - 1]) {
            next_chunk = 1;
        }
    }

    *str = malloc((len + 1) * sizeof **str);
    LLLY_CHECK_ERR_RETURN(!*str, LOGMEM(lllybs->ctx), -1);

    ret += (r = lllyb_read(data, (uint8_t *)*str, len, lllybs));
    LLLYB_HAVE_READ_GOTO(r, data, error);

    while (next_chunk) {
        cur_len = lllybs->written[lllybs->used - 1];
        if (lllybs->position[lllybs->used - 1]) {
            next_chunk = 1;
        } else {
            next_chunk = 0;
        }

        *str = llly_realloc(*str, (len + cur_len + 1) * sizeof **str);
        LLLY_CHECK_ERR_RETURN(!*str, LOGMEM(lllybs->ctx), -1);

        ret += (r = lllyb_read(data, ((uint8_t *)*str) + len, cur_len, lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, error);

        len += cur_len;
    }

    ((char *)*str)[len] = '\0';
    return ret;

error:
    free((char *)*str);
    *str = NULL;
    return -1;
}

static void
lllyb_read_stop_subtree(struct lllyb_state *lllybs)
{
    if (lllybs->written[lllybs->used - 1]) {
        LOGINT(lllybs->ctx);
    }

    --lllybs->used;
}

static int
lllyb_read_start_subtree(const char *data, struct lllyb_state *lllybs)
{
    uint8_t meta_buf[LLLYB_META_BYTES];

    if (lllybs->used == lllybs->size) {
        lllybs->size += LLLYB_STATE_STEP;
        lllybs->written = llly_realloc(lllybs->written, lllybs->size * sizeof *lllybs->written);
        lllybs->position = llly_realloc(lllybs->position, lllybs->size * sizeof *lllybs->position);
        lllybs->inner_chunks = llly_realloc(lllybs->inner_chunks, lllybs->size * sizeof *lllybs->inner_chunks);
        LLLY_CHECK_ERR_RETURN(!lllybs->written || !lllybs->position || !lllybs->inner_chunks, LOGMEM(lllybs->ctx), -1);
    }

    memcpy(meta_buf, data, LLLYB_META_BYTES);

    ++lllybs->used;
    lllybs->written[lllybs->used - 1] = meta_buf[0];
    lllybs->inner_chunks[lllybs->used - 1] = meta_buf[LLLYB_SIZE_BYTES];
    lllybs->position[lllybs->used - 1] = (lllybs->written[lllybs->used - 1] == LLLYB_SIZE_MAX ? 1 : 0);

    return LLLYB_META_BYTES;
}

static int
lllyb_parse_model(const char *data, const struct lllys_module **mod, int options, struct lllyb_state *lllybs)
{
    int r, ret = 0;
    char *mod_name = NULL, mod_rev[11];
    uint16_t rev;

    /* model name */
    ret += (r = lllyb_read_string(data, &mod_name, 1, lllybs));
    LLLYB_HAVE_READ_GOTO(r, data, error);

    /* revision */
    ret += (r = lllyb_read_number(&rev, sizeof rev, 2, data, lllybs));
    LLLYB_HAVE_READ_GOTO(r, data, error);

    if (rev) {
        sprintf(mod_rev, "%04u-%02u-%02u", ((rev & 0xFE00) >> 9) + 2000, (rev & 0x01E0) >> 5, rev & 0x001Fu);
        *mod = llly_ctx_get_module(lllybs->ctx, mod_name, mod_rev, 0);
        if ((options & LLLYD_OPT_LYB_MOD_UPDATE) && !(*mod)) {
            /* try to use an updated module */
            *mod = llly_ctx_get_module(lllybs->ctx, mod_name, NULL, 1);
            if (*mod && (!(*mod)->implemented || !(*mod)->rev_size || (strcmp((*mod)->rev[0].date, mod_rev) < 0))) {
                /* not an implemented module in a newer revision */
                *mod = NULL;
            }
        }
    } else {
        *mod = llly_ctx_get_module(lllybs->ctx, mod_name, NULL, 0);
    }
    if (lllybs->ctx->data_clb) {
        if (!*mod) {
            *mod = lllybs->ctx->data_clb(lllybs->ctx, mod_name, NULL, 0, lllybs->ctx->data_clb_data);
        } else if (!(*mod)->implemented) {
            *mod = lllybs->ctx->data_clb(lllybs->ctx, mod_name, (*mod)->ns, LLLY_MODCLB_NOT_IMPLEMENTED, lllybs->ctx->data_clb_data);
        }
    }

    if (!*mod) {
        LOGERR(lllybs->ctx, LLLY_EINVAL, "Invalid context for LLLYB data parsing, missing module \"%s%s%s\".",
               mod_name, rev ? "@" : "", rev ? mod_rev : "");
        goto error;
    } else if (!(*mod)->implemented) {
        LOGERR(lllybs->ctx, LLLY_EINVAL, "Invalid context for LLLYB data parsing, module \"%s%s%s\" not implemented.",
               mod_name, rev ? "@" : "", rev ? mod_rev : "");
        goto error;
    }

    free(mod_name);
    return ret;

error:
    free(mod_name);
    return -1;
}

static struct lllyd_node *
lllyb_new_node(const struct lllys_node *schema)
{
    struct lllyd_node *node;

    switch (schema->nodetype) {
    case LLLYS_CONTAINER:
    case LLLYS_LIST:
    case LLLYS_NOTIF:
    case LLLYS_RPC:
    case LLLYS_ACTION:
        node = calloc(sizeof(struct lllyd_node), 1);
        break;
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
        node = calloc(sizeof(struct lllyd_node_leaf_list), 1);

        if (((struct lllys_node_leaf *)schema)->type.base == LLLY_TYPE_LEAFREF) {
            node->validity |= LLLYD_VAL_LEAFREF;
        }
        break;
    case LLLYS_ANYDATA:
    case LLLYS_ANYXML:
        node = calloc(sizeof(struct lllyd_node_anydata), 1);
        break;
    default:
        return NULL;
    }
    LLLY_CHECK_ERR_RETURN(!node, LOGMEM(schema->module->ctx), NULL);

    /* fill basic info */
    node->schema = (struct lllys_node *)schema;
    if (resolve_applies_when(schema, 0, NULL)) {
        /* this data are considered trusted so if this node exists, it means its when must have been true */
        node->when_status = LLLYD_WHEN | LLLYD_WHEN_TRUE;
    }
    node->prev = node;

    return node;
}

static int
lllyb_parse_anydata(struct lllyd_node *node, const char *data, struct lllyb_state *lllybs)
{
    int r, ret = 0;
    char *str = NULL;
    struct lllyd_node_anydata *any = (struct lllyd_node_anydata *)node;

    /* read value type */
    ret += (r = lllyb_read(data, (uint8_t *)&any->value_type, sizeof any->value_type, lllybs));
    LLLYB_HAVE_READ_RETURN(r, data, -1);

    /* read anydata content */
    if (any->value_type == LLLYD_ANYDATA_DATATREE) {
        /* invalid situation */
        LOGINT(node->schema->module->ctx);
        return -1;
    } else if (any->value_type == LLLYD_ANYDATA_LYB) {
        ret += (r = lllyb_read_string(data, &any->value.mem, 0, lllybs));
        LLLYB_HAVE_READ_RETURN(r, data, -1);
    } else {
        ret += (r = lllyb_read_string(data, &str, 0, lllybs));
        LLLYB_HAVE_READ_RETURN(r, data, -1);

        /* add to dictionary */
        any->value.str = lllydict_insert_zc(node->schema->module->ctx, str);
    }

    return ret;
}

/* generally, fill lllyd_val value union */
static int
lllyb_parse_val_1(struct lllys_type *type, LLLY_DATA_TYPE value_type, uint8_t value_flags, const char *data,
        const char **value_str, lllyd_val *value, struct lllyb_state *lllybs)
{
    int r, ret;
    size_t i;
    char *str = NULL;
    uint8_t byte;
    uint64_t num;

    if (value_flags & LLLY_VALUE_USER) {
        /* just read value_str */
        ret = lllyb_read_string(data, &str, 0, lllybs);
        if (ret > -1) {
            *value_str = lllydict_insert_zc(lllybs->ctx, str);
        }
        return ret;
    }

    /* find the correct structure, go through leafrefs and typedefs */
    switch (value_type) {
    case LLLY_TYPE_ENUM:
        for (; type->base == LLLY_TYPE_LEAFREF; type = &type->info.lref.target->type);
        for (; !type->info.enums.count; type = &type->der->type);
        break;
    case LLLY_TYPE_BITS:
        for (; type->base == LLLY_TYPE_LEAFREF; type = &type->info.lref.target->type);
        for (; !type->info.bits.count; type = &type->der->type);
        break;
    default:
        break;
    }

    switch (value_type) {
    case LLLY_TYPE_INST:
    case LLLY_TYPE_IDENT:
    case LLLY_TYPE_UNION:
        /* we do not actually fill value now, but value_str */
        ret = lllyb_read_string(data, &str, 0, lllybs);
        if (ret > -1) {
            *value_str = lllydict_insert_zc(lllybs->ctx, str);
        }
        break;
    case LLLY_TYPE_BINARY:
    case LLLY_TYPE_STRING:
    case LLLY_TYPE_UNKNOWN:
        /* read string */
        ret = lllyb_read_string(data, &str, 0, lllybs);
        if (ret > -1) {
            value->string = lllydict_insert_zc(lllybs->ctx, str);
        }
        break;
    case LLLY_TYPE_BITS:
        value->bit = calloc(type->info.bits.count, sizeof *value->bit);
        LLLY_CHECK_ERR_RETURN(!value->bit, LOGMEM(lllybs->ctx), -1);

        /* read values */
        ret = 0;
        for (i = 0; i < type->info.bits.count; ++i) {
            if (i % 8 == 0) {
                /* read another byte */
                ret += (r = lllyb_read(data + ret, &byte, sizeof byte, lllybs));
                if (r < 0) {
                    return -1;
                }
            }

            if (byte & (0x01 << (i % 8))) {
                /* bit is set */
                value->bit[i] = &type->info.bits.bit[i];
            }
        }
        break;
    case LLLY_TYPE_BOOL:
        /* read byte */
        ret = lllyb_read(data, &byte, sizeof byte, lllybs);
        if ((ret > 0) && byte) {
            value->bln = 1;
        }
        break;
    case LLLY_TYPE_EMPTY:
        /* nothing to read */
        ret = 0;
        break;
    case LLLY_TYPE_ENUM:
        num = 0;
        ret = lllyb_read_enum(&num, type->info.enums.count, data, lllybs);
        if (ret > 0) {
            assert(num < type->info.enums.count);
            value->enm = &type->info.enums.enm[num];
        }
        break;
    case LLLY_TYPE_INT8:
    case LLLY_TYPE_UINT8:
        ret = lllyb_read_number(&value->uint8, sizeof value->uint8, 1, data, lllybs);
        break;
    case LLLY_TYPE_INT16:
    case LLLY_TYPE_UINT16:
        ret = lllyb_read_number(&value->uint16, sizeof value->uint16, 2, data, lllybs);
        break;
    case LLLY_TYPE_INT32:
    case LLLY_TYPE_UINT32:
        ret = lllyb_read_number(&value->uint32, sizeof value->uint32, 4, data, lllybs);
        break;
    case LLLY_TYPE_DEC64:
    case LLLY_TYPE_INT64:
    case LLLY_TYPE_UINT64:
        ret = lllyb_read_number(&value->uint64, sizeof value->uint64, 8, data, lllybs);
        break;
    default:
        return -1;
    }

    return ret;
}

/* generally, fill value_str */
static int
lllyb_parse_val_2(struct lllys_type *type, struct lllyd_node_leaf_list *leaf, struct lllyd_attr *attr, struct unres_data *unres)
{
    struct llly_ctx *ctx;
    struct lllys_module *mod;
    struct lllys_type *rtype = NULL;
    char num_str[22], *str;
    int64_t frac, num;
    uint32_t i, str_len;
    uint8_t *value_flags, dig;
    const char **value_str;
    LLLY_DATA_TYPE value_type;
    lllyd_val *value;

    if (leaf) {
        ctx = leaf->schema->module->ctx;
        mod = lllys_node_module(leaf->schema);

        value = &leaf->value;
        value_str = &leaf->value_str;
        value_flags = &leaf->value_flags;
        value_type = leaf->value_type;
    } else {
        ctx = attr->annotation->module->ctx;
        mod = lllys_main_module(attr->annotation->module);

        value = &attr->value;
        value_str = &attr->value_str;
        value_flags = &attr->value_flags;
        value_type = attr->value_type;
    }

    if (*value_flags & LLLY_VALUE_USER) {
        /* unfortunately, we need to also fill the value properly, so just parse it again */
        *value_flags &= ~LLLY_VALUE_USER;
        if (!lllyp_parse_value(type, value_str, NULL, leaf, attr, NULL, 1, (leaf ? leaf->dflt : 0), 1)) {
            return -1;
        }

        if (!(*value_flags & LLLY_VALUE_USER)) {
            LOGWRN(ctx, "Value \"%s\" was stored as a user type, but it is not in the current context.", value_str);
        }
        return 0;
    }

    /* we are parsing leafref/ptr union stored as the target type,
     * so we first parse it into string and then resolve the leafref/ptr union */
    if ((type->base == LLLY_TYPE_LEAFREF) || (type->base == LLLY_TYPE_INST)
            || ((type->base == LLLY_TYPE_UNION) && type->info.uni.has_ptr_type)) {
        if ((value_type == LLLY_TYPE_INST) || (value_type == LLLY_TYPE_IDENT) || (value_type == LLLY_TYPE_UNION)) {
            /* we already have a string */
            goto parse_reference;
        }
    }

    /* find the correct structure, go through leafrefs and typedefs */
    switch (value_type) {
    case LLLY_TYPE_BITS:
        for (rtype = type; rtype->base == LLLY_TYPE_LEAFREF; rtype = &rtype->info.lref.target->type);
        for (; !rtype->info.bits.count; rtype = &rtype->der->type);
        break;
    case LLLY_TYPE_DEC64:
        for (rtype = type; rtype->base == LLLY_TYPE_LEAFREF; rtype = &type->info.lref.target->type);
        break;
    default:
        break;
    }

    switch (value_type) {
    case LLLY_TYPE_IDENT:
        /* fill the identity pointer now */
        value->ident = resolve_identref(type, *value_str, (struct lllyd_node *)leaf, mod, (leaf ? leaf->dflt : 0));
        if (!value->ident) {
            return -1;
        }
        break;
    case LLLY_TYPE_INST:
        /* unresolved instance-identifier, keep value NULL */
        value->instance = NULL;
        break;
    case LLLY_TYPE_BINARY:
    case LLLY_TYPE_STRING:
    case LLLY_TYPE_UNKNOWN:
        /* just re-assign it */
        *value_str = value->string;
        break;
    case LLLY_TYPE_BITS:
        /* print the set bits */
        str = malloc(1);
        LLLY_CHECK_ERR_RETURN(!str, LOGMEM(ctx), -1);
        str[0] = '\0';
        str_len = 0;
        for (i = 0; i < rtype->info.bits.count; ++i) {
            if (value->bit[i]) {
                str = llly_realloc(str, str_len + strlen(value->bit[i]->name) + (str_len ? 1 : 0) + 1);
                LLLY_CHECK_ERR_RETURN(!str, LOGMEM(ctx), -1);

                str_len += sprintf(str + str_len, "%s%s", str_len ? " " : "", value->bit[i]->name);
            }
        }

        *value_str = lllydict_insert_zc(ctx, str);
        break;
    case LLLY_TYPE_BOOL:
        *value_str = lllydict_insert(ctx, (value->bln ? "true" : "false"), 0);
        break;
    case LLLY_TYPE_EMPTY:
        *value_str = lllydict_insert(ctx, "", 0);
        break;
    case LLLY_TYPE_UNION:
        if (attr) {
            /* we do not support union type attribute */
            LOGINT(ctx);
            return -1;
        }

        if (resolve_union(leaf, type, 1, 2, NULL)) {
            return -1;
        }
        break;
    case LLLY_TYPE_ENUM:
        /* print the value */
        *value_str = lllydict_insert(ctx, value->enm->name, 0);
        break;
    case LLLY_TYPE_INT8:
        sprintf(num_str, "%d", value->int8);
        *value_str = lllydict_insert(ctx, num_str, 0);
        break;
    case LLLY_TYPE_UINT8:
        sprintf(num_str, "%u", value->uint8);
        *value_str = lllydict_insert(ctx, num_str, 0);
        break;
    case LLLY_TYPE_INT16:
        sprintf(num_str, "%d", value->int16);
        *value_str = lllydict_insert(ctx, num_str, 0);
        break;
    case LLLY_TYPE_UINT16:
        sprintf(num_str, "%u", value->uint16);
        *value_str = lllydict_insert(ctx, num_str, 0);
        break;
    case LLLY_TYPE_INT32:
        sprintf(num_str, "%d", value->int32);
        *value_str = lllydict_insert(ctx, num_str, 0);
        break;
    case LLLY_TYPE_UINT32:
        sprintf(num_str, "%u", value->uint32);
        *value_str = lllydict_insert(ctx, num_str, 0);
        break;
    case LLLY_TYPE_INT64:
        sprintf(num_str, "%"PRId64, value->int64);
        *value_str = lllydict_insert(ctx, num_str, 0);
        break;
    case LLLY_TYPE_UINT64:
        sprintf(num_str, "%"PRIu64, value->uint64);
        *value_str = lllydict_insert(ctx, num_str, 0);
        break;
    case LLLY_TYPE_DEC64:
        num = value->dec64 / (int64_t)rtype->info.dec64.div;
        frac = value->dec64 % (int64_t)rtype->info.dec64.div;
        dig = rtype->info.dec64.dig;

        /* frac should always be positive, remove trailing zeros */
        if (frac < 0) {
            frac *= -1;
        }
        while ((dig > 1) && !(frac % 10)) {
            frac /= 10;
            --dig;
        }

        /* handle special case of int64_t not supporting printing -0 */
        sprintf(num_str, "%s%"PRId64".%.*"PRId64, (num == 0) && (value->dec64 < 0) ? "-" : "", num, dig, frac);
        *value_str = lllydict_insert(ctx, num_str, 0);
        break;
    default:
        return -1;
    }

    if ((type->base == LLLY_TYPE_LEAFREF) || (type->base == LLLY_TYPE_INST)
            || ((type->base == LLLY_TYPE_UNION) && type->info.uni.has_ptr_type)) {
parse_reference:
        assert(*value_str);

        if (attr) {
            /* we do not support reference types of attributes */
            LOGINT(ctx);
            return -1;
        }

        if (type->base == LLLY_TYPE_INST) {
            if (unres_data_add(unres, (struct lllyd_node *)leaf, UNRES_INSTID)) {
                return -1;
            }
        } else if (type->base == LLLY_TYPE_LEAFREF) {
            if (unres_data_add(unres, (struct lllyd_node *)leaf, UNRES_LEAFREF)) {
                return -1;
            }
        } else {
            if (unres_data_add(unres, (struct lllyd_node *)leaf, UNRES_UNION)) {
                return -1;
            }
        }
    }

    return 0;
}

static int
lllyb_parse_value(struct lllys_type *type, struct lllyd_node_leaf_list *leaf, struct lllyd_attr *attr, const char *data,
                struct unres_data *unres, struct lllyb_state *lllybs)
{
    int r, ret = 0;
    uint8_t start_byte;

    const char **value_str;
    lllyd_val *value;
    LLLY_DATA_TYPE *value_type;
    uint8_t *value_flags;

    assert((leaf || attr) && (!leaf || !attr));

    if (leaf) {
        value_str = &leaf->value_str;
        value = &leaf->value;
        value_type = &leaf->value_type;
        value_flags = &leaf->value_flags;
    } else {
        value_str = &attr->value_str;
        value = &attr->value;
        value_type = &attr->value_type;
        value_flags = &attr->value_flags;
    }

    /* read value type and flags on the first byte */
    ret += (r = lllyb_read(data, &start_byte, sizeof start_byte, lllybs));
    LLLYB_HAVE_READ_RETURN(r, data, -1);

    /* fill value type, flags */
    *value_type = start_byte & 0x1F;
    if (start_byte & 0x80) {
        assert(leaf);
        leaf->dflt = 1;
    }
    if (start_byte & 0x40) {
        *value_flags |= LLLY_VALUE_USER;
    }
    if (start_byte & 0x20) {
        *value_flags |= LLLY_VALUE_UNRES;
    }

    ret += (r = lllyb_parse_val_1(type, *value_type, *value_flags, data, value_str, value, lllybs));
    LLLYB_HAVE_READ_RETURN(r, data, -1);

    /* union is handled specially */
    if ((type->base == LLLY_TYPE_UNION) && !(*value_flags & LLLY_VALUE_USER)) {
        assert(*value_type == LLLY_TYPE_STRING);

        *value_str = value->string;
        value->string = NULL;
        *value_type = LLLY_TYPE_UNION;
    }

    ret += (r = lllyb_parse_val_2(type, leaf, attr, unres));
    LLLYB_HAVE_READ_RETURN(r, data, -1);

    return ret;
}

static int
lllyb_parse_attr_name(const struct lllys_module *mod, const char *data, struct lllys_ext_instance_complex **ext, int options,
                    struct lllyb_state *lllybs)
{
    int r, ret = 0, pos, i, j, k;
    const struct lllys_submodule *submod = NULL;
    char *attr_name = NULL;

    /* attr name */
    ret += (r = lllyb_read_string(data, &attr_name, 1, lllybs));
    LLLYB_HAVE_READ_RETURN(r, data, -1);

    /* search module */
    pos = -1;
    for (i = 0, j = 0; i < mod->ext_size; i = i + j + 1) {
        j = lllys_ext_instance_presence(&mod->ctx->models.list[0]->extensions[0], &mod->ext[i], mod->ext_size - i);
        if (j == -1) {
            break;
        }
        if (llly_strequal(mod->ext[i + j]->arg_value, attr_name, 0)) {
            pos = i + j;
            break;
        }
    }

    /* try submodules */
    if (pos == -1) {
        for (k = 0; k < mod->inc_size; ++k) {
            submod = mod->inc[k].submodule;
            for (i = 0, j = 0; i < submod->ext_size; i = i + j + 1) {
                j = lllys_ext_instance_presence(&mod->ctx->models.list[0]->extensions[0], &submod->ext[i], submod->ext_size - i);
                if (j == -1) {
                    break;
                }
                if (llly_strequal(submod->ext[i + j]->arg_value, attr_name, 0)) {
                    pos = i + j;
                    break;
                }
            }
        }
    }

    if (pos == -1) {
        *ext = NULL;
    } else {
        *ext = submod ? (struct lllys_ext_instance_complex *)submod->ext[pos] : (struct lllys_ext_instance_complex *)mod->ext[pos];
    }

    if (!*ext && (options & LLLYD_OPT_STRICT)) {
        LOGVAL(mod->ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Failed to find annotation \"%s\" in \"%s\".", attr_name, mod->name);
        free(attr_name);
        return -1;
    }

    free(attr_name);
    return ret;
}

static int
lllyb_parse_attributes(struct lllyd_node *node, const char *data, int options, struct unres_data *unres, struct lllyb_state *lllybs)
{
    int r, ret = 0;
    uint8_t i, count = 0;
    const struct lllys_module *mod;
    struct lllys_type **type;
    struct lllyd_attr *attr = NULL;
    struct lllys_ext_instance_complex *ext;

    /* read number of attributes stored */
    ret += (r = lllyb_read(data, &count, 1, lllybs));
    LLLYB_HAVE_READ_GOTO(r, data, error);

    /* read attributes */
    for (i = 0; i < count; ++i) {
        ret += (r = lllyb_read_start_subtree(data, lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, error);

        /* find model */
        ret += (r = lllyb_parse_model(data, &mod, options, lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, error);

        if (mod) {
            /* annotation name */
            ret += (r = lllyb_parse_attr_name(mod, data, &ext, options, lllybs));
            LLLYB_HAVE_READ_GOTO(r, data, error);
        }

        if (!mod || !ext) {
            /* unknown attribute, skip it */
            do {
                ret += (r = lllyb_read(data, NULL, lllybs->written[lllybs->used - 1], lllybs));
                LLLYB_HAVE_READ_GOTO(r, data, error);
            } while (lllybs->written[lllybs->used - 1]);
            goto stop_subtree;
        }

        /* allocate new attribute */
        if (!attr) {
            assert(!node->attr);

            attr = calloc(1, sizeof *attr);
            LLLY_CHECK_ERR_GOTO(!attr, LOGMEM(lllybs->ctx), error);

            node->attr = attr;
        } else {
            attr->next = calloc(1, sizeof *attr);
            LLLY_CHECK_ERR_GOTO(!attr->next, LOGMEM(lllybs->ctx), error);

            attr = attr->next;
        }

        /* attribute annotation */
        attr->annotation = ext;

        /* attribute name */
        attr->name = lllydict_insert(lllybs->ctx, attr->annotation->arg_value, 0);

        /* get the type */
        type = (struct lllys_type **)lllys_ext_complex_get_substmt(LLLY_STMT_TYPE, attr->annotation, NULL);
        if (!type || !(*type)) {
            goto error;
        }

        /* attribute value */
        ret += (r = lllyb_parse_value(*type, NULL, attr, data, unres, lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, error);

stop_subtree:
        lllyb_read_stop_subtree(lllybs);
    }

    return ret;

error:
    lllyd_free_attr(lllybs->ctx, node, node->attr, 1);
    return -1;
}

static int
lllyb_is_schema_hash_match(struct lllys_node *sibling, LLLYB_HASH *hash, uint8_t hash_count)
{
    LLLYB_HASH sibling_hash;
    uint8_t i;

    /* compare all the hashes starting from collision ID 0 */
    for (i = 0; i < hash_count; ++i) {
        sibling_hash = lllyb_hash(sibling, i);
        if (sibling_hash != hash[i]) {
            return 0;
        }
    }

    return 1;
}

static int
lllyb_parse_schema_hash(const struct lllys_node *sparent, const struct lllys_module *mod, const char *data, const char *yang_data_name,
                      int options, struct lllys_node **snode, struct lllyb_state *lllybs)
{
    int r, ret = 0;
    uint8_t i, j;
    struct lllys_node *sibling;
    LLLYB_HASH hash[LLLYB_HASH_BITS - 1];

    assert((sparent || mod) && (!sparent || !mod));

    /* read the first hash */
    ret += (r = lllyb_read(data, &hash[0], sizeof *hash, lllybs));
    LLLYB_HAVE_READ_RETURN(r, data, -1);

    /* based on the first hash read all the other ones, if any */
    for (i = 0; !(hash[0] & (LLLYB_HASH_COLLISION_ID >> i)); ++i) {
        if (i > LLLYB_HASH_BITS) {
            return -1;
        }
    }

    /* move the first hash on its accurate position */
    hash[i] = hash[0];

    /* read the rest of hashes */
    for (j = i; j; --j) {
        ret += (r = lllyb_read(data, &hash[j - 1], sizeof *hash, lllybs));
        LLLYB_HAVE_READ_RETURN(r, data, -1);

        /* correct collision ID */
        assert(hash[j - 1] & (LLLYB_HASH_COLLISION_ID >> (j - 1)));
        /* preceded with zeros */
        assert(!(hash[j - 1] & (LLLYB_HASH_MASK << (LLLYB_HASH_BITS - (j - 1)))));
    }

    /* handle yang data templates */
    if ((options & LLLYD_OPT_DATA_TEMPLATE) && yang_data_name && mod) {
        sparent = lllyp_get_yang_data_template(mod, yang_data_name, strlen(yang_data_name));
        if (!sparent) {
            sibling = NULL;
            goto finish;
        }
    }

    /* handle RPC/action input/output */
    if (sparent && (sparent->nodetype & (LLLYS_RPC | LLLYS_ACTION))) {
        sibling = NULL;
        while ((sibling = (struct lllys_node *)lllys_getnext(sibling, sparent, NULL, LLLYS_GETNEXT_WITHINOUT))) {
            if ((sibling->nodetype == LLLYS_INPUT) && (options & LLLYD_OPT_RPC)) {
                break;
            }
            if ((sibling->nodetype == LLLYS_OUTPUT) && (options & LLLYD_OPT_RPCREPLY)) {
                break;
            }
        }
        if (!sibling) {
            /* fail */
            goto finish;
        }

        /* use only input/output children nodes */
        sparent = sibling;
    }

    /* find our node with matching hashes */
    sibling = NULL;
    while ((sibling = (struct lllys_node *)lllys_getnext(sibling, sparent, mod, 0))) {
        /* skip schema nodes from models not present during printing */
        if (lllyb_has_schema_model(sibling, lllybs->models, lllybs->mod_count) && lllyb_is_schema_hash_match(sibling, hash, i + 1)) {
            /* match found */
            break;
        }
    }

finish:
    *snode = sibling;
    if (!sibling && (options & LLLYD_OPT_STRICT)) {
        if (mod) {
            LOGVAL(lllybs->ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Failed to find matching hash for a top-level node from \"%s\".",
                    mod->name);
        } else {
            LOGVAL(lllybs->ctx, LLLYE_SPEC, LLLY_VLOG_LYS, sparent, "Failed to find matching hash for a child of \"%s\".",
                    sparent->name);
        }
        return -1;
    }

    return ret;
}

static int
lllyb_skip_subtree(const char *data, struct lllyb_state *lllybs)
{
    int r, ret = 0;

    do {
        /* first skip any meta information inside */
        r = lllybs->inner_chunks[lllybs->used - 1] * LLLYB_META_BYTES;
        data += r;
        ret += r;

        /* then read data */
        ret += (r = lllyb_read(data, NULL, lllybs->written[lllybs->used - 1], lllybs));
        LLLYB_HAVE_READ_RETURN(r, data, -1);
    } while (lllybs->written[lllybs->used - 1]);

    return ret;
}

static int
lllyb_parse_subtree(const char *data, struct lllyd_node *parent, struct lllyd_node **first_sibling, const char *yang_data_name,
        int options, struct unres_data *unres, struct lllyb_state *lllybs)
{
    int r, ret = 0;
    struct lllyd_node *node = NULL, *iter;
    const struct lllys_module *mod;
    struct lllys_node *snode;

    assert((parent && !first_sibling) || (!parent && first_sibling));

    /* register a new subtree */
    ret += (r = lllyb_read_start_subtree(data, lllybs));
    LLLYB_HAVE_READ_GOTO(r, data, error);

    if (!parent) {
        /* top-level, read module name */
        ret += (r = lllyb_parse_model(data, &mod, options, lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, error);

        if (mod) {
            /* read hash, find the schema node starting from mod, possibly yang_data_name */
            r = lllyb_parse_schema_hash(NULL, mod, data, yang_data_name, options, &snode, lllybs);
        }
    } else {
        mod = lllyd_node_module(parent);

        /* read hash, find the schema node starting from parent schema */
        r = lllyb_parse_schema_hash(parent->schema, NULL, data, NULL, options, &snode, lllybs);
    }
    ret += r;
    LLLYB_HAVE_READ_GOTO(r, data, error);

    if (!mod || !snode) {
        /* unknown data subtree, skip it whole */
        ret += (r = lllyb_skip_subtree(data, lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, error);
        goto stop_subtree;
    }

    /*
     * read the node
     */
    node = lllyb_new_node(snode);
    if (!node) {
        goto error;
    }

    ret += (r = lllyb_parse_attributes(node, data, options, unres, lllybs));
    LLLYB_HAVE_READ_GOTO(r, data, error);

    /* read node content */
    switch (snode->nodetype) {
    case LLLYS_CONTAINER:
    case LLLYS_LIST:
    case LLLYS_NOTIF:
    case LLLYS_RPC:
    case LLLYS_ACTION:
        /* nothing to read */
        break;
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
        ret += (r = lllyb_parse_value(&((struct lllys_node_leaf *)node->schema)->type, (struct lllyd_node_leaf_list *)node,
                                    NULL, data, unres, lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, error);
        break;
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        ret += (r = lllyb_parse_anydata(node, data, lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, error);
        break;
    default:
        goto error;
    }

    /* insert into data tree, manually */
    if (parent) {
        if (!parent->child) {
            /* only child */
            parent->child = node;
        } else {
            /* last child */
            parent->child->prev->next = node;
            node->prev = parent->child->prev;
            parent->child->prev = node;
        }
        node->parent = parent;
    } else if (*first_sibling) {
        /* last sibling */
        (*first_sibling)->prev->next = node;
        node->prev = (*first_sibling)->prev;
        (*first_sibling)->prev = node;
    } else {
        /* only sibling */
        *first_sibling = node;
    }

    /* read all descendants */
    while (lllybs->written[lllybs->used - 1]) {
        ret += (r = lllyb_parse_subtree(data, node, NULL, NULL, options, unres, lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, error);
    }

    /* make containers default if should be */
    if ((node->schema->nodetype == LLLYS_CONTAINER) && !((struct lllys_node_container *)node->schema)->presence) {
        LLLY_TREE_FOR(node->child, iter) {
            if (!iter->dflt) {
                break;
            }
        }

        if (!iter) {
            node->dflt = 1;
        }
    }

#ifdef LLLY_ENABLED_CACHE
    /* calculate the hash and insert it into parent (list with keys is handled when its keys are inserted) */
    if ((node->schema->nodetype != LLLYS_LIST) || !((struct lllys_node_list *)node->schema)->keys_size) {
        lllyd_hash(node);
        lllyd_insert_hash(node);
    }
#endif

stop_subtree:
    /* end the subtree */
    lllyb_read_stop_subtree(lllybs);

    return ret;

error:
    lllyd_free(node);
    if (first_sibling && (*first_sibling == node)) {
        *first_sibling = NULL;
    }
    return -1;
}

static int
lllyb_parse_data_models(const char *data, int options, struct lllyb_state *lllybs)
{
    int i, r, ret = 0;

    /* read model count */
    ret += (r = lllyb_read_number(&lllybs->mod_count, sizeof lllybs->mod_count, 2, data, lllybs));
    LLLYB_HAVE_READ_RETURN(r, data, -1);

    if (lllybs->mod_count) {
        lllybs->models = malloc(lllybs->mod_count * sizeof *lllybs->models);
        LLLY_CHECK_ERR_RETURN(!lllybs->models, LOGMEM(lllybs->ctx), -1);

        /* read modules */
        for (i = 0; i < lllybs->mod_count; ++i) {
            ret += (r = lllyb_parse_model(data, &lllybs->models[i], options, lllybs));
            LLLYB_HAVE_READ_RETURN(r, data, -1);
        }
    }

    return ret;
}

static int
lllyb_parse_magic_number(const char *data, struct lllyb_state *lllybs)
{
    int r, ret = 0;
    char magic_byte = 0;

    ret += (r = lllyb_read(data, (uint8_t *)&magic_byte, 1, lllybs));
    LLLYB_HAVE_READ_RETURN(r, data, -1);
    if (magic_byte != 'l') {
        LOGERR(lllybs->ctx, LLLY_EINVAL, "Invalid first magic number byte \"0x%02x\".", magic_byte);
        return -1;
    }

    ret += (r = lllyb_read(data, (uint8_t *)&magic_byte, 1, lllybs));
    LLLYB_HAVE_READ_RETURN(r, data, -1);
    if (magic_byte != 'y') {
        LOGERR(lllybs->ctx, LLLY_EINVAL, "Invalid second magic number byte \"0x%02x\".", magic_byte);
        return -1;
    }

    ret += (r = lllyb_read(data, (uint8_t *)&magic_byte, 1, lllybs));
    LLLYB_HAVE_READ_RETURN(r, data, -1);
    if (magic_byte != 'b') {
        LOGERR(lllybs->ctx, LLLY_EINVAL, "Invalid third magic number byte \"0x%02x\".", magic_byte);
        return -1;
    }

    return ret;
}

static int
lllyb_parse_header(const char *data, struct lllyb_state *lllybs)
{
    int ret = 0;
    uint8_t byte = 0;

    /* TODO version, any flags? */
    ret += lllyb_read(data, (uint8_t *)&byte, sizeof byte, lllybs);

    return ret;
}

struct lllyd_node *
lllyd_parse_lyb(struct llly_ctx *ctx, const char *data, int options, const struct lllyd_node *data_tree,
              const char *yang_data_name, int *parsed)
{
    int r = 0, ret = 0;
    struct lllyd_node *node = NULL, *next, *act_notif = NULL;
    struct unres_data *unres = NULL;
    struct lllyb_state lllybs;

    if (!ctx || !data) {
        LOGARG;
        return NULL;
    }

    lllybs.written = malloc(LLLYB_STATE_STEP * sizeof *lllybs.written);
    lllybs.position = malloc(LLLYB_STATE_STEP * sizeof *lllybs.position);
    lllybs.inner_chunks = malloc(LLLYB_STATE_STEP * sizeof *lllybs.inner_chunks);
    LLLY_CHECK_ERR_GOTO(!lllybs.written || !lllybs.position || !lllybs.inner_chunks, LOGMEM(ctx), finish);
    lllybs.used = 0;
    lllybs.size = LLLYB_STATE_STEP;
    lllybs.models = NULL;
    lllybs.mod_count = 0;
    lllybs.ctx = ctx;

    unres = calloc(1, sizeof *unres);
    LLLY_CHECK_ERR_GOTO(!unres, LOGMEM(ctx), finish);

    /* read magic number */
    ret += (r = lllyb_parse_magic_number(data, &lllybs));
    LLLYB_HAVE_READ_GOTO(r, data, finish);

    /* read header */
    ret += (r = lllyb_parse_header(data, &lllybs));
    LLLYB_HAVE_READ_GOTO(r, data, finish);

    /* read used models */
    ret += (r = lllyb_parse_data_models(data, options, &lllybs));
    LLLYB_HAVE_READ_GOTO(r, data, finish);

    /* read subtree(s) */
    while (data[0]) {
        ret += (r = lllyb_parse_subtree(data, NULL, &node, yang_data_name, options, unres, &lllybs));
        if (r < 0) {
            lllyd_free_withsiblings(node);
            node = NULL;
            goto finish;
        }
        data += r;
    }

    /* read the last zero, parsing finished */
    ++ret;
    r = ret;

    if (options & LLLYD_OPT_DATA_ADD_YANGLIB) {
        if (lllyd_merge(node, llly_ctx_info(ctx), LLLYD_OPT_DESTRUCT | LLLYD_OPT_EXPLICIT)) {
            LOGERR(ctx, LLLY_EINT, "Adding ietf-yang-library data failed.");
            lllyd_free_withsiblings(node);
            node = NULL;
            goto finish;
        }
    }

    /* resolve any unresolved instance-identifiers */
    if (unres->count) {
        if (options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY | LLLYD_OPT_NOTIF)) {
            LLLY_TREE_DFS_BEGIN(node, next, act_notif) {
                if (act_notif->schema->nodetype & (LLLYS_RPC | LLLYS_ACTION | LLLYS_NOTIF)) {
                    break;
                }
                LLLY_TREE_DFS_END(node, next, act_notif);
            }
        }
        if (lllyd_defaults_add_unres(&node, options, ctx, NULL, 0, data_tree, act_notif, unres, 0)) {
            lllyd_free_withsiblings(node);
            node = NULL;
            goto finish;
        }
    }

finish:
    free(lllybs.written);
    free(lllybs.position);
    free(lllybs.inner_chunks);
    free(lllybs.models);
    if (unres) {
        free(unres->node);
        free(unres->type);
        free(unres);
    }

    if (parsed) {
        *parsed = r;
    }
    return node;
}

API int
lllyd_lyb_data_length(const char *data)
{
    FUN_IN;

    struct lllyb_state lllybs;
    int r = 0, ret = 0, i;
    size_t len;
    uint8_t buf[LLLYB_SIZE_MAX];

    if (!data) {
        return -1;
    }

    lllybs.written = malloc(LLLYB_STATE_STEP * sizeof *lllybs.written);
    lllybs.position = malloc(LLLYB_STATE_STEP * sizeof *lllybs.position);
    lllybs.inner_chunks = malloc(LLLYB_STATE_STEP * sizeof *lllybs.inner_chunks);
    LLLY_CHECK_ERR_GOTO(!lllybs.written || !lllybs.position || !lllybs.inner_chunks, LOGMEM(NULL), finish);
    lllybs.used = 0;
    lllybs.size = LLLYB_STATE_STEP;
    lllybs.models = NULL;
    lllybs.mod_count = 0;
    lllybs.ctx = NULL;

    /* read magic number */
    ret += (r = lllyb_parse_magic_number(data, &lllybs));
    LLLYB_HAVE_READ_GOTO(r, data, finish);

    /* read header */
    ret += (r = lllyb_parse_header(data, &lllybs));
    LLLYB_HAVE_READ_GOTO(r, data, finish);

    /* read model count */
    ret += (r = lllyb_read_number(&lllybs.mod_count, sizeof lllybs.mod_count, 2, data, &lllybs));
    LLLYB_HAVE_READ_GOTO(r, data, finish);

    /* read all models */
    for (i = 0; i < lllybs.mod_count; ++i) {
        /* module name length */
        len = 0;
        ret += (r = lllyb_read_number(&len, sizeof len, 2, data, &lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, finish);

        /* model name */
        ret += (r = lllyb_read(data, buf, len, &lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, finish);

        /* revision */
        ret += (r = lllyb_read(data, buf, 2, &lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, finish);
    }

    while (data[0]) {
        /* register a new subtree */
        ret += (r = lllyb_read_start_subtree(data, &lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, finish);

        /* skip it */
        ret += (r = lllyb_skip_subtree(data, &lllybs));
        LLLYB_HAVE_READ_GOTO(r, data, finish);

        /* subtree finished */
        lllyb_read_stop_subtree(&lllybs);
    }

    /* read the last zero, parsing finished */
    ++ret;

finish:
    free(lllybs.written);
    free(lllybs.position);
    free(lllybs.inner_chunks);
    free(lllybs.models);
    return ret;
}
