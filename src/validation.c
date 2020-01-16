/**
 * @file validation.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Data tree validation functions
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "validation.h"
#include "libyang.h"
#include "xpath.h"
#include "parser.h"
#include "resolve.h"
#include "tree_internal.h"
#include "xml_internal.h"

static int
lllyv_keys(const struct lllyd_node *list)
{
    struct lllyd_node *child;
    struct lllys_node_list *schema = (struct lllys_node_list *)list->schema; /* shortcut */
    int i;

    for (i = 0, child = list->child; i < schema->keys_size; i++, child = child->next) {
        if (!child || child->schema != (struct lllys_node *)schema->keys[i]) {
            /* key not found on the correct place */
            LOGVAL(schema->module->ctx, LLLYE_MISSELEM, LLLY_VLOG_LYD, list, schema->keys[i]->name, schema->name);
            for ( ; child; child = child->next) {
                if (child->schema == (struct lllys_node *)schema->keys[i]) {
                    LOGVAL(schema->module->ctx, LLLYE_SPEC, LLLY_VLOG_LYD, child, "Invalid position of the key element.");
                    break;
                }
            }
            return 1;
        }
    }
    return 0;
}

int
lllyv_data_context(const struct lllyd_node *node, int options, struct unres_data *unres)
{
    const struct lllys_node *siter = NULL;
    struct lllys_node *sparent, *op;
    struct lllyd_node_leaf_list *leaf = (struct lllyd_node_leaf_list *)node;
    struct llly_ctx *ctx = node->schema->module->ctx;

    assert(node);
    assert(unres);

    /* check if the node instance is enabled by if-feature */
    if (lllys_is_disabled(node->schema, 2)) {
        LOGVAL(ctx, LLLYE_INELEM, LLLY_VLOG_LYD, node, node->schema->name);
        return 1;
    }

    /* find (nested) operation node */
    for (op = node->schema; op && !(op->nodetype & (LLLYS_NOTIF | LLLYS_RPC | LLLYS_ACTION)); op = lllys_parent(op));

    if (!(options & (LLLYD_OPT_NOTIF_FILTER | LLLYD_OPT_EDIT | LLLYD_OPT_GET | LLLYD_OPT_GETCONFIG))
            && (!(options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY | LLLYD_OPT_NOTIF)) || op)) {
        if (node->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) {
            /* if union with leafref/intsid, leafref itself (invalid) or instance-identifier, store the node for later resolving */
            if ((((struct lllys_node_leaf *)leaf->schema)->type.base == LLLY_TYPE_UNION)
                    && ((struct lllys_node_leaf *)leaf->schema)->type.info.uni.has_ptr_type) {
                if (unres_data_add(unres, (struct lllyd_node *)node, UNRES_UNION)) {
                    return 1;
                }
            } else if ((((struct lllys_node_leaf *)leaf->schema)->type.base == LLLY_TYPE_LEAFREF)
                    && ((leaf->validity & LLLYD_VAL_LEAFREF) || (leaf->value_flags & LLLY_VALUE_UNRES))) {
                /* always retry validation on unres leafrefs, if again not possible, the correct flags should
                 * be set and the leafref will be kept unresolved */
                leaf->value_flags &= ~LLLY_VALUE_UNRES;
                leaf->validity |= LLLYD_VAL_LEAFREF;

                if (unres_data_add(unres, (struct lllyd_node *)node, UNRES_LEAFREF)) {
                    return 1;
                }
            } else if (((struct lllys_node_leaf *)leaf->schema)->type.base == LLLY_TYPE_INST) {
                if (unres_data_add(unres, (struct lllyd_node *)node, UNRES_INSTID)) {
                    return 1;
                }
            }
        }

        /* check all relevant when conditions */
        if (node->when_status & LLLYD_WHEN) {
            if (unres_data_add(unres, (struct lllyd_node *)node, UNRES_WHEN)) {
                return 1;
            }
        }
    } else if (node->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) {
        /* just remove the flag if it was set */
        leaf->validity &= ~LLLYD_VAL_LEAFREF;
    }

    /* check for (non-)presence of status data in edit-config data */
    if ((options & (LLLYD_OPT_EDIT | LLLYD_OPT_GETCONFIG | LLLYD_OPT_CONFIG)) && (node->schema->flags & LLLYS_CONFIG_R)) {
        LOGVAL(ctx, LLLYE_INELEM, LLLY_VLOG_LYD, node, node->schema->name);
        return 1;
    }

    /* check elements order in case of RPC's input and output */
    if (!(options & (LLLYD_OPT_TRUSTED | LLLYD_OPT_NOTIF_FILTER)) && (options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY))
            && (node->validity & LLLYD_VAL_MAND) && op) {
        if ((node->prev != node) && node->prev->next) {
            /* find schema data parent */
            for (sparent = lllys_parent(node->schema);
                    sparent && (sparent->nodetype & (LLLYS_USES | LLLYS_CHOICE | LLLYS_CASE));
                    sparent = lllys_parent(sparent));
            for (siter = lllys_getnext(node->schema, sparent, lllyd_node_module(node), 0);
                    siter;
                    siter = lllys_getnext(siter, sparent, lllyd_node_module(node), 0)) {
                if (siter == node->prev->schema) {
                    /* data predecessor has the schema node after
                     * the schema node of the data node being checked */
                    LOGVAL(ctx, LLLYE_INORDER, LLLY_VLOG_LYD, node, node->schema->name, siter->name);
                    return 1;
                }
            }

        }
    }

    return 0;
}

/*
 * actions (cb_data):
 * 0  - compare all uniques
 * n  - compare n-th unique
 */
static int
lllyv_list_uniq_equal(void *val1_p, void *val2_p, int UNUSED(mod), void *cb_data)
{
    struct llly_ctx *ctx;
    struct lllys_node_list *slist;
    struct lllyd_node *diter, *first, *second;
    const char *val1, *val2;
    char *path1, *path2, *uniq_str;
    uint16_t idx_uniq;
    int i, j, r, action;

    assert(val1_p && val2_p);

    first = *((struct lllyd_node **)val1_p);
    second = *((struct lllyd_node **)val2_p);
    action = (intptr_t)cb_data;

    assert(first && (first->schema->nodetype == LLLYS_LIST));
    assert(second && (second->schema == first->schema));

    ctx = first->schema->module->ctx;

    slist = (struct lllys_node_list *)first->schema;

    /* compare unique leaves */
    if (action > 0) {
        i = action - 1;
        if (i < slist->unique_size) {
            goto uniquecheck;
        }
    }
    for (i = 0; i < slist->unique_size; i++) {
uniquecheck:
        for (j = 0; j < slist->unique[i].expr_size; j++) {
            /* first */
            diter = resolve_data_descendant_schema_nodeid(slist->unique[i].expr[j], first->child);
            if (diter) {
                val1 = ((struct lllyd_node_leaf_list *)diter)->value_str;
            } else {
                /* use default value */
                if (lllyd_get_unique_default(slist->unique[i].expr[j], first, &val1)) {
                    return 1;
                }
            }

            /* second */
            diter = resolve_data_descendant_schema_nodeid(slist->unique[i].expr[j], second->child);
            if (diter) {
                val2 = ((struct lllyd_node_leaf_list *)diter)->value_str;
            } else {
                /* use default value */
                if (lllyd_get_unique_default(slist->unique[i].expr[j], second, &val2)) {
                    return 1;
                }
            }

            if (!val1 || !val2 || !llly_strequal(val1, val2, 1)) {
                /* values differ or either one is not set */
                break;
            }
        }
        if (j && (j == slist->unique[i].expr_size)) {
            /* all unique leafs are the same in this set, create this nice error */
            llly_vlog_build_path(LLLY_VLOG_LYD, first, &path1, 0, 0);
            llly_vlog_build_path(LLLY_VLOG_LYD, second, &path2, 0, 0);

            /* use buffer to rebuild the unique string */
            uniq_str = malloc(1024);
            idx_uniq = 0;
            for (j = 0; j < slist->unique[i].expr_size; ++j) {
                if (j) {
                    uniq_str[idx_uniq++] = ' ';
                }
                r = lllyd_build_relative_data_path(lllys_node_module((struct lllys_node *)slist), first,
                                                 slist->unique[i].expr[j], &uniq_str[idx_uniq]);
                if (r == -1) {
                    goto unique_errmsg_cleanup;
                }
                idx_uniq += r;
            }

            LOGVAL(ctx, LLLYE_NOUNIQ, LLLY_VLOG_LYD, second, uniq_str, path1, path2);
unique_errmsg_cleanup:
            free(path1);
            free(path2);
            free(uniq_str);
            return 1;
        }

        if (action > 0) {
            /* done */
            return 0;
        }
    }

    return 0;
}

int
lllyv_data_unique(struct lllyd_node *list)
{
    struct lllyd_node *diter;
    struct llly_set *set;
    uint32_t i, j, n = 0;
    int ret = 0;
    uint32_t hash, u, usize = 0;
    struct hash_table **uniqtables = NULL;
    const char *id;
    char *path;
    struct lllys_node_list *slist;
    struct llly_ctx *ctx = list->schema->module->ctx;

    if (!(list->validity & LLLYD_VAL_UNIQUE)) {
        /* validated sa part of another instance validation */
        return 0;
    }

    slist = (struct lllys_node_list *)list->schema;

    /* get all list instances */
    if (llly_vlog_build_path(LLLY_VLOG_LYD, list, &path, 0, 1)) {
        return -1;
    }
    set = lllyd_find_path(list, path);
    free(path);
    if (!set) {
        return -1;
    }

    for (i = 0; i < set->number; ++i) {
        /* remove the flag */
        set->set.d[i]->validity &= ~LLLYD_VAL_UNIQUE;
    }

    if (set->number == 2) {
        /* simple comparison */
        if (lllyv_list_uniq_equal(&set->set.d[0], &set->set.d[1], 0, (void *)0)) {
            /* instance duplication */
            llly_set_free(set);
            return 1;
        }
    } else if (set->number > 2) {
        /* use hashes for comparison */
        /* first, allocate the table, the size depends on number of items in the set */
        for (u = 31; u > 0; u--) {
            usize = set->number << u;
            usize = usize >> u;
            if (usize == set->number) {
                break;
            }
        }
        if (u == 0) {
            LOGINT(ctx);
            ret = -1;
            goto cleanup;
        } else {
            u = 32 - u;
            usize = 1 << u;
        }

        n = slist->unique_size;
        uniqtables = malloc(n * sizeof *uniqtables);
        if (!uniqtables) {
            LOGMEM(ctx);
            ret = -1;
            n = 0;
            goto cleanup;
        }
        for (j = 0; j < n; j++) {
            uniqtables[j] = lllyht_new(usize, sizeof(struct lllyd_node *), lllyv_list_uniq_equal, (void *)(j + 1L), 0);
            if (!uniqtables[j]) {
                LOGMEM(ctx);
                ret = -1;
                goto cleanup;
            }
        }

        for (u = 0; u < set->number; u++) {
            /* loop for unique - get the hash for the instances */
            for (j = 0; j < n; j++) {
                id = NULL;
                for (i = hash = 0; i < slist->unique[j].expr_size; i++) {
                    diter = resolve_data_descendant_schema_nodeid(slist->unique[j].expr[i], set->set.d[u]->child);
                    if (diter) {
                        id = ((struct lllyd_node_leaf_list *)diter)->value_str;
                    } else {
                        /* use default value */
                        if (lllyd_get_unique_default(slist->unique[j].expr[i], set->set.d[u], &id)) {
                            ret = -1;
                            goto cleanup;
                        }
                    }
                    if (!id) {
                        /* unique item not present nor has default value */
                        break;
                    }
                    hash = dict_hash_multi(hash, id, strlen(id));
                }
                if (!id) {
                    /* skip this list instance since its unique set is incomplete */
                    continue;
                }

                /* finish the hash value */
                hash = dict_hash_multi(hash, NULL, 0);

                /* insert into the hashtable */
                if (lllyht_insert(uniqtables[j], &set->set.d[u], hash, NULL)) {
                    ret = 1;
                    goto cleanup;
                }
            }
        }
    }

cleanup:
    llly_set_free(set);
    for (j = 0; j < n; j++) {
        if (!uniqtables[j]) {
            /* failed when allocating uniquetables[j], following j are not allocated */
            break;
        }
        lllyht_free(uniqtables[j]);
    }
    free(uniqtables);

    return ret;
}

static int
lllyv_list_equal(void *val1_p, void *val2_p, int UNUSED(mod), void *UNUSED(cb_data))
{
    struct llly_ctx *ctx;
    struct lllys_node_list *slist;
    const struct lllys_node *snode = NULL;
    struct lllyd_node *diter, *first, *second;
    const char *val1, *val2;
    int i;

    assert(val1_p && val2_p);

    first = *((struct lllyd_node **)val1_p);
    second = *((struct lllyd_node **)val2_p);

    assert(first && (first->schema->nodetype & (LLLYS_LIST | LLLYS_LEAFLIST)));
    assert(second && (second->schema == first->schema));

    ctx = first->schema->module->ctx;

    switch (first->schema->nodetype) {
    case LLLYS_LEAFLIST:
        if ((first->schema->flags & LLLYS_CONFIG_R) && first->schema->module->version >= LLLYS_VERSION_1_1) {
            /* same values are allowed for status data */
            return 0;
        }
        /* compare values */
        if (llly_strequal(((struct lllyd_node_leaf_list *)first)->value_str,
                        ((struct lllyd_node_leaf_list *)second)->value_str, 1)) {
            LOGVAL(ctx, LLLYE_DUPLEAFLIST, LLLY_VLOG_LYD, second, second->schema->name,
                   ((struct lllyd_node_leaf_list *)second)->value_str);
            return 1;
        }
        return 0;
    case LLLYS_LIST:
        slist = (struct lllys_node_list *)first->schema;

        /* compare keys */
        if (!slist->keys_size) {
            /* status lists without keys */
            return 0;
        } else {
            for (i = 0; i < slist->keys_size; i++) {
                snode = (struct lllys_node *)slist->keys[i];
                val1 = val2 = NULL;
                LLLY_TREE_FOR(first->child, diter) {
                    if (diter->schema == snode) {
                        val1 = ((struct lllyd_node_leaf_list *)diter)->value_str;
                        break;
                    }
                }
                LLLY_TREE_FOR(second->child, diter) {
                    if (diter->schema == snode) {
                        val2 = ((struct lllyd_node_leaf_list *)diter)->value_str;
                        break;
                    }
                }
                if (!llly_strequal(val1, val2, 1)) {
                    return 0;
                }
            }
        }

        LOGVAL(ctx, LLLYE_DUPLIST, LLLY_VLOG_LYD, second, second->schema->name);
        return 1;

    default:
        LOGINT(ctx);
        return 1;
    }
}

int
lllyv_data_dup(struct lllyd_node *node, struct lllyd_node *start)
{
    struct lllyd_node *diter, *key;
    struct llly_set *set;
    int i, ret = 0;
    uint32_t hash, u, usize = 0;
    struct hash_table *keystable = NULL;
    const char *id;
    struct llly_ctx *ctx = node->schema->module->ctx;

    /* get the first list/leaflist instance sibling */
    if (!start) {
        start = lllyd_first_sibling(node);
    }

    /* check uniqueness of the list/leaflist instances (compare values) */
    set = llly_set_new();
    for (diter = start; diter; diter = diter->next) {
        if (diter->schema != node->schema) {
            /* check only instances of the same list/leaflist */
            continue;
        }

        /* remove the flag */
        diter->validity &= ~LLLYD_VAL_DUP;

        /* store for comparison */
        llly_set_add(set, diter, LLLY_SET_OPT_USEASLIST);
    }

    if (set->number == 2) {
        /* simple comparison */
        if (lllyv_list_equal(&set->set.d[0], &set->set.d[1], 0, 0)) {
            /* instance duplication */
            llly_set_free(set);
            return 1;
        }
    } else if (set->number > 2) {
        /* use hashes for comparison */
        /* first, allocate the table, the size depends on number of items in the set */
        for (u = 31; u > 0; u--) {
            usize = set->number << u;
            usize = usize >> u;
            if (usize == set->number) {
                break;
            }
        }
        if (u == 0) {
            LOGINT(ctx);
            ret = 1;
            goto cleanup;
        } else {
            u = 32 - u;
            usize = 1 << u;
        }
        keystable = lllyht_new(usize, sizeof(struct lllyd_node *), lllyv_list_equal, 0, 0);
        if (!keystable) {
            LOGMEM(ctx);
            ret = 1;
            goto cleanup;
        }

        for (u = 0; u < set->number; u++) {
            /* get the hash for the instance - keys */
            if (node->schema->nodetype == LLLYS_LEAFLIST) {
                id = ((struct lllyd_node_leaf_list *)set->set.d[u])->value_str;
                hash = dict_hash_multi(0, id, strlen(id));
            } else { /* LLLYS_LIST */
                for (hash = i = 0, key = set->set.d[u]->child;
                        i < ((struct lllys_node_list *)set->set.d[u]->schema)->keys_size;
                        i++, key = key->next) {
                    id = ((struct lllyd_node_leaf_list *)key)->value_str;
                    hash = dict_hash_multi(hash, id, strlen(id));
                }
            }
            /* finish the hash value */
            hash = dict_hash_multi(hash, NULL, 0);

            /* insert into the hashtable */
            if (lllyht_insert(keystable, &set->set.d[u], hash, NULL)) {
                ret = 1;
                goto cleanup;
            }
        }
    }

cleanup:
    llly_set_free(set);
    lllyht_free(keystable);

    return ret;
}

static struct lllys_type *
find_orig_type(struct lllys_type *par_type, LLLY_DATA_TYPE base_type)
{
    struct lllys_type *type, *prev_type, *tmp_type;
    int found;

    /* go through typedefs */
    for (type = par_type; type->der->type.der; type = &type->der->type);

    if (type->base == base_type) {
        /* we have the result */
        return type;
    } else if ((type->base == LLLY_TYPE_LEAFREF) && !(type->value_flags & LLLY_VALUE_UNRES)) {
        /* go through the leafref */
        assert(type->info.lref.target);
        return find_orig_type(&((struct lllys_node_leaf *)type->info.lref.target)->type, base_type);
    } else if (type->base == LLLY_TYPE_UNION) {
        /* go through all the union types */
        prev_type = NULL;
        found = 0;
        while ((prev_type = lllyp_get_next_union_type(type, prev_type, &found))) {
            tmp_type = find_orig_type(prev_type, base_type);
            if (tmp_type) {
                return tmp_type;
            }
            found = 0;
        }
    }

    /* not found */
    return NULL;
}

static int
lllyv_extension(struct lllys_ext_instance **ext, uint8_t size, struct lllyd_node *node)
{
    uint i;

    for (i = 0; i < size; ++i) {
        if ((ext[i]->flags & LLLYEXT_OPT_VALID) && ext[i]->def->plugin->valid_data) {
            if (ext[i]->def->plugin->valid_data(ext[i], node)) {
                return EXIT_FAILURE;
            }
        }
    }
    return 0;
}

static int
lllyv_type_extension(struct lllyd_node_leaf_list *leaf, struct lllys_type *type, int first_type)
{
    struct lllyd_node *node = (struct lllyd_node *)leaf;
    unsigned int i;

    switch (type->base) {
        case LLLY_TYPE_ENUM:
            if (first_type && lllyv_extension(leaf->value.enm->ext, leaf->value.enm->ext_size, node)) {
                return EXIT_FAILURE;
            }
            break;
        case LLLY_TYPE_STRING:
            if (type->info.str.length &&
                lllyv_extension(type->info.str.length->ext, type->info.str.length->ext_size, node)) {
                return EXIT_FAILURE;
            }
            for(i = 0; i < type->info.str.pat_count; ++i) {
                if (lllyv_extension(type->info.str.patterns[i].ext, type->info.str.patterns[i].ext_size, node)) {
                    return EXIT_FAILURE;
                }
            }
            break;
        case LLLY_TYPE_DEC64:
            if (type->info.dec64.range &&
                lllyv_extension(type->info.dec64.range->ext, type->info.dec64.range->ext_size, node)) {
                return EXIT_FAILURE;
            }
            break;
        case LLLY_TYPE_INT8:
        case LLLY_TYPE_INT16:
        case LLLY_TYPE_INT32:
        case LLLY_TYPE_INT64:
        case LLLY_TYPE_UINT8:
        case LLLY_TYPE_UINT16:
        case LLLY_TYPE_UINT32:
        case LLLY_TYPE_UINT64:
            if (type->info.num.range &&
                lllyv_extension(type->info.num.range->ext, type->info.num.range->ext_size, node)) {
                return EXIT_FAILURE;
            }
            break;
        case LLLY_TYPE_BITS:
            if (first_type) {
                /* get the count of bits */
                type = find_orig_type(&((struct lllys_node_leaf *) leaf->schema)->type, LLLY_TYPE_BITS);
                for (i = 0; i < type->info.bits.count; ++i) {
                    if (!leaf->value.bit[i]) {
                        continue;
                    }
                    if (lllyv_extension(leaf->value.bit[i]->ext, leaf->value.bit[i]->ext_size, node)) {
                        return EXIT_FAILURE;
                    }
                }
            }
            break;
        case LLLY_TYPE_UNION:
            for (i = 0; i < type->info.uni.count; ++i) {
                if (type->info.uni.types[i].base == leaf->value_type) {
                    break;
                }
            }
            if (i < type->info.uni.count &&
                lllyv_type_extension(leaf, &type->info.uni.types[i], first_type)) {
                return EXIT_FAILURE;
            }
            break;
        default:
            break;
    }


    if (lllyv_extension(type->ext, type->ext_size, node)) {
        return EXIT_FAILURE;
    }

    while (type->der->type.der) {
        type = &type->der->type;
        if ((type->parent->flags & LLLYS_VALID_EXT)) {
            if (lllyv_type_extension(leaf, type, 0) || lllyv_extension(type->parent->ext, type->parent->ext_size, node)) {
                return EXIT_FAILURE;
            }
        }
    }

    return EXIT_SUCCESS;
}

int
lllyv_data_content(struct lllyd_node *node, int options, struct unres_data *unres)
{
    const struct lllys_node *schema, *siter, *parent;
    struct lllyd_node *diter, *start = NULL;
    struct lllys_ident *ident;
    struct lllys_tpdf *tpdf;
    struct lllys_type *type = NULL;
    struct lllyd_node_leaf_list *leaf;
    unsigned int i, j = 0;
    uint8_t iff_size;
    struct lllys_iffeature *iff;
    const char *id, *idname;
    struct llly_ctx *ctx;

    assert(node);
    assert(node->schema);
    assert(unres);

    schema = node->schema; /* shortcut */
    ctx = schema->module->ctx;

    if (!(node->schema->nodetype & (LLLYS_NOTIF | LLLYS_RPC | LLLYS_ACTION))) {
        for (diter = node->parent; diter; diter = diter->parent) {
            if (diter->schema->nodetype & (LLLYS_NOTIF | LLLYS_RPC | LLLYS_ACTION)) {
                break;
            }
        }
        if (!diter && (options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY | LLLYD_OPT_NOTIF))) {
            /* validating parent of a nested notification/action, skip most checks */
            options |= LLLYD_OPT_TRUSTED;
        }
    }

    if (node->validity & LLLYD_VAL_MAND) {
        if (!(options & (LLLYD_OPT_TRUSTED | LLLYD_OPT_NOTIF_FILTER))) {
            /* check presence and correct order of all keys in case of list */
            if (schema->nodetype == LLLYS_LIST && !(options & (LLLYD_OPT_GET | LLLYD_OPT_GETCONFIG))) {
                if (lllyv_keys(node)) {
                    return 1;
                }
            }

            if (schema->nodetype & (LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_ANYDATA)) {
                /* check number of instances (similar to list uniqueness) for non-list nodes */

                /* find duplicity */
                start = lllyd_first_sibling(node);
                for (diter = start; diter; diter = diter->next) {
                    if (diter->schema == schema && diter != node) {
                        parent = lllys_parent(schema);
                        LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYD, node, schema->name,
                               parent ? (parent->nodetype == LLLYS_EXT) ? ((struct lllys_ext_instance *)parent)->arg_value : parent->name : "data tree");
                        return 1;
                    }
                }
            }

            if (options & LLLYD_OPT_OBSOLETE) {
                /* status - of the node's schema node itself and all its parents that
                * cannot have their own instance (like a choice statement) */
                siter = node->schema;
                do {
                    if (((siter->flags & LLLYS_STATUS_MASK) == LLLYS_STATUS_OBSLT) && (options & LLLYD_OPT_OBSOLETE)) {
                        LOGVAL(ctx, LLLYE_OBSDATA, LLLY_VLOG_LYD, node, schema->name);
                        return 1;
                    }
                    siter = lllys_parent(siter);
                } while (siter && !(siter->nodetype & (LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA)));

                /* status of the identity value */
                if (schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) {
                    if (options & LLLYD_OPT_OBSOLETE) {
                        /* check that we are not instantiating obsolete type */
                        tpdf = ((struct lllys_node_leaf *)node->schema)->type.der;
                        while (tpdf) {
                            if ((tpdf->flags & LLLYS_STATUS_MASK) == LLLYS_STATUS_OBSLT) {
                                LOGVAL(ctx, LLLYE_OBSTYPE, LLLY_VLOG_LYD, node, schema->name, tpdf->name);
                                return 1;
                            }
                            tpdf = tpdf->type.der;
                        }
                    }
                    if (((struct lllyd_node_leaf_list *)node)->value_type == LLLY_TYPE_IDENT) {
                        ident = ((struct lllyd_node_leaf_list *)node)->value.ident;
                        if (lllyp_check_status(schema->flags, schema->module, schema->name,
                                        ident->flags, ident->module, ident->name, NULL)) {
                            LOGPATH(ctx, LLLY_VLOG_LYD, node);
                            return 1;
                        }
                    }
                }
            }
        }

        /* check validation function for extension */
        if (schema->flags & LLLYS_VALID_EXT) {
            // check extension in node
            if (lllyv_extension(schema->ext, schema->ext_size, node)) {
                return EXIT_FAILURE;
            }

            if (schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) {
                type = &((struct lllys_node_leaf *) schema)->type;
                leaf = (struct lllyd_node_leaf_list *) node;
                if (lllyv_type_extension(leaf, type, 1)) {
                    return EXIT_FAILURE;
                }
            }
        }

        /* remove the flag */
        node->validity &= ~LLLYD_VAL_MAND;
    }

    if (schema->nodetype & (LLLYS_LIST | LLLYS_CONTAINER | LLLYS_NOTIF | LLLYS_RPC | LLLYS_ACTION)) {
        siter = NULL;
        while ((siter = lllys_getnext(siter, schema, NULL, 0))) {
            if (siter->nodetype & (LLLYS_LIST | LLLYS_LEAFLIST)) {
                LLLY_TREE_FOR(node->child, diter) {
                    if (diter->schema == siter && (diter->validity & LLLYD_VAL_DUP)) {
                        /* skip key uniqueness check in case of get/get-config data */
                        if (!(options & (LLLYD_OPT_TRUSTED | LLLYD_OPT_GET | LLLYD_OPT_GETCONFIG))) {
                            if (lllyv_data_dup(diter, node->child)) {
                                return 1;
                            }
                        } else {
                            /* always remove the flag */
                            diter->validity &= ~LLLYD_VAL_DUP;
                        }
                        /* all schema instances checked, continue with another schema node */
                        break;
                    }
                }
            }
        }
    }

    if (node->validity & LLLYD_VAL_UNIQUE) {
        if (options & LLLYD_OPT_TRUSTED) {
            /* just remove flag */
            node->validity &= ~LLLYD_VAL_UNIQUE;
        } else {
            /* check the unique constraint at the end (once the parsing is done) */
            if (unres_data_add(unres, node, UNRES_UNIQ_LEAVES)) {
                return 1;
            }
        }
    }

    if (schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) {
        /* since feature can be enabled/disabled, do this check despite the validity flag,
         * - check if the type value (enum, bit, identity) is disabled via feature  */
        leaf = (struct lllyd_node_leaf_list *)node;
        switch (leaf->value_type) {
        case LLLY_TYPE_BITS:
            id = "Bit";
            /* get the count of bits */
            type = find_orig_type(&((struct lllys_node_leaf *)leaf->schema)->type, LLLY_TYPE_BITS);
            for (j = iff_size = 0; j < type->info.bits.count; j++) {
                if (!leaf->value.bit[j]) {
                    continue;
                }
                idname = leaf->value.bit[j]->name;
                iff_size = leaf->value.bit[j]->iffeature_size;
                iff = leaf->value.bit[j]->iffeature;
                break;
nextbit:
                iff_size = 0;
            }
            break;
        case LLLY_TYPE_ENUM:
            id = "Enum";
            idname = leaf->value_str;
            iff_size = leaf->value.enm->iffeature_size;
            iff = leaf->value.enm->iffeature;
            break;
        case LLLY_TYPE_IDENT:
            id = "Identity";
            idname = leaf->value_str;
            iff_size = leaf->value.ident->iffeature_size;
            iff = leaf->value.ident->iffeature;
            break;
        default:
            iff_size = 0;
            break;
        }

        if (iff_size) {
            for (i = 0; i < iff_size; i++) {
                if (!resolve_iffeature(&iff[i])) {
                    LOGVAL(ctx, LLLYE_INVAL, LLLY_VLOG_LYD, node, leaf->value_str, schema->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "%s \"%s\" is disabled by its if-feature condition.",
                           id, idname);
                    return 1;
                }
            }
            if (leaf->value_type == LLLY_TYPE_BITS) {
                goto nextbit;
            }
        }
    }

    /* check must conditions */
    if (!(options & (LLLYD_OPT_TRUSTED | LLLYD_OPT_NOTIF_FILTER | LLLYD_OPT_EDIT | LLLYD_OPT_GET | LLLYD_OPT_GETCONFIG))) {
        i = resolve_applies_must(node);
        if ((i & 0x1) && unres_data_add(unres, node, UNRES_MUST)) {
            return 1;
        }
        if ((i & 0x2) && unres_data_add(unres, node, UNRES_MUST_INOUT)) {
            return 1;
        }
    }

    return 0;
}

int
lllyv_multicases(struct lllyd_node *node, struct lllys_node *schemanode, struct lllyd_node **first_sibling,
               int autodelete, struct lllyd_node *nodel)
{
    struct lllys_node *sparent, *schoice, *scase, *saux;
    struct lllyd_node *next, *iter;
    assert(node || schemanode);

    if (!schemanode) {
        schemanode = node->schema;
    }

    for (sparent = lllys_parent(schemanode); sparent && (sparent->nodetype == LLLYS_USES); sparent = lllys_parent(sparent));
    if (!sparent || !(sparent->nodetype & (LLLYS_CHOICE | LLLYS_CASE))) {
        /* node is not under any choice */
        return 0;
    } else if (!first_sibling || !(*first_sibling)) {
        /* nothing to check */
        return 0;
    }

    /* remember which case to skip in which choice */
    if (sparent->nodetype == LLLYS_CHOICE) {
        schoice = sparent;
        scase = schemanode;
    } else {
        schoice = lllys_parent(sparent);
        scase = sparent;
    }

autodelete:
    /* remove all nodes from other cases than 'sparent' */
    LLLY_TREE_FOR_SAFE(*first_sibling, next, iter) {
        if (schemanode == iter->schema) {
            continue;
        }

        for (sparent = lllys_parent(iter->schema); sparent && (sparent->nodetype == LLLYS_USES); sparent = lllys_parent(sparent));
        if (sparent && ((sparent->nodetype == LLLYS_CHOICE && sparent == schoice) /* another implicit case */
                || (sparent->nodetype == LLLYS_CASE && sparent != scase && lllys_parent(sparent) == schoice)) /* another case */
                ) {
            if (autodelete) {
                if (iter == nodel) {
                    LOGVAL(schemanode->module->ctx, LLLYE_MCASEDATA, LLLY_VLOG_LYD, iter, schoice->name);
                    return 2;
                }
                if (iter == *first_sibling) {
                    *first_sibling = next;
                }
                lllyd_free(iter);
            } else {
                LOGVAL(schemanode->module->ctx, LLLYE_MCASEDATA, LLLY_VLOG_LYD, iter, schoice->name);
                return 1;
            }
        }
    }

    if (*first_sibling && (saux = lllys_parent(schoice)) && (saux->nodetype & LLLYS_CASE)) {
        /* go recursively in case of nested choices */
        schoice = lllys_parent(saux);
        scase = saux;
        goto autodelete;
    }

    return 0;
}
