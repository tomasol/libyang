/**
 * @file tree_data.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Manipulation with libyang data structures
 *
 * Copyright (c) 2015 - 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "libyang.h"
#include "common.h"
#include "context.h"
#include "tree_data.h"
#include "parser.h"
#include "resolve.h"
#include "xml_internal.h"
#include "tree_internal.h"
#include "validation.h"
#include "xpath.h"

static struct lllys_node *lllyd_get_schema_inctx(const struct lllyd_node *node, struct llly_ctx *ctx);

static struct lllyd_node *lllyd_dup_withsiblings_to_ctx(const struct lllyd_node *node, int options, struct llly_ctx *ctx);

static struct lllyd_node *lllyd_new_dummy(struct lllyd_node *root, struct lllyd_node *parent, const struct lllys_node *schema,
                                      const char *value, int dflt);

static int
lllyd_anydata_equal(struct lllyd_node *first, struct lllyd_node *second)
{
    char *str1 = NULL, *str2 = NULL;
    struct lllyd_node_anydata *anydata;

    assert(first->schema->nodetype & LLLYS_ANYDATA);
    assert(first->schema->nodetype == second->schema->nodetype);

    anydata = (struct lllyd_node_anydata *)first;
    if (!anydata->value.str) {
        lllyxml_print_mem(&str1, anydata->value.xml, LLLYXML_PRINT_SIBLINGS);
        anydata->value.str = lllydict_insert_zc(anydata->schema->module->ctx, str1);
    }
    str1 = (char *)anydata->value.str;

    anydata = (struct lllyd_node_anydata *)second;
    if (!anydata->value.str) {
        lllyxml_print_mem(&str2, anydata->value.xml, LLLYXML_PRINT_SIBLINGS);
        anydata->value.str = lllydict_insert_zc(anydata->schema->module->ctx, str2);
    }
    str2 = (char *)anydata->value.str;

    if (first->schema->module->ctx != second->schema->module->ctx) {
        return llly_strequal(str1, str2, 0);
    } else {
        return llly_strequal(str1, str2, 1);
    }
}

/* used in tests */
int
lllyd_list_has_keys(struct lllyd_node *list)
{
    struct lllyd_node *iter;
    struct lllys_node_list *slist;
    int i;

    assert(list->schema->nodetype == LLLYS_LIST);

    /* even though hash is 0, it may be a valid hash, that is what we are going to check */

    slist = (struct lllys_node_list *)list->schema;
    if (!slist->keys_size) {
        /* always has keys */
        return 1;
    }

    i = 0;
    iter = list->child;
    while (iter && (i < slist->keys_size)) {
        if (iter->schema != (struct lllys_node *)slist->keys[i]) {
            /* missing key */
            return 0;
        }

        ++i;
        iter = iter->next;
    }
    if (i < slist->keys_size) {
        /* missing key */
        return 0;
    }

    /* all keys found */
    return 1;
}

static int
lllyd_leaf_val_equal(struct lllyd_node *node1, struct lllyd_node *node2, int diff_ctx)
{
    assert(node1->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST));
    assert(node1->schema->nodetype == node2->schema->nodetype);

    if (diff_ctx) {
        return llly_strequal(((struct lllyd_node_leaf_list *)node1)->value_str, ((struct lllyd_node_leaf_list *)node2)->value_str, 0);
    } else {
        return llly_strequal(((struct lllyd_node_leaf_list *)node1)->value_str, ((struct lllyd_node_leaf_list *)node2)->value_str, 1);
    }
}

/*
 * withdefaults (only for leaf-list):
 * 0 - treat default nodes are normal nodes
 * 1 - only change is that if 2 nodes have the same value, but one is default, the other not, they are considered non-equal
 */
int
lllyd_list_equal(struct lllyd_node *node1, struct lllyd_node *node2, int with_defaults)
{
    int i, diff_ctx;
    struct lllyd_node *elem1, *next1, *elem2, *next2;
    struct lllys_node *elem1_sch;
    struct llly_ctx *ctx = node2->schema->module->ctx;

    diff_ctx = (node1->schema->module->ctx != node2->schema->module->ctx);

    switch (node2->schema->nodetype) {
    case LLLYS_LEAFLIST:
        if (lllyd_leaf_val_equal(node1, node2, diff_ctx) && (!with_defaults || (node1->dflt == node2->dflt))) {
            return 1;
        }
        break;
    case LLLYS_LIST:
        if (((struct lllys_node_list *)node1->schema)->keys_size) {
            /* lists with keys, their equivalence isb ased on their keys */
            elem1 = node1->child;
            elem2 = node2->child;
            elem1_sch = NULL;
            /* the exact data order is guaranteed */
            for (i = 0; i < ((struct lllys_node_list *)node1->schema)->keys_size; ++i) {
                if (diff_ctx && elem1) {
                    /* we have different contexts */
                    if (!elem1_sch) {
                        elem1_sch = lllyd_get_schema_inctx(elem1, ctx);
                        if (!elem1_sch) {
                            LOGERR(ctx, LLLY_EINVAL, "Target context does not contain a required schema node (%s:%s).",
                                   lllyd_node_module(elem1)->name, elem1->schema->name);
                            return -1;
                        }
                    } else {
                        /* just move to the next schema node */
                        elem1_sch = elem1_sch->next;
                    }
                }
                if (!elem1 || !elem2 || ((elem1_sch ? elem1_sch : elem1->schema) != elem2->schema)
                        || !lllyd_leaf_val_equal(elem1, elem2, diff_ctx)) {
                    break;
                }
                elem1 = elem1->next;
                elem2 = elem2->next;
            }
            if (i == ((struct lllys_node_list *)node1->schema)->keys_size) {
                return 1;
            }
        } else {
            /* lists wihtout keys, their equivalence is based on values of all the children (both dierct and indirect) */
            if (!node1->child && !node2->child) {
                /* no children, nothing to compare */
                return 1;
            }

            /* status lists without keys, we need to compare all the children :( */

            /* LLLY_TREE_DFS_BEGIN for 2 data trees */
            elem1 = next1 = node1->child;
            elem2 = next2 = node2->child;
            while (elem1 && elem2) {
                /* node comparison */
#ifdef LLLY_ENABLED_CACHE
                if (elem1->hash != elem2->hash) {
                    break;
                }
#endif
                if (diff_ctx) {
                    elem1_sch = lllyd_get_schema_inctx(elem1, ctx);
                    if (!elem1_sch) {
                        LOGERR(ctx, LLLY_EINVAL, "Target context does not contain a required schema node (%s:%s).",
                               lllyd_node_module(elem1)->name, elem1->schema->name);
                        return -1;
                    }
                } else {
                    elem1_sch = elem1->schema;
                }
                if (elem1_sch != elem2->schema) {
                    break;
                }
                if (elem2->schema->nodetype == LLLYS_LIST) {
                    if (!lllyd_list_has_keys(elem1) && !lllyd_list_has_keys(elem2)) {
                        /* we encountered lists without keys (but have some defined in schema), ignore them for comparison */
                        next1 = NULL;
                        next2 = NULL;
                        goto next_sibling;
                    }
                    /* we will compare all the children of this list instance, not just keys */
                } else if (elem2->schema->nodetype & (LLLYS_LEAFLIST | LLLYS_LEAF)) {
                    if (!lllyd_leaf_val_equal(elem1, elem2, diff_ctx) && (!with_defaults || (elem1->dflt == elem2->dflt))) {
                        break;
                    }
                } else if (elem2->schema->nodetype & LLLYS_ANYDATA) {
                    if (!lllyd_anydata_equal(elem1, elem2)) {
                        break;
                    }
                }

                /* LLLY_TREE_DFS_END for 2 data trees */
                if (elem2->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA)) {
                    next1 = NULL;
                    next2 = NULL;
                } else {
                    next1 = elem1->child;
                    next2 = elem2->child;
                }

next_sibling:
                if (!next1) {
                    next1 = elem1->next;
                }
                if (!next2) {
                    next2 = elem2->next;
                }

                while (!next1) {
                    elem1 = elem1->parent;
                    if (elem1 == node1) {
                        break;
                    }
                    next1 = elem1->next;
                }
                while (!next2) {
                    elem2 = elem2->parent;
                    if (elem2 == node2) {
                        break;
                    }
                    next2 = elem2->next;
                }

                elem1 = next1;
                elem2 = next2;
            }

            if (!elem1 && !elem2) {
                /* all children were successfully compared */
                return 1;
            }
        }
        break;
    default:
        LOGINT(ctx);
        return -1;
    }

    return 0;
}

#ifdef LLLY_ENABLED_CACHE

static int
lllyd_hash_table_val_equal(void *val1_p, void *val2_p, int mod, void *UNUSED(cb_data))
{
    struct lllyd_node *val1, *val2;

    val1 = *((struct lllyd_node **)val1_p);
    val2 = *((struct lllyd_node **)val2_p);

    if (mod) {
        if (val1 == val2) {
            return 1;
        } else {
            return 0;
        }
    }

    if (val1->schema != val2->schema) {
        return 0;
    }

    switch (val1->schema->nodetype) {
    case LLLYS_CONTAINER:
    case LLLYS_LEAF:
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        return 1;
    case LLLYS_LEAFLIST:
    case LLLYS_LIST:
        return lllyd_list_equal(val1, val2, 0);
    default:
        break;
    }

    LOGINT(val1->schema->module->ctx);
    return 0;
}

static void
lllyd_hash_keyless_list_dfs(struct lllyd_node *child, uint32_t *hash)
{
    LLLY_TREE_FOR(child, child) {
        switch (child->schema->nodetype) {
        case LLLYS_CONTAINER:
            lllyd_hash_keyless_list_dfs(child->child, hash);
            break;
        case LLLYS_LIST:
            /* ignore lists with missing keys */
            if (lllyd_list_has_keys(child)) {
                lllyd_hash_keyless_list_dfs(child->child, hash);
            }
            break;
        case LLLYS_LEAFLIST:
        case LLLYS_ANYXML:
        case LLLYS_ANYDATA:
        case LLLYS_LEAF:
            *hash = dict_hash_multi(*hash, (char *)&child->hash, sizeof child->hash);
            break;
        default:
            assert(0);
        }
    }
}

int
lllyd_hash(struct lllyd_node *node)
{
    struct lllyd_node *iter;
    int i;

    if ((node->schema->nodetype != LLLYS_LIST) || lllyd_list_has_keys(node)) {
        node->hash = dict_hash_multi(0, lllyd_node_module(node)->name, strlen(lllyd_node_module(node)->name));
        node->hash = dict_hash_multi(node->hash, node->schema->name, strlen(node->schema->name));
        if (node->schema->nodetype == LLLYS_LEAFLIST) {
            node->hash = dict_hash_multi(node->hash, ((struct lllyd_node_leaf_list *)node)->value_str,
                                        strlen(((struct lllyd_node_leaf_list *)node)->value_str));
        } else if (node->schema->nodetype == LLLYS_LIST) {
            if (((struct lllys_node_list *)node->schema)->keys_size) {
                for (i = 0, iter = node->child; i < ((struct lllys_node_list *)node->schema)->keys_size; ++i, iter = iter->next) {
                    assert(iter);
                    node->hash = dict_hash_multi(node->hash, ((struct lllyd_node_leaf_list *)iter)->value_str,
                                                 strlen(((struct lllyd_node_leaf_list *)iter)->value_str));
                }
            } else {
                /* no-keys list */
                lllyd_hash_keyless_list_dfs(node->child, &node->hash);
            }
        }
        node->hash = dict_hash_multi(node->hash, NULL, 0);
        return 0;
    }

    return 1;
}

static void
lllyd_keyless_list_hash_change(struct lllyd_node *parent)
{
    int r;

    while (parent && !(parent->schema->flags & LLLYS_CONFIG_W)) {
        if (parent->schema->nodetype == LLLYS_LIST) {
            if (parent->hash && !((struct lllys_node_list *)parent->schema)->keys_size) {
                if (parent->parent && parent->parent->ht) {
                    /* remove the list from the parent */
                    r = lllyht_remove(parent->parent->ht, &parent, parent->hash);
                    assert(!r);
                    (void)r;
                }
                /* recalculate the hash */
                lllyd_hash(parent);
                if (parent->parent && parent->parent->ht) {
                    /* re-add the list again */
                    r = lllyht_insert(parent->parent->ht, &parent, parent->hash, NULL);
                    assert(!r);
                    (void)r;
                }
            } else if (!lllyd_list_has_keys(parent)) {
                /* a parent is a list without keys so it cannot be a part of any parent hash */
                break;
            }
        }

        parent = parent->parent;
    }
}

static void
_lyd_insert_hash(struct lllyd_node *node, int keyless_list_check)
{
    struct lllyd_node *iter;
    int i;

    if (node->parent) {
        if ((node->schema->nodetype != LLLYS_LIST) || lllyd_list_has_keys(node)) {
            if ((node->schema->nodetype == LLLYS_LEAF) && lllys_is_key((struct lllys_node_leaf *)node->schema, NULL)) {
                /* we are adding a key which means that it may be the last missing key for our parent's hash */
                if (!lllyd_hash(node->parent)) {
                    /* yep, we successfully hashed node->parent so it is technically now added to its parent (hash-wise) */
                    _lyd_insert_hash(node->parent, 0);
                }
            }

            /* create parent hash table if required, otherwise just add the new child */
            if (!node->parent->ht) {
                for (i = 0, iter = node->parent->child; iter; ++i, iter = iter->next) {
                    if ((iter->schema->nodetype == LLLYS_LIST) && !lllyd_list_has_keys(iter)) {
                        /* it will either never have keys and will never be hashed or has not all keys created yet */
                        --i;
                    }
                }
                assert(i <= LLLY_CACHE_HT_MIN_CHILDREN);
                if (i == LLLY_CACHE_HT_MIN_CHILDREN) {
                    /* create hash table, insert all the children */
                    node->parent->ht = lllyht_new(1, sizeof(struct lllyd_node *), lllyd_hash_table_val_equal, NULL, 1);
                    LLLY_TREE_FOR(node->parent->child, iter) {
                        if ((iter->schema->nodetype == LLLYS_LIST) && !lllyd_list_has_keys(iter)) {
                            /* skip lists without keys */
                            continue;
                        }

                        if (lllyht_insert(node->parent->ht, &iter, iter->hash, NULL)) {
                            assert(0);
                        }
                    }
                }
            } else {
                if (lllyht_insert(node->parent->ht, &node, node->hash, NULL)) {
                    assert(0);
                }
            }

            /* if node was in a state data subtree, wasn't it a part of a key-less list hash? */
            if (keyless_list_check) {
                lllyd_keyless_list_hash_change(node->parent);
            }
        }
    }
}

/* we have inserted node into a parent */
void
lllyd_insert_hash(struct lllyd_node *node)
{
    _lyd_insert_hash(node, 1);
}

static void
_lyd_unlink_hash(struct lllyd_node *node, struct lllyd_node *orig_parent, int keyless_list_check)
{
#ifndef NDEBUG
    struct lllyd_node *iter;

    /* it must already be unlinked otherwise keyless lists would get wrong hash */
    if (keyless_list_check && orig_parent) {
        LLLY_TREE_FOR(orig_parent->child, iter) {
            assert(iter != node);
        }
    }
#endif

    if (orig_parent && node->hash && ((node->schema->nodetype != LLLYS_LIST) || lllyd_list_has_keys(node))) {
        if (orig_parent->ht) {
            if (lllyht_remove(orig_parent->ht, &node, node->hash)) {
                assert(0);
            }

            /* if no longer enough children, free the whole hash table */
            if (orig_parent->ht->used < LLLY_CACHE_HT_MIN_CHILDREN) {
                lllyht_free(orig_parent->ht);
                orig_parent->ht = NULL;
            }
        }

        /* if the parent is missing a key now, remove hash, also from parent */
        if (lllys_is_key((struct lllys_node_leaf *)node->schema, NULL) && orig_parent->hash) {
            _lyd_unlink_hash(orig_parent, orig_parent->parent, 0);
            orig_parent->hash = 0;
        }

        /* if node was in a state data subtree, shouldn't it be a part of a key-less list hash? */
        if (keyless_list_check) {
            lllyd_keyless_list_hash_change(orig_parent);
        }
    }
}

/* we are unlinking a child from a parent */
void
lllyd_unlink_hash(struct lllyd_node *node, struct lllyd_node *orig_parent)
{
    _lyd_unlink_hash(node, orig_parent, 1);
}

#endif

/**
 * @brief get the list of \p data's siblings of the given schema
 */
static int
lllyd_get_node_siblings(const struct lllyd_node *data, const struct lllys_node *schema, struct llly_set *set)
{
    const struct lllyd_node *iter;

    assert(set && !set->number);
    assert(schema);
    assert(schema->nodetype & (LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LIST | LLLYS_LEAFLIST | LLLYS_ANYDATA | LLLYS_NOTIF | LLLYS_RPC |
                               LLLYS_ACTION));

    if (!data) {
        return 0;
    }

    LLLY_TREE_FOR(data, iter) {
        if (iter->schema == schema) {
            llly_set_add(set, (void*)iter, LLLY_SET_OPT_USEASLIST);
        }
    }

    return set->number;
}

/**
 * Check whether there are any "when" statements on a \p schema node and evaluate them.
 *
 * @return -1 on error, 0 on no when or evaluated to true, 1 on when evaluated to false
 */
static int
lllyd_is_when_false(struct lllyd_node *root, struct lllyd_node *last_parent, struct lllys_node *schema, int options)
{
    enum int_log_opts prev_ilo;
    struct lllyd_node *current, *dummy;

    if ((!(options & LLLYD_OPT_TYPEMASK) || (options & (LLLYD_OPT_CONFIG | LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY | LLLYD_OPT_NOTIF | LLLYD_OPT_DATA_TEMPLATE)))
            && resolve_applies_when(schema, 1, last_parent ? last_parent->schema : NULL)) {
        /* evaluate when statements on a dummy data node */
        if (schema->nodetype == LLLYS_CHOICE) {
            schema = (struct lllys_node *)lllys_getnext(NULL, schema, NULL, LLLYS_GETNEXT_NOSTATECHECK);
        }
        dummy = lllyd_new_dummy(root, last_parent, schema, NULL, 0);
        if (!dummy) {
            return -1;
        }
        if (!dummy->parent && root) {
            /* connect dummy nodes into the data tree, insert it before the root
             * to optimize later unlinking (lllyd_free()) */
            lllyd_insert_before(root, dummy);
        }
        for (current = dummy; current; current = current->child) {
            llly_ilo_change(NULL, ILO_IGNORE, &prev_ilo, NULL);
            resolve_when(current, 0, NULL);
            llly_ilo_restore(NULL, prev_ilo, NULL, 0);

            if (current->when_status & LLLYD_WHEN_FALSE) {
                /* when evaluates to false */
                lllyd_free(dummy);
                return 1;
            }

            if (current->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA)) {
                /* termination node without a child */
                break;
            }
        }
        lllyd_free(dummy);
    }

    return 0;
}

/**
 * @param[in] root Root node to be able search the data tree in case of no instance
 * @return
 *  0 - all restrictions met
 *  1 - restrictions not met
 *  2 - schema node not enabled
 */
static int
lllyd_check_mandatory_data(struct lllyd_node *root, struct lllyd_node *last_parent,
                         struct llly_set *instances, struct lllys_node *schema, int options)
{
    struct llly_ctx *ctx = schema->module->ctx;
    uint32_t limit;
    uint16_t status;

    if (!instances->number) {
        /* no instance in the data tree - check if the instantiating is enabled
         * (check: if-feature, when, status data in non-status data tree)
         */
        status = (schema->flags & LLLYS_STATUS_MASK);
        if (lllys_is_disabled(schema, 2) || (status && status != LLLYS_STATUS_CURR)) {
            /* disabled by if-feature */
            return EXIT_SUCCESS;
        } else if ((options & LLLYD_OPT_TRUSTED) || ((options & LLLYD_OPT_TYPEMASK) && (schema->flags & LLLYS_CONFIG_R))) {
            /* status schema node in non-status data tree */
            return EXIT_SUCCESS;
        } else if (lllyd_is_when_false(root, last_parent, schema, options)) {
            return EXIT_SUCCESS;
        }
        /* the schema instance is not disabled by anything, continue with checking */
    }

    /* checking various mandatory conditions */
    switch (schema->nodetype) {
    case LLLYS_LEAF:
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        /* mandatory */
        if ((schema->flags & LLLYS_MAND_TRUE) && !instances->number) {
            LOGVAL(ctx, LLLYE_MISSELEM, LLLY_VLOG_LYD, last_parent, schema->name,
                   last_parent ? last_parent->schema->name : lllys_node_module(schema)->name);
            return EXIT_FAILURE;
        }
        break;
    case LLLYS_LIST:
        /* min-elements */
        limit = ((struct lllys_node_list *)schema)->min;
        if (limit && limit > instances->number) {
            LOGVAL(ctx, LLLYE_NOMIN, LLLY_VLOG_LYD, last_parent, schema->name);
            return EXIT_FAILURE;
        }
        /* max elements */
        limit = ((struct lllys_node_list *)schema)->max;
        if (limit && limit < instances->number) {
            LOGVAL(ctx, LLLYE_NOMAX, LLLY_VLOG_LYD, instances->set.d[limit], schema->name);
            return EXIT_FAILURE;
        }

        break;

    case LLLYS_LEAFLIST:
        /* min-elements */
        limit = ((struct lllys_node_leaflist *)schema)->min;
        if (limit && limit > instances->number) {
            LOGVAL(ctx, LLLYE_NOMIN, LLLY_VLOG_LYD, last_parent, schema->name);
            return EXIT_FAILURE;
        }
        /* max elements */
        limit = ((struct lllys_node_leaflist *)schema)->max;
        if (limit && limit < instances->number) {
            LOGVAL(ctx, LLLYE_NOMAX, LLLY_VLOG_LYD, instances->set.d[limit], schema->name);
            return EXIT_FAILURE;
        }
        break;
    default:
        /* we cannot get here */
        assert(0);
        break;
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Check the specific subtree, specified by \p schema node, for presence of mandatory nodes. Function goes
 * recursively into the subtree.
 *
 * What is being checked:
 * - mandatory statement in leaf, choice, anyxml and anydata
 * - min-elements and max-elements in list and leaf-list
 *
 * @param[in] tree Data tree, needed for case that subtree is NULL (in case of not existing data nodes to explore)
 * @param[in] subtree Depend ons \p toplevel flag:
 *                 toplevel = 1, then subtree is ignored, instead the tree is taken to search in top level data elements (if any)
 *                 toplevel = 0, subtree is the parent data node of the possible instances of the schema node being checked
 * @param[in] last_parent The last present parent data node (so it does not need to be a direct parent) of the possible
 *                 instances of the schema node being checked
 * @param[in] schema The schema node being checked for mandatory nodes
 * @param[in] toplevel, see the \p root parameter description
 * @param[in] options @ref parseroptions to specify the type of the data tree.
 * @return EXIT_SUCCESS or EXIT_FAILURE if there are missing mandatory nodes
 */
static int
lllyd_check_mandatory_subtree(struct lllyd_node *tree, struct lllyd_node *subtree, struct lllyd_node *last_parent,
                            struct lllys_node *schema, int toplevel, int options)
{
    struct lllys_node *siter, *siter_prev;
    struct lllyd_node *iter;
    struct llly_set *present = NULL;
    unsigned int u;
    int ret = EXIT_FAILURE;

    assert(schema);

    if (lllys_is_disabled(schema, 0)) {
        return EXIT_SUCCESS;
    }

    if (schema->nodetype & (LLLYS_LEAF | LLLYS_LIST | LLLYS_LEAFLIST | LLLYS_ANYDATA | LLLYS_CONTAINER)) {
        /* data node */
        present = llly_set_new();
        if (!present) {
            goto error;
        }
        if ((toplevel && tree) || (!toplevel && subtree)) {
            if (toplevel) {
                lllyd_get_node_siblings(tree, schema, present);
            } else {
                lllyd_get_node_siblings(subtree->child, schema, present);
            }
        }
    }

    switch (schema->nodetype) {
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        /* check the schema item */
        if (lllyd_check_mandatory_data(tree, last_parent, present, schema, options)) {
            goto error;
        }
        break;
    case LLLYS_LIST:
        /* check the schema item */
        if (lllyd_check_mandatory_data(tree, last_parent, present, schema, options)) {
            goto error;
        }

        /* go recursively */
        for (u = 0; u < present->number; u++) {
            LLLY_TREE_FOR(schema->child, siter) {
                if (lllyd_check_mandatory_subtree(tree, present->set.d[u], present->set.d[u], siter, 0, options)) {
                    goto error;
                }
            }
        }
        break;

    case LLLYS_CONTAINER:
        if (present->number || !((struct lllys_node_container *)schema)->presence) {
            /* if we have existing or non-presence container, go recursively */
            LLLY_TREE_FOR(schema->child, siter) {
                if (lllyd_check_mandatory_subtree(tree, present->number ? present->set.d[0] : NULL,
                                                present->number ? present->set.d[0] : last_parent,
                                                siter, 0, options)) {
                    goto error;
                }
            }
        }
        break;
    case LLLYS_CHOICE:
        /* get existing node in the data tree from the choice */
        iter = NULL;
        if ((toplevel && tree) || (!toplevel && subtree)) {
            LLLY_TREE_FOR(toplevel ? tree : subtree->child, iter) {
                for (siter = lllys_parent(iter->schema), siter_prev = iter->schema;
                        siter && (siter->nodetype & (LLLYS_CASE | LLLYS_USES | LLLYS_CHOICE));
                        siter_prev = siter, siter = lllys_parent(siter)) {
                    if (siter == schema) {
                        /* we have the choice instance */
                        break;
                    }
                }
                if (siter == schema) {
                    /* we have the choice instance;
                     * the condition must be the same as in the loop because of
                     * choice's sibling nodes that break the loop, so siter is not NULL,
                     * but it is not the same as schema */
                    break;
                }
            }
        }
        if (!iter) {
            if (lllyd_is_when_false(tree, last_parent, schema, options)) {
                /* nothing to check */
                break;
            }
            if (((struct lllys_node_choice *)schema)->dflt) {
                /* there is a default case */
                if (lllyd_check_mandatory_subtree(tree, subtree, last_parent, ((struct lllys_node_choice *)schema)->dflt,
                                                toplevel, options)) {
                    goto error;
                }
            } else if (schema->flags & LLLYS_MAND_TRUE) {
                /* choice requires some data to be instantiated */
                LOGVAL(schema->module->ctx, LLLYE_NOMANDCHOICE, LLLY_VLOG_LYD, last_parent, schema->name);
                goto error;
            }
        } else {
            /* one of the choice's cases is instantiated, continue into this case */
            /* since iter != NULL, siter must be also != NULL and we also know siter_prev
             * which points to the child of schema leading towards the instantiated data */
            assert(siter && siter_prev);
            if (lllyd_check_mandatory_subtree(tree, subtree, last_parent, siter_prev, toplevel, options)) {
                goto error;
            }
        }
        break;
    case LLLYS_NOTIF:
        /* skip if validating a notification */
        if (!(options & LLLYD_OPT_NOTIF)) {
            break;
        }
        /* fallthrough */
    case LLLYS_CASE:
    case LLLYS_USES:
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
        /* go recursively */
        LLLY_TREE_FOR(schema->child, siter) {
            if (lllyd_check_mandatory_subtree(tree, subtree, last_parent, siter, toplevel, options)) {
                goto error;
            }
        }
        break;
    default:
        /* stop */
        break;
    }

    ret = EXIT_SUCCESS;

error:
    llly_set_free(present);
    return ret;
}

int
lllyd_check_mandatory_tree(struct lllyd_node *root, struct llly_ctx *ctx, const struct lllys_module **modules, int mod_count,
                         int options)
{
    struct lllys_node *siter;
    int i;

    assert(root || ctx);
    assert(!(options & LLLYD_OPT_ACT_NOTIF));

    if (options & (LLLYD_OPT_TRUSTED | LLLYD_OPT_EDIT | LLLYD_OPT_GET | LLLYD_OPT_GETCONFIG)) {
        /* no check is needed */
        return EXIT_SUCCESS;
    }

    if (!ctx) {
        /* get context */
        ctx = root->schema->module->ctx;
    }

    if (!(options & LLLYD_OPT_TYPEMASK) || (options & LLLYD_OPT_CONFIG)) {
        if (options & LLLYD_OPT_NOSIBLINGS) {
            if (root && lllyd_check_mandatory_subtree(root, NULL, NULL, root->schema, 1, options)) {
                return EXIT_FAILURE;
            }
        } else if (modules && mod_count) {
            for (i = 0; i < mod_count; ++i) {
                LLLY_TREE_FOR(modules[i]->data, siter) {
                    if (!(siter->nodetype & (LLLYS_RPC | LLLYS_NOTIF)) &&
                            lllyd_check_mandatory_subtree(root, NULL, NULL, siter, 1, options)) {
                        return EXIT_FAILURE;
                    }
                }
            }
        } else {
            for (i = 0; i < ctx->models.used; i++) {
                /* skip not implemented and disabled modules */
                if (!ctx->models.list[i]->implemented || ctx->models.list[i]->disabled) {
                    continue;
                }
                if ((options & LLLYD_OPT_DATA_NO_YANGLIB) && !strcmp(ctx->models.list[i]->name, "ietf-yang-library")) {
                    /* skip ietf-yang-library */
                    continue;
                }
                LLLY_TREE_FOR(ctx->models.list[i]->data, siter) {
                    if (!(siter->nodetype & (LLLYS_RPC | LLLYS_NOTIF)) &&
                            lllyd_check_mandatory_subtree(root, NULL, NULL, siter, 1, options)) {
                        return EXIT_FAILURE;
                    }
                }
            }
        }
    } else if (options & LLLYD_OPT_NOTIF) {
        if (!root || (root->schema->nodetype != LLLYS_NOTIF)) {
            LOGERR(ctx, LLLY_EINVAL, "Subtree is not a single notification.");
            return EXIT_FAILURE;
        }
        if (root->schema->child && lllyd_check_mandatory_subtree(root, root, root, root->schema, 0, options)) {
            return EXIT_FAILURE;
        }
    } else if (options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY)) {
        if (!root || !(root->schema->nodetype & (LLLYS_RPC | LLLYS_ACTION))) {
            LOGERR(ctx, LLLY_EINVAL, "Subtree is not a single RPC/action/reply.");
            return EXIT_FAILURE;
        }
        if (options & LLLYD_OPT_RPC) {
            for (siter = root->schema->child; siter && siter->nodetype != LLLYS_INPUT; siter = siter->next);
        } else { /* LLLYD_OPT_RPCREPLY */
            for (siter = root->schema->child; siter && siter->nodetype != LLLYS_OUTPUT; siter = siter->next);
        }
        if (siter && lllyd_check_mandatory_subtree(root, root, root, siter, 0, options)) {
            return EXIT_FAILURE;
        }
    } else if (options & LLLYD_OPT_DATA_TEMPLATE) {
        if (root && lllyd_check_mandatory_subtree(root, NULL, NULL, root->schema, 1, options)) {
            return EXIT_FAILURE;
        }
    } else {
        LOGINT(ctx);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static struct lllyd_node *
lllyd_parse_(struct llly_ctx *ctx, const struct lllyd_node *rpc_act, const char *data, LLLYD_FORMAT format, int options,
           const struct lllyd_node *data_tree, const char *yang_data_name)
{
    struct lllyxml_elem *xml;
    struct lllyd_node *result = NULL;
    int xmlopt = LLLYXML_PARSE_MULTIROOT;

    if (!ctx || !data) {
        LOGARG;
        return NULL;
    }

    if (options & LLLYD_OPT_NOSIBLINGS) {
        xmlopt = 0;
    }

    /* we must free all the errors, otherwise we are unable to properly check returned llly_errno :-/ */
    llly_errno = LLLY_SUCCESS;
    switch (format) {
    case LLLYD_XML:
        xml = lllyxml_parse_mem(ctx, data, xmlopt);
        if (llly_errno) {
            break;
        }
        if (options & LLLYD_OPT_RPCREPLY) {
            result = lllyd_parse_xml(ctx, &xml, options, rpc_act, data_tree);
        } else if (options & (LLLYD_OPT_RPC | LLLYD_OPT_NOTIF)) {
            result = lllyd_parse_xml(ctx, &xml, options, data_tree);
        } else if (options & LLLYD_OPT_DATA_TEMPLATE) {
            result = lllyd_parse_xml(ctx, &xml, options, yang_data_name);
        } else {
            result = lllyd_parse_xml(ctx, &xml, options);
        }
        lllyxml_free_withsiblings(ctx, xml);
        break;
    case LLLYD_JSON:
        result = lllyd_parse_json(ctx, data, options, rpc_act, data_tree, yang_data_name);
        break;
    case LLLYD_LYB:
        result = lllyd_parse_lyb(ctx, data, options, data_tree, yang_data_name, NULL);
        break;
    default:
        /* error */
        break;
    }

    if (llly_errno) {
        lllyd_free_withsiblings(result);
        return NULL;
    }

    if ((options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY)) && lllyd_schema_sort(result, 1)) {
        /* rpc and rpc-reply must be sorted */
        lllyd_free_withsiblings(result);
        return NULL;
    }

    return result;
}

static struct lllyd_node *
lllyd_parse_data_(struct llly_ctx *ctx, const char *data, LLLYD_FORMAT format, int options, va_list ap)
{
    const struct lllyd_node *rpc_act = NULL, *data_tree = NULL, *iter;
    const char *yang_data_name = NULL;

    if (lllyp_data_check_options(ctx, options, __func__)) {
        return NULL;
    }

    if (options & LLLYD_OPT_RPCREPLY) {
        rpc_act = va_arg(ap, const struct lllyd_node *);
        if (!rpc_act || rpc_act->parent || !(rpc_act->schema->nodetype & (LLLYS_RPC | LLLYS_LIST | LLLYS_CONTAINER))) {
            LOGERR(ctx, LLLY_EINVAL, "%s: invalid variable parameter (const struct lllyd_node *rpc_act).", __func__);
            return NULL;
        }
    }
    if (options & (LLLYD_OPT_RPC | LLLYD_OPT_NOTIF | LLLYD_OPT_RPCREPLY)) {
        data_tree = va_arg(ap, const struct lllyd_node *);
        if (data_tree) {
            if (options & LLLYD_OPT_NOEXTDEPS) {
                LOGERR(ctx, LLLY_EINVAL, "%s: invalid parameter (variable arg const struct lllyd_node *data_tree and LLLYD_OPT_NOEXTDEPS set).",
                       __func__);
                return NULL;
            }

            LLLY_TREE_FOR(data_tree, iter) {
                if (iter->parent) {
                    /* a sibling is not top-level */
                    LOGERR(ctx, LLLY_EINVAL, "%s: invalid variable parameter (const struct lllyd_node *data_tree).", __func__);
                    return NULL;
                }
            }

            /* move it to the beginning */
            for (; data_tree->prev->next; data_tree = data_tree->prev);

            /* LLLYD_OPT_NOSIBLINGS cannot be set in this case */
            if (options & LLLYD_OPT_NOSIBLINGS) {
                LOGERR(ctx, LLLY_EINVAL, "%s: invalid parameter (variable arg const struct lllyd_node *data_tree with LLLYD_OPT_NOSIBLINGS).", __func__);
                return NULL;
            }
        }
    }
    if (options & LLLYD_OPT_DATA_TEMPLATE) {
        yang_data_name = va_arg(ap, const char *);
    }

    return lllyd_parse_(ctx, rpc_act, data, format, options, data_tree, yang_data_name);
}

API struct lllyd_node *
lllyd_parse_mem(struct llly_ctx *ctx, const char *data, LLLYD_FORMAT format, int options, ...)
{
    FUN_IN;

    va_list ap;
    struct lllyd_node *result;

    va_start(ap, options);
    result = lllyd_parse_data_(ctx, data, format, options, ap);
    va_end(ap);

    return result;
}

static struct lllyd_node *
lllyd_parse_fd_(struct llly_ctx *ctx, int fd, LLLYD_FORMAT format, int options, va_list ap)
{
    struct lllyd_node *ret;
    size_t length;
    char *data;

    if (!ctx || (fd == -1)) {
        LOGARG;
        return NULL;
    }

    if (lllyp_mmap(ctx, fd, 0, &length, (void **)&data)) {
        LOGERR(ctx, LLLY_ESYS, "Mapping file descriptor into memory failed (%s()).", __func__);
        return NULL;
    } else if (!data) {
        return NULL;
    }

    ret = lllyd_parse_data_(ctx, data, format, options, ap);

    lllyp_munmap(data, length);

    return ret;
}

API struct lllyd_node *
lllyd_parse_fd(struct llly_ctx *ctx, int fd, LLLYD_FORMAT format, int options, ...)
{
    FUN_IN;

    struct lllyd_node *ret;
    va_list ap;

    va_start(ap, options);
    ret = lllyd_parse_fd_(ctx, fd, format, options, ap);
    va_end(ap);

    return ret;
}

API struct lllyd_node *
lllyd_parse_path(struct llly_ctx *ctx, const char *path, LLLYD_FORMAT format, int options, ...)
{
    FUN_IN;

    int fd;
    struct lllyd_node *ret;
    va_list ap;

    if (!ctx || !path) {
        LOGARG;
        return NULL;
    }

    fd = open(path, O_RDONLY);
    if (fd == -1) {
        LOGERR(ctx, LLLY_ESYS, "Failed to open data file \"%s\" (%s).", path, strerror(errno));
        return NULL;
    }

    va_start(ap, options);
    ret = lllyd_parse_fd_(ctx, fd, format, options, ap);

    va_end(ap);
    close(fd);

    return ret;
}

static struct lllys_node *
lllyd_new_find_schema(struct lllyd_node *parent, const struct lllys_module *module, int rpc_output)
{
    struct lllys_node *siblings;

    if (!parent) {
        siblings = module->data;
    } else {
        if (!parent->schema) {
            return NULL;
        }
        siblings = parent->schema->child;
        if (siblings && (siblings->nodetype == (rpc_output ? LLLYS_INPUT : LLLYS_OUTPUT))) {
            siblings = siblings->next;
        }
        if (siblings && (siblings->nodetype == (rpc_output ? LLLYS_OUTPUT : LLLYS_INPUT))) {
            siblings = siblings->child;
        }
    }

    return siblings;
}

struct lllyd_node *
_lyd_new(struct lllyd_node *parent, const struct lllys_node *schema, int dflt)
{
    struct lllyd_node *ret;

    ret = calloc(1, sizeof *ret);
    LLLY_CHECK_ERR_RETURN(!ret, LOGMEM(schema->module->ctx), NULL);

    ret->schema = (struct lllys_node *)schema;
    ret->validity = llly_new_node_validity(schema);
    if (resolve_applies_when(schema, 0, NULL)) {
        ret->when_status = LLLYD_WHEN;
    }
    ret->prev = ret;
    ret->dflt = dflt;

#ifdef LLLY_ENABLED_CACHE
    lllyd_hash(ret);
#endif

    if (parent) {
        if (lllyd_insert(parent, ret)) {
            lllyd_free(ret);
            return NULL;
        }
    }
    return ret;
}

API struct lllyd_node *
lllyd_new(struct lllyd_node *parent, const struct lllys_module *module, const char *name)
{
    FUN_IN;

    const struct lllys_node *snode = NULL, *siblings;

    if ((!parent && !module) || !name) {
        LOGARG;
        return NULL;
    }

    siblings = lllyd_new_find_schema(parent, module, 0);
    if (!siblings) {
        LOGARG;
        return NULL;
    }

    if (lllys_getnext_data(module, lllys_parent(siblings), name, strlen(name), LLLYS_CONTAINER | LLLYS_LIST | LLLYS_NOTIF
                         | LLLYS_RPC | LLLYS_ACTION, 0, &snode) || !snode) {
        LOGERR(siblings->module->ctx, LLLY_EINVAL, "Failed to find \"%s\" as a sibling to \"%s:%s\".",
               name, lllys_node_module(siblings)->name, siblings->name);
        return NULL;
    }

    return _lyd_new(parent, snode, 0);
}

static struct lllyd_node *
lllyd_create_leaf(const struct lllys_node *schema, const char *val_str, int dflt)
{
    struct lllyd_node_leaf_list *ret;

    ret = calloc(1, sizeof *ret);
    LLLY_CHECK_ERR_RETURN(!ret, LOGMEM(schema->module->ctx), NULL);

    ret->schema = (struct lllys_node *)schema;
    ret->validity = llly_new_node_validity(schema);
    if (resolve_applies_when(schema, 0, NULL)) {
        ret->when_status = LLLYD_WHEN;
    }
    ret->prev = (struct lllyd_node *)ret;
    ret->value_type = ((struct lllys_node_leaf *)schema)->type.base;
    ret->value_str = lllydict_insert(schema->module->ctx, val_str ? val_str : "", 0);
    ret->dflt = dflt;

#ifdef LLLY_ENABLED_CACHE
    lllyd_hash((struct lllyd_node *)ret);
#endif

    return (struct lllyd_node *)ret;
}

static struct lllyd_node *
_lyd_new_leaf(struct lllyd_node *parent, const struct lllys_node *schema, const char *val_str, int dflt, int edit_leaf)
{
    struct lllyd_node *ret;

    ret = lllyd_create_leaf(schema, val_str, dflt);
    if (!ret) {
        return NULL;
    }

    /* connect to parent */
    if (parent) {
        if (lllyd_insert(parent, ret)) {
            lllyd_free(ret);
            return NULL;
        }
    }

    if (edit_leaf && !((struct lllyd_node_leaf_list *)ret)->value_str[0]) {
        /* empty edit leaf, it is fine */
        ((struct lllyd_node_leaf_list *)ret)->value_type = LLLY_TYPE_UNKNOWN;
        return ret;
    }

    /* resolve the type correctly (after it was connected to parent cause of log) */
    if (!lllyp_parse_value(&((struct lllys_node_leaf *)ret->schema)->type, &((struct lllyd_node_leaf_list *)ret)->value_str,
                         NULL, (struct lllyd_node_leaf_list *)ret, NULL, NULL, 1, dflt, 0)) {
        lllyd_free(ret);
        return NULL;
    }

    if ((ret->schema->nodetype == LLLYS_LEAF) && (ret->schema->flags & LLLYS_UNIQUE)) {
        for (; parent && (parent->schema->nodetype != LLLYS_LIST); parent = parent->parent);
        if (parent) {
            parent->validity |= LLLYD_VAL_UNIQUE;
        } else {
            LOGINT(schema->module->ctx);
        }
    }

    return ret;
}

API struct lllyd_node *
lllyd_new_leaf(struct lllyd_node *parent, const struct lllys_module *module, const char *name, const char *val_str)
{
    FUN_IN;

    const struct lllys_node *snode = NULL, *siblings;

    if ((!parent && !module) || !name) {
        LOGARG;
        return NULL;
    }

    siblings = lllyd_new_find_schema(parent, module, 0);
    if (!siblings) {
        LOGARG;
        return NULL;
    }

    if (lllys_getnext_data(module, lllys_parent(siblings), name, strlen(name), LLLYS_LEAFLIST | LLLYS_LEAF, 0, &snode) || !snode) {
        LOGERR(siblings->module->ctx, LLLY_EINVAL, "Failed to find \"%s\" as a sibling to \"%s:%s\".",
               name, lllys_node_module(siblings)->name, siblings->name);
        return NULL;
    }

    return _lyd_new_leaf(parent, snode, val_str, 0, 0);
}

/**
 * @brief Update (add) default flag of the parents of the added node.
 *
 * @param[in] node Added node
 */
static void
lllyd_wd_update_parents(struct lllyd_node *node)
{
    struct lllyd_node *parent = node->parent, *iter;

    for (parent = node->parent; parent; parent = node->parent) {
        if (parent->dflt || parent->schema->nodetype != LLLYS_CONTAINER ||
                ((struct lllys_node_container *)parent->schema)->presence) {
            /* parent is already default and there is nothing to update or
             * it is not a non-presence container -> stop the loop */
            break;
        }
        /* check that there is still some non default sibling */
        for (iter = node->prev; iter != node; iter = iter->prev) {
            if (!iter->dflt) {
                break;
            }
        }
        if (iter == node && node->prev != node) {
            /* all siblings are implicit default nodes, propagate it to the parent */
            node = node->parent;
            node->dflt = 1;
            continue;
        } else {
            /* stop the loop */
            break;
        }
    }
}

static void
check_leaf_list_backlinks(struct lllyd_node *node)
{
    struct lllyd_node *next, *iter;
    struct lllyd_node_leaf_list *leaf_list;
    struct llly_set *set, *data;
    uint32_t i, j;
    int validity_changed = 0;

    /* fix leafrefs */
    LLLY_TREE_DFS_BEGIN(node, next, iter) {
        /* the node is target of a leafref */
        if ((iter->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) && iter->schema->child) {
            set = (struct llly_set *)iter->schema->child;
            for (i = 0; i < set->number; i++) {
                data = lllyd_find_instance(iter, set->set.s[i]);
                if (data) {
                    for (j = 0; j < data->number; j++) {
                        /* invalidate the leafref, a change concerning it happened */
                        leaf_list = (struct lllyd_node_leaf_list *)data->set.d[j];
                        leaf_list->validity |= LLLYD_VAL_LEAFREF;
                        validity_changed = 1;
                        if (leaf_list->value_type == LLLY_TYPE_LEAFREF) {
                            /* remove invalid link and put unresolved value back */
                            lllyp_parse_value(&((struct lllys_node_leaf *)leaf_list->schema)->type, &leaf_list->value_str,
                                            NULL, leaf_list, NULL, NULL, 1, leaf_list->dflt, 0);
                        }
                    }
                    llly_set_free(data);
                } else {
                    LOGINT(node->schema->module->ctx);
                    return;
                }
            }
        }
        LLLY_TREE_DFS_END(node, next, iter)
    }

    /* invalidate parent to make sure it will be checked in future validation */
    if (validity_changed && node->parent) {
        node->parent->validity |= LLLYD_VAL_MAND;
    }
}

API int
lllyd_change_leaf(struct lllyd_node_leaf_list *leaf, const char *val_str)
{
    FUN_IN;

    const char *backup;
    int val_change, dflt_change;
    struct lllyd_node *parent;

    if (!leaf || (leaf->schema->nodetype != LLLYS_LEAF)) {
        LOGARG;
        return -1;
    }

    backup = leaf->value_str;
    leaf->value_str = lllydict_insert(leaf->schema->module->ctx, val_str ? val_str : "", 0);
    /* leaf->value is erased by lllyp_parse_value() */

    /* parse the type correctly, makes the value canonical if needed */
    if (!lllyp_parse_value(&((struct lllys_node_leaf *)leaf->schema)->type, &leaf->value_str, NULL, leaf, NULL, NULL, 1, 0, 0)) {
        lllydict_remove(leaf->schema->module->ctx, backup);
        return -1;
    }

    if (!strcmp(backup, leaf->value_str)) {
        /* the value remains the same */
        val_change = 0;
    } else {
        val_change = 1;
    }

    /* value is correct, remove backup */
    lllydict_remove(leaf->schema->module->ctx, backup);

    /* clear the default flag, the value is different */
    if (leaf->dflt) {
        for (parent = (struct lllyd_node *)leaf; parent; parent = parent->parent) {
            parent->dflt = 0;
        }
        dflt_change = 1;
    } else {
        dflt_change = 0;
    }

    if (val_change) {
        /* make the node non-validated */
        leaf->validity = llly_new_node_validity(leaf->schema);

        /* check possible leafref backlinks */
        check_leaf_list_backlinks((struct lllyd_node *)leaf);
    }

    if (val_change && (leaf->schema->flags & LLLYS_UNIQUE)) {
        for (parent = leaf->parent; parent && (parent->schema->nodetype != LLLYS_LIST); parent = parent->parent);
        if (parent) {
            parent->validity |= LLLYD_VAL_UNIQUE;
        }
    }

    return (val_change || dflt_change ? 0 : 1);
}

static struct lllyd_node *
lllyd_create_anydata(struct lllyd_node *parent, const struct lllys_node *schema, void *value,
                   LLLYD_ANYDATA_VALUETYPE value_type)
{
    struct lllyd_node *iter;
    struct lllyd_node_anydata *ret;
    int len;

    ret = calloc(1, sizeof *ret);
    LLLY_CHECK_ERR_RETURN(!ret, LOGMEM(schema->module->ctx), NULL);

    ret->schema = (struct lllys_node *)schema;
    ret->validity = llly_new_node_validity(schema);
    if (resolve_applies_when(schema, 0, NULL)) {
        ret->when_status = LLLYD_WHEN;
    }
    ret->prev = (struct lllyd_node *)ret;

    /* set the value */
    switch (value_type) {
    case LLLYD_ANYDATA_CONSTSTRING:
    case LLLYD_ANYDATA_SXML:
    case LLLYD_ANYDATA_JSON:
        ret->value.str = lllydict_insert(schema->module->ctx, (const char *)value, 0);
        break;
    case LLLYD_ANYDATA_STRING:
    case LLLYD_ANYDATA_SXMLD:
    case LLLYD_ANYDATA_JSOND:
        ret->value.str = lllydict_insert_zc(schema->module->ctx, (char *)value);
        value_type &= ~LLLYD_ANYDATA_STRING; /* make const string from string */
        break;
    case LLLYD_ANYDATA_DATATREE:
        ret->value.tree = (struct lllyd_node *)value;
        break;
    case LLLYD_ANYDATA_XML:
        ret->value.xml = (struct lllyxml_elem *)value;
        break;
    case LLLYD_ANYDATA_LYB:
        len = lllyd_lyb_data_length(value);
        if (len == -1) {
            LOGERR(schema->module->ctx, LLLY_EINVAL, "Invalid LLLYB data.");
            return NULL;
        }
        ret->value.mem = malloc(len);
        LLLY_CHECK_ERR_RETURN(!ret->value.mem, LOGMEM(schema->module->ctx); free(ret), NULL);
        memcpy(ret->value.mem, value, len);
        break;
    case LLLYD_ANYDATA_LYBD:
        ret->value.mem = value;
        value_type &= ~LLLYD_ANYDATA_STRING; /* make const string from string */
        break;
    }
    ret->value_type = value_type;

#ifdef LLLY_ENABLED_CACHE
    lllyd_hash((struct lllyd_node *)ret);
#endif

    /* connect to parent */
    if (parent) {
        if (lllyd_insert(parent, (struct lllyd_node*)ret)) {
            lllyd_free((struct lllyd_node*)ret);
            return NULL;
        }

        /* remove the flag from parents */
        for (iter = parent; iter && iter->dflt; iter = iter->parent) {
            iter->dflt = 0;
        }
    }

    return (struct lllyd_node*)ret;
}

API struct lllyd_node *
lllyd_new_anydata(struct lllyd_node *parent, const struct lllys_module *module, const char *name,
                void *value, LLLYD_ANYDATA_VALUETYPE value_type)
{
    FUN_IN;

    const struct lllys_node *siblings, *snode;

    if ((!parent && !module) || !name) {
        LOGARG;
        return NULL;
    }

    siblings = lllyd_new_find_schema(parent, module, 0);
    if (!siblings) {
        LOGARG;
        return NULL;
    }

    if (lllys_getnext_data(module, lllys_parent(siblings), name, strlen(name), LLLYS_ANYDATA, 0, &snode) || !snode) {
        LOGERR(siblings->module->ctx, LLLY_EINVAL, "Failed to find \"%s\" as a sibling to \"%s:%s\".",
               name, lllys_node_module(siblings)->name, siblings->name);
        return NULL;
    }

    return lllyd_create_anydata(parent, snode, value, value_type);
}

API struct lllyd_node *
lllyd_new_yangdata(const struct lllys_module *module, const char *name_template, const char *name)
{
    FUN_IN;

    const struct lllys_node *schema = NULL, *snode;

    if (!module || !name_template || !name) {
        LOGARG;
        return NULL;
    }

    schema = lllyp_get_yang_data_template(module, name_template, strlen(name_template));
    if (!schema) {
        LOGERR(module->ctx, LLLY_EINVAL, "Failed to find yang-data template \"%s\".", name_template);
        return NULL;
    }

    if (lllys_getnext_data(module, schema, name, strlen(name), LLLYS_CONTAINER, 0, &snode) || !snode) {
        LOGERR(module->ctx, LLLY_EINVAL, "Failed to find \"%s\" as a container child of \"%s:%s\".",
               name, module->name, schema->name);
        return NULL;
    }

    return _lyd_new(NULL, snode, 0);
}

API struct lllyd_node *
lllyd_new_output(struct lllyd_node *parent, const struct lllys_module *module, const char *name)
{
    FUN_IN;

    const struct lllys_node *snode = NULL, *siblings;

    if ((!parent && !module) || !name) {
        LOGARG;
        return NULL;
    }

    siblings = lllyd_new_find_schema(parent, module, 1);
    if (!siblings) {
        LOGARG;
        return NULL;
    }

    if (lllys_getnext_data(module, lllys_parent(siblings), name, strlen(name), LLLYS_CONTAINER | LLLYS_LIST | LLLYS_NOTIF
                         | LLLYS_RPC | LLLYS_ACTION, 0, &snode) || !snode) {
        LOGERR(siblings->module->ctx, LLLY_EINVAL, "Failed to find \"%s\" as a sibling to \"%s:%s\".",
               name, lllys_node_module(siblings)->name, siblings->name);
        return NULL;
    }

    return _lyd_new(parent, snode, 0);
}

API struct lllyd_node *
lllyd_new_output_leaf(struct lllyd_node *parent, const struct lllys_module *module, const char *name, const char *val_str)
{
    FUN_IN;

    const struct lllys_node *snode = NULL, *siblings;

    if ((!parent && !module) || !name) {
        LOGARG;
        return NULL;
    }

    siblings = lllyd_new_find_schema(parent, module, 1);
    if (!siblings) {
        LOGARG;
        return NULL;
    }

    if (lllys_getnext_data(module, lllys_parent(siblings), name, strlen(name), LLLYS_LEAFLIST | LLLYS_LEAF, 0, &snode) || !snode) {
        LOGERR(siblings->module->ctx, LLLY_EINVAL, "Failed to find \"%s\" as a sibling to \"%s:%s\".",
               name, lllys_node_module(siblings)->name, siblings->name);
        return NULL;
    }

    return _lyd_new_leaf(parent, snode, val_str, 0, 0);
}

API struct lllyd_node *
lllyd_new_output_anydata(struct lllyd_node *parent, const struct lllys_module *module, const char *name,
                       void *value, LLLYD_ANYDATA_VALUETYPE value_type)
{
    FUN_IN;

    const struct lllys_node *siblings, *snode;

    if ((!parent && !module) || !name) {
        LOGARG;
        return NULL;
    }

    siblings = lllyd_new_find_schema(parent, module, 1);
    if (!siblings) {
        LOGARG;
        return NULL;
    }

    if (lllys_getnext_data(module, lllys_parent(siblings), name, strlen(name), LLLYS_ANYDATA, 0, &snode) || !snode) {
        LOGERR(siblings->module->ctx, LLLY_EINVAL, "Failed to find \"%s\" as a sibling to \"%s:%s\".",
               name, lllys_node_module(siblings)->name, siblings->name);
        return NULL;
    }

    return lllyd_create_anydata(parent, snode, value, value_type);
}

char *
lllyd_make_canonical(const struct lllys_node *schema, const char *val_str, int val_str_len)
{
    struct lllyd_node *node;
    char *str;

    assert(schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST));

    str = strndup(val_str, val_str_len);
    if (!str) {
        LOGMEM(schema->module->ctx);
        return NULL;
    }

    node = lllyd_create_leaf(schema, str, 0);
    free(str);
    if (!node) {
        return NULL;
    }

    /* parse the value into a fake leaf */
    if (!lllyp_parse_value(&((struct lllys_node_leaf *)node->schema)->type, &((struct lllyd_node_leaf_list *)node)->value_str,
                         NULL, (struct lllyd_node_leaf_list *)node, NULL, NULL, 1, 0, 0)) {
        lllyd_free(node);
        return NULL;
    }

    str = strdup(((struct lllyd_node_leaf_list *)node)->value_str);
    lllyd_free(node);
    if (!str) {
        LOGMEM(schema->module->ctx);
        return NULL;
    }

    return str;
}

static int
lllyd_new_path_list_predicate(struct lllyd_node *list, const char *list_name, const char *predicate, int *parsed)
{
    const char *mod_name, *name, *value;
    char *key_val;
    int r, i, mod_name_len, nam_len, val_len, has_predicate;
    struct lllys_node_list *slist;
    struct lllys_node *key;

    slist = (struct lllys_node_list *)list->schema;

    /* is the predicate a number? */
    if (((r = parse_schema_json_predicate(predicate, &mod_name, &mod_name_len, &name, &nam_len, &value, &val_len, &has_predicate)) < 1)
            || !strncmp(name, ".", nam_len)) {
        LOGVAL(slist->module->ctx, LLLYE_PATH_INCHAR, LLLY_VLOG_NONE, NULL, predicate[-r], &predicate[-r]);
        return -1;
    }

    if (isdigit(name[0])) {
        /* position index - creating without keys */
        *parsed += r;
        return 0;
    }

    /* it's not a number, so there must be some keys */
    if (!slist->keys_size) {
        /* there are none, so pretend we did not parse anything to get invalid char error later */
        return 0;
    }

    /* go through all the keys */
    i = 0;
    goto check_parsed_values;

    for (; i < slist->keys_size; ++i) {
        if (!has_predicate) {
            LOGVAL(slist->module->ctx, LLLYE_PATH_MISSKEY, LLLY_VLOG_NONE, NULL, list_name);
            return -1;
        }

        if (((r = parse_schema_json_predicate(predicate, &mod_name, &mod_name_len, &name, &nam_len, &value, &val_len, &has_predicate)) < 1)
                || !strncmp(name, ".", nam_len)) {
            LOGVAL(slist->module->ctx, LLLYE_PATH_INCHAR, LLLY_VLOG_NONE, NULL, predicate[-r], &predicate[-r]);
            return -1;
        }

check_parsed_values:
        key = (struct lllys_node *)slist->keys[i];
        *parsed += r;
        predicate += r;

        if (!value || (!mod_name && (lllys_node_module(key) != lllys_node_module((struct lllys_node *)slist)))
                || (mod_name && (strncmp(lllys_node_module(key)->name, mod_name, mod_name_len) || lllys_node_module(key)->name[mod_name_len]))
                || strncmp(key->name, name, nam_len) || key->name[nam_len]) {
            LOGVAL(slist->module->ctx, LLLYE_PATH_INKEY, LLLY_VLOG_NONE, NULL, name);
            return -1;
        }

        key_val = malloc((val_len + 1) * sizeof(char));
        LLLY_CHECK_ERR_RETURN(!key_val, LOGMEM(slist->module->ctx), -1);
        strncpy(key_val, value, val_len);
        key_val[val_len] = '\0';

        if (!_lyd_new_leaf(list, key, key_val, 0, 0)) {
            free(key_val);
            return -1;
        }
        free(key_val);
    }

    return 0;
}

static struct lllyd_node *
lllyd_new_path_update(struct lllyd_node *node, void *value, LLLYD_ANYDATA_VALUETYPE value_type, int dflt)
{
    struct llly_ctx *ctx = node->schema->module->ctx;
    struct lllyd_node_anydata *any;
    int len;

    switch (node->schema->nodetype) {
    case LLLYS_LEAF:
        if (value_type > LLLYD_ANYDATA_STRING) {
            LOGARG;
            return NULL;
        }

        if (lllyd_change_leaf((struct lllyd_node_leaf_list *)node, value) == 0) {
            /* there was an actual change */
            if (dflt) {
                node->dflt = 1;
            }
            return node;
        }

        if (dflt) {
            /* maybe the value is the same, but the node is default now */
            node->dflt = 1;
            return node;
        }

        break;
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        /* the nodes are the same if:
         * 1) the value types are strings (LLLYD_ANYDATA_STRING and LLLYD_ANYDATA_CONSTSTRING equals)
         *    and the strings equals
         * 2) the value types are the same, but not strings and the pointers (not the content) are the
         *    same
         */
        any = (struct lllyd_node_anydata *)node;
        if (any->value_type <= LLLYD_ANYDATA_STRING && value_type <= LLLYD_ANYDATA_STRING) {
            if (llly_strequal(any->value.str, (char *)value, 0)) {
                /* values are the same */
                return NULL;
            }
        } else if (any->value_type == value_type) {
            /* compare pointers */
            if ((void *)any->value.tree == value) {
                /* values are the same */
                return NULL;
            }
        }

        /* values are not the same - 1) remove the old one ... */
        switch (any->value_type) {
        case LLLYD_ANYDATA_CONSTSTRING:
        case LLLYD_ANYDATA_SXML:
        case LLLYD_ANYDATA_JSON:
            lllydict_remove(ctx, any->value.str);
            break;
        case LLLYD_ANYDATA_DATATREE:
            lllyd_free_withsiblings(any->value.tree);
            break;
        case LLLYD_ANYDATA_XML:
            lllyxml_free_withsiblings(ctx, any->value.xml);
            break;
        case LLLYD_ANYDATA_LYB:
            free(any->value.mem);
            break;
        case LLLYD_ANYDATA_STRING:
        case LLLYD_ANYDATA_SXMLD:
        case LLLYD_ANYDATA_JSOND:
        case LLLYD_ANYDATA_LYBD:
            /* dynamic strings are used only as input parameters */
            assert(0);
            break;
        }
        /* ... and 2) store the new one */
        switch (value_type) {
        case LLLYD_ANYDATA_CONSTSTRING:
        case LLLYD_ANYDATA_SXML:
        case LLLYD_ANYDATA_JSON:
            any->value.str = lllydict_insert(ctx, (const char *)value, 0);
            break;
        case LLLYD_ANYDATA_STRING:
        case LLLYD_ANYDATA_SXMLD:
        case LLLYD_ANYDATA_JSOND:
            any->value.str = lllydict_insert_zc(ctx, (char *)value);
            value_type &= ~LLLYD_ANYDATA_STRING; /* make const string from string */
            break;
        case LLLYD_ANYDATA_DATATREE:
            any->value.tree = value;
            break;
        case LLLYD_ANYDATA_XML:
            any->value.xml = value;
            break;
        case LLLYD_ANYDATA_LYB:
            len = lllyd_lyb_data_length(value);
            if (len == -1) {
                LOGERR(ctx, LLLY_EINVAL, "Invalid LLLYB data.");
                return NULL;
            }
            any->value.mem = malloc(len);
            LLLY_CHECK_ERR_RETURN(!any->value.mem, LOGMEM(ctx), NULL);
            memcpy(any->value.mem, value, len);
            break;
        case LLLYD_ANYDATA_LYBD:
            any->value.mem = value;
            value_type &= ~LLLYD_ANYDATA_STRING; /* make const string from string */
            break;
        }
        return node;
    default:
        /* nothing needed - containers, lists and leaf-lists do not have value or it cannot be changed */
        break;
    }

    /* not updated */
    return NULL;
}

API struct lllyd_node *
lllyd_new_path(struct lllyd_node *data_tree, const struct llly_ctx *ctx, const char *path, void *value,
             LLLYD_ANYDATA_VALUETYPE value_type, int options)
{
    FUN_IN;

    char *str;
    const char *mod_name, *name, *val_name, *val, *node_mod_name, *id, *backup_mod_name = NULL, *yang_data_name = NULL;
    struct lllyd_node *ret = NULL, *node, *parent = NULL;
    const struct lllys_node *schild, *sparent, *tmp;
    const struct lllys_node_list *slist;
    const struct lllys_module *module, *prev_mod;
    int r, i, parsed = 0, mod_name_len, nam_len, val_name_len, val_len;
    int is_relative = -1, has_predicate, first_iter = 1, edit_leaf;
    int backup_is_relative, backup_mod_name_len, yang_data_name_len;

    if (!path || (!data_tree && !ctx)
            || (!data_tree && (path[0] != '/'))) {
        LOGARG;
        return NULL;
    }

    if (!ctx) {
        ctx = data_tree->schema->module->ctx;
    }

    id = path;

    if (data_tree) {
        if (path[0] == '/') {
            /* absolute path, go through all the siblings and try to find the right parent, if exists,
             * first go through all the next siblings keeping the original order, for positional predicates */
            for (node = data_tree; !parsed && node; node = node->next) {
                parent = resolve_partial_json_data_nodeid(id, value_type > LLLYD_ANYDATA_STRING ? NULL : value, node,
                                                          options, &parsed);
            }
            if (!parsed) {
                for (node = data_tree->prev; !parsed && node->next; node = node->prev) {
                    parent = resolve_partial_json_data_nodeid(id, value_type > LLLYD_ANYDATA_STRING ? NULL : value, node,
                                                              options, &parsed);
                }
            }
        } else {
            /* relative path, use only the provided data tree root */
            parent = resolve_partial_json_data_nodeid(id, value_type > LLLYD_ANYDATA_STRING ? NULL : value, data_tree,
                                                      options, &parsed);
        }
        if (parsed == -1) {
            return NULL;
        }
        if (parsed) {
            assert(parent);
            /* if we parsed something we have a relative path now for sure, otherwise we don't know */
            is_relative = 1;

            id += parsed;

            if (!id[0]) {
                /* the node exists, are we supposed to update it or is it default? */
                if (!(options & LLLYD_PATH_OPT_UPDATE) && (!parent->dflt || (options & LLLYD_PATH_OPT_DFLT))) {
                    LOGVAL(ctx, LLLYE_PATH_EXISTS, LLLY_VLOG_STR, path);
                    return NULL;
                }

                /* no change, the default node already exists */
                if (parent->dflt && (options & LLLYD_PATH_OPT_DFLT)) {
                    return NULL;
                }

                return lllyd_new_path_update(parent, value, value_type, options & LLLYD_PATH_OPT_DFLT);
            }
        }
    }

    backup_is_relative = is_relative;
    if ((r = parse_schema_nodeid(id, &mod_name, &mod_name_len, &name, &nam_len, &is_relative, NULL, NULL, 1)) < 1) {
        LOGVAL(ctx, LLLYE_PATH_INCHAR, LLLY_VLOG_NONE, NULL, id[-r], &id[-r]);
        return NULL;
    }

    if (name[0] == '#') {
        if (is_relative) {
            LOGVAL(ctx, LLLYE_PATH_INCHAR, LLLY_VLOG_NONE, NULL, '#', name);
            return NULL;
        }
        yang_data_name = name + 1;
        yang_data_name_len = nam_len - 1;
        backup_mod_name = mod_name;
        backup_mod_name_len = mod_name_len;
        /* move to the next node in the path */
        id += r;
    } else {
        is_relative = backup_is_relative;
    }

    if ((r = parse_schema_nodeid(id, &mod_name, &mod_name_len, &name, &nam_len, &is_relative, &has_predicate, NULL, 0)) < 1) {
        LOGVAL(ctx, LLLYE_PATH_INCHAR, LLLY_VLOG_NONE, NULL, id[-r], &id[-r]);
        return NULL;
    }
    /* move to the next node in the path */
    id += r;

    if (backup_mod_name) {
        mod_name = backup_mod_name;
        mod_name_len = backup_mod_name_len;
    }

    /* prepare everything for the schema search loop */
    if (is_relative) {
        /* we are relative to data_tree or parent if some part of the path already exists */
        if (!data_tree) {
            LOGERR(ctx, LLLY_EINVAL, "%s: provided relative path (%s) without context node.", __func__, path);
            return NULL;
        } else if (!parent) {
            parent = data_tree;
        }
        sparent = parent->schema;
        module = prev_mod = lllys_node_module(sparent);
    } else {
        /* we are starting from scratch, absolute path */
        assert(!parent);
        if (!mod_name) {
            str = strndup(path, (name + nam_len) - path);
            LOGVAL(ctx, LLLYE_PATH_MISSMOD, LLLY_VLOG_STR, str);
            free(str);
            return NULL;
        }

        module = llly_ctx_nget_module(ctx, mod_name, mod_name_len, NULL, 1);

        if (!module) {
            str = strndup(path, (mod_name + mod_name_len) - path);
            LOGVAL(ctx, LLLYE_PATH_INMOD, LLLY_VLOG_STR, str);
            free(str);
            return NULL;
        }
        mod_name = NULL;
        mod_name_len = 0;
        prev_mod = module;

        sparent = NULL;
        if (yang_data_name) {
            sparent = lllyp_get_yang_data_template(module, yang_data_name, yang_data_name_len);
            if (!sparent) {
                str = strndup(path, (yang_data_name + yang_data_name_len) - path);
                LOGVAL(ctx, LLLYE_PATH_INNODE, LLLY_VLOG_STR, str);
                free(str);
                return NULL;
            }
        }
    }

    /* create nodes in a loop */
    while (1) {
        /* find the schema node */
        schild = NULL;
        while ((schild = lllys_getnext(schild, sparent, module, 0))) {
            if (schild->nodetype & (LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST
                                    | LLLYS_ANYDATA | LLLYS_NOTIF | LLLYS_RPC | LLLYS_ACTION)) {
                /* module comparison */
                if (mod_name) {
                    node_mod_name = lllys_node_module(schild)->name;
                    if (strncmp(node_mod_name, mod_name, mod_name_len) || node_mod_name[mod_name_len]) {
                        continue;
                    }
                } else if (lllys_node_module(schild) != prev_mod) {
                    continue;
                }

                /* name check */
                if (strncmp(schild->name, name, nam_len) || schild->name[nam_len]) {
                    continue;
                }

                /* RPC/action in/out check */
                for (tmp = lllys_parent(schild); tmp && (tmp->nodetype == LLLYS_USES); tmp = lllys_parent(tmp));
                if (tmp) {
                    if (options & LLLYD_PATH_OPT_OUTPUT) {
                        if (tmp->nodetype == LLLYS_INPUT) {
                            continue;
                        }
                    } else {
                        if (tmp->nodetype == LLLYS_OUTPUT) {
                            continue;
                        }
                    }
                }

                break;
            }
        }

        if (!schild) {
            str = strndup(path, (name + nam_len) - path);
            LOGVAL(ctx, LLLYE_PATH_INNODE, LLLY_VLOG_STR, str);
            free(str);
            lllyd_free(ret);
            return NULL;
        }

        /* we have the right schema node */
        switch (schild->nodetype) {
        case LLLYS_CONTAINER:
        case LLLYS_LIST:
        case LLLYS_NOTIF:
        case LLLYS_RPC:
        case LLLYS_ACTION:
            if (options & LLLYD_PATH_OPT_NOPARENT) {
                /* these were supposed to exist */
                str = strndup(path, (name + nam_len) - path);
                LOGVAL(ctx, LLLYE_PATH_MISSPAR, LLLY_VLOG_STR, str);
                free(str);
                lllyd_free(ret);
                return NULL;
            }
            node = _lyd_new(is_relative ? parent : NULL, schild, (options & LLLYD_PATH_OPT_DFLT) ? 1 : 0);
            break;
        case LLLYS_LEAF:
        case LLLYS_LEAFLIST:
            str = NULL;
            if (has_predicate) {
                if ((r = parse_schema_json_predicate(id, NULL, NULL, &val_name, &val_name_len, &val, &val_len, &has_predicate)) < 1) {
                    LOGVAL(ctx, LLLYE_PATH_INCHAR, LLLY_VLOG_NONE, NULL, id[-r], &id[-r]);
                    lllyd_free(ret);
                    return NULL;
                }
                id += r;

                if ((val_name[0] != '.') || (val_name_len != 1)) {
                    LOGVAL(ctx, LLLYE_PATH_INCHAR, LLLY_VLOG_NONE, NULL, val_name[0], val_name);
                    lllyd_free(ret);
                    return NULL;
                }

                str = strndup(val, val_len);
                if (!str) {
                    LOGMEM(ctx);
                    lllyd_free(ret);
                    return NULL;
                }
            }
            if (id[0]) {
                LOGVAL(ctx, LLLYE_PATH_INCHAR, LLLY_VLOG_NONE, NULL, id[0], id);
                free(str);
                lllyd_free(ret);
                return NULL;
            }

            if ((options & LLLYD_PATH_OPT_EDIT) && schild->nodetype == LLLYS_LEAF) {
                edit_leaf = 1;
            } else {
                edit_leaf = 0;
            }
            node = _lyd_new_leaf(is_relative ? parent : NULL, schild, (str ? str : value),
                                 (options & LLLYD_PATH_OPT_DFLT) ? 1 : 0, edit_leaf);
            free(str);
            break;
        case LLLYS_ANYXML:
        case LLLYS_ANYDATA:
            if (id[0]) {
                LOGVAL(ctx, LLLYE_PATH_INCHAR, LLLY_VLOG_NONE, NULL, id[0], id);
                lllyd_free(ret);
                return NULL;
            }
            if (value_type <= LLLYD_ANYDATA_STRING && !value) {
                value_type = LLLYD_ANYDATA_CONSTSTRING;
                value = "";
            }
            node = lllyd_create_anydata(is_relative ? parent : NULL, schild, value, value_type);
            break;
        default:
            LOGINT(ctx);
            node = NULL;
            break;
        }

        if (!node) {
            str = strndup(path, id - path);
            if (is_relative) {
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_STR, str, "Failed to create node \"%s\" as a child of \"%s\".",
                       schild->name, parent->schema->name);
            } else {
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_STR, str, "Failed to create node \"%s\".", schild->name);
            }
            free(str);
            lllyd_free(ret);
            return NULL;
        }
        /* special case when we are creating a sibling of a top-level data node */
        if (!is_relative) {
            if (data_tree) {
                for (; data_tree->next; data_tree = data_tree->next);
                if (lllyd_insert_after(data_tree, node)) {
                    lllyd_free(ret);
                    return NULL;
                }
            }
            is_relative = 1;
        }

        if (first_iter) {
            /* sort if needed, but only when inserted somewhere */
            sparent = node->schema;
            do {
                sparent = lllys_parent(sparent);
            } while (sparent && (sparent->nodetype != ((options & LLLYD_PATH_OPT_OUTPUT) ? LLLYS_OUTPUT : LLLYS_INPUT)));
            if (sparent && lllyd_schema_sort(node, 0)) {
                lllyd_free(ret);
                return NULL;
            }

            /* set first created node */
            ret = node;
            first_iter = 0;
        }

        parsed = 0;
        if ((schild->nodetype == LLLYS_LIST) && has_predicate && lllyd_new_path_list_predicate(node, name, id, &parsed)) {
            lllyd_free(ret);
            return NULL;
        }
        id += parsed;

        if (!id[0]) {
            /* we are done */
            if (options & LLLYD_PATH_OPT_NOPARENTRET) {
                /* last created node */
                return node;
            }
            return ret;
        }

        /* prepare for another iteration */
        parent = node;
        sparent = schild;
        prev_mod = lllys_node_module(schild);

        /* parse another node */
        if ((r = parse_schema_nodeid(id, &mod_name, &mod_name_len, &name, &nam_len, &is_relative, &has_predicate, NULL, 0)) < 1) {
            LOGVAL(ctx, LLLYE_PATH_INCHAR, LLLY_VLOG_NONE, NULL, id[-r], &id[-r]);
            lllyd_free(ret);
            return NULL;
        }
        id += r;

        /* if a key of a list was supposed to be created, it is created as a part of the list instance creation */
        if ((schild->nodetype == LLLYS_LIST) && !mod_name) {
            slist = (const struct lllys_node_list *)schild;
            for (i = 0; i < slist->keys_size; ++i) {
                if (!strncmp(slist->keys[i]->name, name, nam_len) && !slist->keys[i]->name[nam_len]) {
                    /* the path continues? there cannot be anything after a key (leaf) */
                    if (id[0]) {
                        LOGVAL(ctx, LLLYE_PATH_INCHAR, LLLY_VLOG_NONE, NULL, id[0], id);
                        lllyd_free(ret);
                        return NULL;
                    }
                    return ret;
                }
            }
        }
    }

    LOGINT(ctx);
    return NULL;
}

API unsigned int
lllyd_list_pos(const struct lllyd_node *node)
{
    FUN_IN;

    unsigned int pos;
    struct lllys_node *schema;

    if (!node || ((node->schema->nodetype != LLLYS_LIST) && (node->schema->nodetype != LLLYS_LEAFLIST))) {
        return 0;
    }

    schema = node->schema;
    pos = 0;
    do {
        if (node->schema == schema) {
            ++pos;
        }
        node = node->prev;
    } while (node->next);

    return pos;
}

static struct lllyd_node *
lllyd_new_dummy(struct lllyd_node *root, struct lllyd_node *parent, const struct lllys_node *schema, const char *value, int dflt)
{
    unsigned int index;
    struct llly_set *spath;
    const struct lllys_node *siter;
    struct lllyd_node *iter, *dummy = NULL;

    assert(schema);
    assert(schema->nodetype & (LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LIST | LLLYS_LEAFLIST | LLLYS_ANYDATA | LLLYS_NOTIF |
                               LLLYS_RPC | LLLYS_ACTION));

    spath = llly_set_new();
    if (!spath) {
        LOGMEM(schema->module->ctx);
        return NULL;
    }

    if (!parent && root) {
        /* find data root */
        for (; root->parent; root = root->parent);   /* vertical move (up) */
        for (; root->prev->next; root = root->prev); /* horizontal move (left) */
    }

    /* build schema path */
    for (siter = schema; siter; siter = lllys_parent(siter)) {
        /* stop if we know some of the parents */
        if (parent && parent->schema == siter) {
            break;
        }

        if (siter->nodetype & (LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LIST | LLLYS_LEAFLIST | LLLYS_ANYDATA | LLLYS_NOTIF |
                               LLLYS_RPC | LLLYS_ACTION)) {
            /* we have a node that can appear in data tree */
            llly_set_add(spath, (void*)siter, LLLY_SET_OPT_USEASLIST);
        } /* else skip the rest node types */
    }

    assert(spath->number > 0);
    index = spath->number;
    if (!parent && !(spath->set.s[index - 1]->nodetype & LLLYS_LEAFLIST)) {
        /* start by searching for the top-level parent */
        LLLY_TREE_FOR(root, iter) {
            if (iter->schema == spath->set.s[index - 1]) {
                parent = iter;
                index--;
                break;
            }
        }
    }

    iter = parent;
    while (iter && index && !(spath->set.s[index - 1]->nodetype & LLLYS_LEAFLIST)) {
        /* search for closer parent on the path */
        LLLY_TREE_FOR(parent->child, iter) {
            if (iter->schema == spath->set.s[index - 1]) {
                index--;
                parent = iter;
                break;
            }
        }
    }
    while(index) {
        /* create the missing part of the path */
        switch (spath->set.s[index - 1]->nodetype) {
        case LLLYS_LEAF:
        case LLLYS_LEAFLIST:
            if (value) {
                iter = _lyd_new_leaf(parent, spath->set.s[index - 1], value, dflt, 0);
            } else {
                iter = lllyd_create_leaf(spath->set.s[index - 1], value, dflt);
                if (iter && parent) {
                    if (lllyd_insert(parent, iter)) {
                        lllyd_free(iter);
                        goto error;
                    }
                }
            }
            break;
        case LLLYS_CONTAINER:
        case LLLYS_LIST:
            iter = _lyd_new(parent, spath->set.s[index - 1], dflt);
            break;
        case LLLYS_ANYXML:
        case LLLYS_ANYDATA:
            iter = lllyd_create_anydata(parent, spath->set.s[index - 1], "", LLLYD_ANYDATA_CONSTSTRING);
            break;
        default:
            goto error;
        }
        if (!iter) {
            LOGINT(schema->module->ctx);
            goto error;
        }

        /* we say it is valid and it is dummy */
        iter->validity = LLLYD_VAL_INUSE;

        if (!dummy) {
            dummy = iter;
        }

        /* continue */
        parent = iter;
        index--;
    }

    llly_set_free(spath);

    return dummy;

error:
    llly_set_free(spath);
    lllyd_free(dummy);
    return NULL;
}

static struct lllys_node *
lllys_get_schema_inctx(struct lllys_node *schema, struct llly_ctx *ctx)
{
    const struct lllys_module *mod, *trg_mod = NULL;
    struct lllys_node *parent, *first_sibling = NULL, *iter = NULL;
    struct llly_set *parents;
    unsigned int index;
    uint32_t idx;
    void **ptr;

    if (!ctx || schema->module->ctx == ctx) {
        /* we have the same context */
        return schema;
    }

    /* store the parents chain */
    parents = llly_set_new();
    for (parent = schema; parent; parent = lllys_parent(parent)) {
        /* note - augments are skipped so we will work only with the implemented modules
         * (where the augments are applied) */
        if (parent->nodetype != LLLYS_USES) {
            llly_set_add(parents, parent, LLLY_SET_OPT_USEASLIST);
        }
    }
    assert(parents->number);
    index = parents->number - 1;

    /* process the parents from the top level */
    /* for the top-level node, we have to locate the module first */
    parent = parents->set.s[index];
    if (parent->nodetype == LLLYS_EXT) {
        ptr = lllys_ext_complex_get_substmt(LLLY_STMT_NODE, (struct lllys_ext_instance_complex *)parent, NULL);
        if (!ptr) {
            llly_set_free(parents);
            return NULL;
        }
        first_sibling = *(struct lllys_node **)ptr;
        parent = parents->set.s[--index];
    }
    idx = 0;
    while ((mod = llly_ctx_get_module_iter(ctx, &idx))) {
        trg_mod = lllys_node_module(parent);
        /* check module name */
        if (strcmp(mod->name, trg_mod->name)) {
            continue;
        }

        /* check revision */
        if ((!mod->rev_size && !trg_mod->rev_size) ||
                (mod->rev_size && trg_mod->rev_size && !strcmp(mod->rev[0].date, trg_mod->rev[0].date))) {
            /* we have match */
            break;
        }
    }
    /* try data callback */
    if (!mod && trg_mod && ctx->data_clb) {
        LOGDBG(LLLY_LDGYANG, "Attempting to load '%s' into context using callback ...", trg_mod->name);
        mod = ctx->data_clb(ctx, trg_mod->name, NULL, 0, ctx->data_clb_data);
    }
    if (!mod) {
        llly_set_free(parents);
        return NULL;
    }
    if (!first_sibling) {
        first_sibling = mod->data;
    }

    /* now search in the schema tree for the matching node */
    while (1) {
        lllys_get_sibling(first_sibling, trg_mod->name, 0, parent->name, 0, parent->nodetype,
                        (const struct lllys_node **)&iter);
        if (!iter) {
            /* not found, iter will be used as NULL result */
            break;
        }

        if (index == 0) {
            /* we are done, iter is the result */
            break;
        } else {
            /* we are going to continue, so update variables for the next loop */
            first_sibling = iter->child;
            parent = parents->set.s[--index];
            iter = NULL;
        }
    }

    llly_set_free(parents);
    return iter;
}

static struct lllys_node *
lllyd_get_schema_inctx(const struct lllyd_node *node, struct llly_ctx *ctx)
{
    assert(node);

    return lllys_get_schema_inctx(node->schema, ctx);
}

/* both target and source were validated */
static void
lllyd_merge_node_update(struct lllyd_node *target, struct lllyd_node *source)
{
    struct llly_ctx *ctx;
    struct lllyd_node_leaf_list *trg_leaf, *src_leaf;
    struct lllyd_node_anydata *trg_any, *src_any;
    int len;

    assert(target->schema->nodetype & (LLLYS_LEAF | LLLYS_ANYDATA));
    ctx = target->schema->module->ctx;

    if (ctx == source->schema->module->ctx) {
        /* source and targets are in the same context */
        if (target->schema->nodetype == LLLYS_LEAF) {
            trg_leaf = (struct lllyd_node_leaf_list *)target;
            src_leaf = (struct lllyd_node_leaf_list *)source;

            lllydict_remove(ctx, trg_leaf->value_str);
            trg_leaf->value_str = lllydict_insert(ctx, src_leaf->value_str, 0);
            trg_leaf->value_type = src_leaf->value_type;
            if (trg_leaf->value_type == LLLY_TYPE_LEAFREF) {
                trg_leaf->validity |= LLLYD_VAL_LEAFREF;
                lllyp_parse_value(&((struct lllys_node_leaf *)trg_leaf->schema)->type, &trg_leaf->value_str,
                                NULL, trg_leaf, NULL, NULL, 1, src_leaf->dflt, 0);
            } else {
                lllyd_free_value(trg_leaf->value, trg_leaf->value_type, trg_leaf->value_flags,
                               &((struct lllys_node_leaf *)trg_leaf->schema)->type, trg_leaf->value_str, NULL, NULL, NULL);
                trg_leaf->value = src_leaf->value;
            }
            trg_leaf->dflt = src_leaf->dflt;

            check_leaf_list_backlinks(target);
        } else { /* ANYDATA */
            trg_any = (struct lllyd_node_anydata *)target;
            src_any = (struct lllyd_node_anydata *)source;

            switch(trg_any->value_type) {
            case LLLYD_ANYDATA_CONSTSTRING:
            case LLLYD_ANYDATA_SXML:
            case LLLYD_ANYDATA_JSON:
                lllydict_remove(ctx, trg_any->value.str);
                break;
            case LLLYD_ANYDATA_DATATREE:
                lllyd_free_withsiblings(trg_any->value.tree);
                break;
            case LLLYD_ANYDATA_XML:
                lllyxml_free_withsiblings(ctx, trg_any->value.xml);
                break;
            case LLLYD_ANYDATA_LYB:
                free(trg_any->value.mem);
                break;
            case LLLYD_ANYDATA_STRING:
            case LLLYD_ANYDATA_SXMLD:
            case LLLYD_ANYDATA_JSOND:
            case LLLYD_ANYDATA_LYBD:
                /* dynamic strings are used only as input parameters */
                assert(0);
                break;
            }

            trg_any->value_type = src_any->value_type;
            trg_any->value = src_any->value;

            src_any->value_type = LLLYD_ANYDATA_DATATREE;
            src_any->value.tree = NULL;
        }
    } else {
        /* we have different contexts for the target and source */
        if (target->schema->nodetype == LLLYS_LEAF) {
            trg_leaf = (struct lllyd_node_leaf_list *)target;
            src_leaf = (struct lllyd_node_leaf_list *)source;

            lllydict_remove(ctx, trg_leaf->value_str);
            trg_leaf->value_str = lllydict_insert(ctx, src_leaf->value_str, 0);
            lllyd_free_value(trg_leaf->value, trg_leaf->value_type, trg_leaf->value_flags,
                           &((struct lllys_node_leaf *)trg_leaf->schema)->type, trg_leaf->value_str, NULL, NULL, NULL);
            trg_leaf->value_type = src_leaf->value_type;
            trg_leaf->dflt = src_leaf->dflt;

            switch (trg_leaf->value_type) {
            case LLLY_TYPE_BINARY:
            case LLLY_TYPE_STRING:
                /* value_str pointer is shared in these cases */
                trg_leaf->value.string = trg_leaf->value_str;
                break;
            case LLLY_TYPE_LEAFREF:
                trg_leaf->validity |= LLLYD_VAL_LEAFREF;
                lllyp_parse_value(&((struct lllys_node_leaf *)trg_leaf->schema)->type, &trg_leaf->value_str,
                                NULL, trg_leaf, NULL, NULL, 1, trg_leaf->dflt, 0);
                break;
            case LLLY_TYPE_INST:
                trg_leaf->value.instance = NULL;
                break;
            case LLLY_TYPE_UNION:
                /* unresolved union (this must be non-validated tree), duplicate the stored string (duplicated
                 * because of possible change of the value in case of instance-identifier) */
                trg_leaf->value.string = lllydict_insert(ctx, src_leaf->value.string, 0);
                break;
            case LLLY_TYPE_BITS:
            case LLLY_TYPE_ENUM:
            case LLLY_TYPE_IDENT:
                /* in case of duplicating bits (no matter if in the same context or not) or enum and identityref into
                 * a different context, searching for the type and duplicating the data is almost as same as resolving
                 * the string value, so due to a simplicity, parse the value for the duplicated leaf */
                lllyp_parse_value(&((struct lllys_node_leaf *)trg_leaf->schema)->type, &trg_leaf->value_str, NULL,
                                trg_leaf, NULL, NULL, 1, trg_leaf->dflt, 1);
                break;
            default:
                trg_leaf->value = src_leaf->value;
                break;
            }

            check_leaf_list_backlinks(target);
        } else { /* ANYDATA */
            trg_any = (struct lllyd_node_anydata *)target;
            src_any = (struct lllyd_node_anydata *)source;

            switch(trg_any->value_type) {
            case LLLYD_ANYDATA_CONSTSTRING:
            case LLLYD_ANYDATA_SXML:
            case LLLYD_ANYDATA_JSON:
                lllydict_remove(ctx, trg_any->value.str);
                break;
            case LLLYD_ANYDATA_DATATREE:
                lllyd_free_withsiblings(trg_any->value.tree);
                break;
            case LLLYD_ANYDATA_XML:
                lllyxml_free_withsiblings(ctx, trg_any->value.xml);
                break;
            case LLLYD_ANYDATA_LYB:
                free(trg_any->value.mem);
                break;
            case LLLYD_ANYDATA_STRING:
            case LLLYD_ANYDATA_SXMLD:
            case LLLYD_ANYDATA_JSOND:
            case LLLYD_ANYDATA_LYBD:
                /* dynamic strings are used only as input parameters */
                assert(0);
                break;
            }

            trg_any->value_type = src_any->value_type;
            if ((void*)src_any->value.tree) {
                /* there is a value to duplicate */
                switch (trg_any->value_type) {
                case LLLYD_ANYDATA_CONSTSTRING:
                case LLLYD_ANYDATA_SXML:
                case LLLYD_ANYDATA_JSON:
                    trg_any->value.str = lllydict_insert(ctx, src_any->value.str, 0);
                    break;
                case LLLYD_ANYDATA_DATATREE:
                    trg_any->value.tree = lllyd_dup_withsiblings_to_ctx(src_any->value.tree, 1, ctx);
                    break;
                case LLLYD_ANYDATA_XML:
                    trg_any->value.xml = lllyxml_dup_elem(ctx, src_any->value.xml, NULL, 1, 1);
                    break;
                case LLLYD_ANYDATA_LYB:
                    len = lllyd_lyb_data_length(src_any->value.mem);
                    if (len == -1) {
                        LOGERR(ctx, LLLY_EINVAL, "Invalid LLLYB data.");
                        return;
                    }
                    trg_any->value.mem = malloc(len);
                    LLLY_CHECK_ERR_RETURN(!trg_any->value.mem, LOGMEM(ctx), );
                    memcpy(trg_any->value.mem, src_any->value.mem, len);
                    break;
                case LLLYD_ANYDATA_STRING:
                case LLLYD_ANYDATA_SXMLD:
                case LLLYD_ANYDATA_JSOND:
                case LLLYD_ANYDATA_LYBD:
                    /* dynamic strings are used only as input parameters */
                    assert(0);
                    break;
                }
            }
        }
    }
}

/* return: 0 (not equal), 1 (equal), -1 (error) */
static int
lllyd_merge_node_schema_equal(struct lllyd_node *node1, struct lllyd_node *node2)
{
    struct lllys_node *sch1;

    if (node1->schema->module->ctx == node2->schema->module->ctx) {
        if (node1->schema != node2->schema) {
            return 0;
        }
    } else {
        /* the nodes are in different contexts, get the appropriate schema nodes from the
         * same context */
        sch1 = lllyd_get_schema_inctx(node1, node2->schema->module->ctx);
        if (!sch1) {
            LOGERR(node2->schema->module->ctx, LLLY_EINVAL, "Target context does not contain a required schema node (%s:%s).",
                   lllyd_node_module(node1)->name, node1->schema->name);
            return -1;
        } else if (sch1 != node2->schema) {
            /* not matching nodes */
            return 0;
        }
    }

    return 1;
}

/* return: 0 (not equal), 1 (equal), 2 (equal and state leaf-/list marked), -1 (error) */
static int
lllyd_merge_node_equal(struct lllyd_node *node1, struct lllyd_node *node2)
{
    int ret;

    switch (node1->schema->nodetype) {
    case LLLYS_CONTAINER:
    case LLLYS_LEAF:
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
    case LLLYS_RPC:
    case LLLYS_ACTION:
    case LLLYS_NOTIF:
        return 1;
    case LLLYS_LEAFLIST:
        if (node1->validity & LLLYD_VAL_INUSE) {
            /* this instance was already matched, we want to find another so that the number of the istances matches */
            assert(node1->schema->flags & LLLYS_CONFIG_R);
            return 0;
        }

        ret = lllyd_list_equal(node1, node2, 1);
        if ((ret == 1) && (node1->schema->flags & LLLYS_CONFIG_R)) {
            /* mark it as matched */
            node1->validity |= LLLYD_VAL_INUSE;
            ret = 2;
        }
        return ret;
    case LLLYS_LIST:
        if (node1->validity & LLLYD_VAL_INUSE) {
            /* this instance was already matched, we want to find another so that the number of the istances matches */
            assert(!((struct lllys_node_list *)node1->schema)->keys_size);
            return 0;
        }

        ret = lllyd_list_equal(node1, node2, 1);
        if ((ret == 1) && !((struct lllys_node_list *)node1->schema)->keys_size) {
            /* mark it as matched */
            node1->validity |= LLLYD_VAL_INUSE;
            ret = 2;
        }
        return ret;
    default:
        break;
    }

    LOGINT(node2->schema->module->ctx);
    return -1;
}

/* spends source */
static int
lllyd_merge_parent_children(struct lllyd_node *target, struct lllyd_node *source, int options)
{
    struct lllyd_node *trg_parent, *src, *src_backup, *src_elem, *src_elem_backup, *src_next, *trg_child, *trg_parent_backup;
    int ret, clear_flag = 0;
    struct llly_ctx *ctx = target->schema->module->ctx; /* shortcut */

    LLLY_TREE_FOR_SAFE(source, src_backup, src) {
        for (src_elem = src_next = src, trg_parent = target;
            src_elem;
            src_elem = src_next) {

            /* it won't get inserted in this case */
            if (src_elem->dflt && (options & LLLYD_OPT_EXPLICIT)) {
                if (src_elem == src) {
                    /* we are done with this subtree in this case */
                    break;
                }
                trg_child = (struct lllyd_node *)1;
                goto src_skip;
            }

            ret = 0;

#ifdef LLLY_ENABLED_CACHE
            struct lllyd_node **trg_child_p;

            /* trees are supposed to be validated so all nodes must have their hash, but lets not be that strict */
            if (!src_elem->hash) {
                lllyd_hash(src_elem);
            }

            if (trg_parent->ht) {
                trg_child = NULL;
                if (!lllyht_find(trg_parent->ht, &src_elem, src_elem->hash, (void **)&trg_child_p)) {
                    trg_child = *trg_child_p;
                    ret = 1;

                    /* it is a bit more difficult with keyless state lists and leaf-lists */
                    if (((trg_child->schema->nodetype == LLLYS_LIST) && !((struct lllys_node_list *)trg_child->schema)->keys_size)
                            || ((trg_child->schema->nodetype == LLLYS_LEAFLIST) && (trg_child->schema->flags & LLLYS_CONFIG_R))) {
                        assert(trg_child->schema->flags & LLLYS_CONFIG_R);

                        while (trg_child && (trg_child->validity & LLLYD_VAL_INUSE)) {
                            /* state lists, find one not-already-found */
                            if (lllyht_find_next(trg_parent->ht, &trg_child, trg_child->hash, (void **)&trg_child_p)) {
                                trg_child = NULL;
                            } else {
                                trg_child = *trg_child_p;
                            }
                        }
                        if (trg_child) {
                            /* mark it as matched */
                            trg_child->validity |= LLLYD_VAL_INUSE;
                            ret = 2;
                        } else {
                            /* actually, it was matched already and no other instance found, so now not a match */
                            ret = 0;
                        }
                    }
                }
            } else
#endif
            {
                LLLY_TREE_FOR(trg_parent->child, trg_child) {
                    /* schema match, data match? */
                    ret = lllyd_merge_node_schema_equal(trg_child, src_elem);
                    if (ret == 1) {
                        ret = lllyd_merge_node_equal(trg_child, src_elem);
                    }
                    if (ret != 0) {
                        /* even data match */
                        break;
                    }
                }
            }

            if (ret > 0) {
                if (trg_child->schema->nodetype & (LLLYS_LEAF | LLLYS_ANYDATA)) {
                    lllyd_merge_node_update(trg_child, src_elem);
                } else if (ret == 2) {
                    clear_flag = 1;
                }
            } else if (ret == -1) {
                /* error */
                lllyd_free_withsiblings(source);
                return 1;
            }

            /* first prepare for the next iteration */
            src_elem_backup = src_elem;
            trg_parent_backup = trg_parent;
            if (((src_elem->schema->nodetype == LLLYS_CONTAINER) || ((src_elem->schema->nodetype == LLLYS_LIST)
                    && ((struct lllys_node_list *)src_elem->schema)->keys_size)) && src_elem->child && trg_child) {
                /* go into children */
                src_next = src_elem->child;
                trg_parent = trg_child;
            } else {
src_skip:
                /* no children (or the whole subtree will be inserted), try siblings */
                if (src_elem == src) {
                    /* we are done with this subtree */
                    if (trg_child) {
                        /* it's an empty container, list without keys, or an already-updated leaf/anydata, nothing else to do */
                        break;
                    } else {
                        /* ... but we still need to insert it */
                        src_next = NULL;
                        goto src_insert;
                    }
                } else {
                    src_next = src_elem->next;
                    /* trg_parent does not change */
                }
            }
            while (!src_next) {
                src_elem = src_elem->parent;
                if (src_elem->parent == src->parent) {
                    /* we are done, no next element to process */
                    break;
                }

                /* parent is already processed, go to its sibling */
                src_next = src_elem->next;
                trg_parent = trg_parent->parent;
            }

            if (!trg_child) {
src_insert:
                /* we need to insert the whole subtree */
                if (ctx == src_elem_backup->schema->module->ctx) {
                    /* same context - unlink the subtree and insert it into the target */
                    lllyd_unlink(src_elem_backup);
                } else {
                    /* different contexts - before inserting subtree, instead of unlinking, duplicate it into the
                     * target context */
                    src_elem_backup = lllyd_dup_to_ctx(src_elem_backup, 1, ctx);
                }

                if (src_elem == source) {
                    /* it will be linked into another data tree and the pointers changed */
                    source = source->next;
                }

                /* insert subtree into the target */
                if (lllyd_insert(trg_parent_backup, src_elem_backup)) {
                    LOGINT(ctx);
                    lllyd_free_withsiblings(source);
                    return 1;
                }
                if (src_elem == src) {
                    /* we are finished for this src */
                    break;
                }
            }
        }
    }

    lllyd_free_withsiblings(source);
    if (clear_flag) {
        return 2;
    }
    return 0;
}

/* spends source */
static int
lllyd_merge_siblings(struct lllyd_node *target, struct lllyd_node *source, int options)
{
    struct lllyd_node *trg, *src, *src_backup, *ins;
    int ret, clear_flag = 0;
    struct llly_ctx *ctx = target->schema->module->ctx; /* shortcut */

    while (target->prev->next) {
        target = target->prev;
    }

    LLLY_TREE_FOR_SAFE(source, src_backup, src) {
        LLLY_TREE_FOR(target, trg) {
            /* sibling found, merge it */
            ret = lllyd_merge_node_schema_equal(trg, src);
            if (ret == 1) {
                ret = lllyd_merge_node_equal(trg, src);
            }
            if (ret > 0) {
                if (ret == 2) {
                    clear_flag = 1;
                }

                switch (trg->schema->nodetype) {
                case LLLYS_LEAF:
                case LLLYS_ANYXML:
                case LLLYS_ANYDATA:
                    lllyd_merge_node_update(trg, src);
                    break;
                case LLLYS_LEAFLIST:
                    /* it's already there, nothing to do */
                    break;
                case LLLYS_LIST:
                case LLLYS_CONTAINER:
                case LLLYS_NOTIF:
                case LLLYS_RPC:
                case LLLYS_INPUT:
                case LLLYS_OUTPUT:
                    ret = lllyd_merge_parent_children(trg, src->child, options);
                    if (ret == 2) {
                        clear_flag = 1;
                    } else if (ret) {
                        lllyd_free_withsiblings(source);
                        return 1;
                    }
                    break;
                default:
                    LOGINT(ctx);
                    lllyd_free_withsiblings(source);
                    return 1;
                }
                break;
            } else if (ret == -1) {
                lllyd_free_withsiblings(source);
                return 1;
            } /* else not equal, nothing to do */
        }

        /* sibling not found, insert it */
        if (!trg) {
            if (ctx != src->schema->module->ctx) {
                ins = lllyd_dup_to_ctx(src, 1, ctx);
            } else {
                lllyd_unlink(src);
                if (src == source) {
                    /* just so source is not freed, we inserted it and need it further */
                    source = src_backup;
                }
                ins = src;
            }
            lllyd_insert_after(target->prev, ins);
        }
    }

    lllyd_free_withsiblings(source);
    if (clear_flag) {
        return 2;
    }
    return 0;
}

API int
lllyd_merge_to_ctx(struct lllyd_node **trg, const struct lllyd_node *src, int options, struct llly_ctx *ctx)
{
    FUN_IN;

    struct lllyd_node *node = NULL, *node2, *target, *trg_merge_start, *src_merge_start = NULL;
    const struct lllyd_node *iter;
    struct lllys_node *src_snode, *sch = NULL;
    int i, src_depth, depth, first_iter, ret, dflt = 1;
    const struct lllys_node *parent = NULL;

    if (!trg || !(*trg) || !src) {
        LOGARG;
        return -1;
    }
    target = *trg;

    parent = lllys_parent(target->schema);

    /* go up all uses */
    while (parent && (parent->nodetype == LLLYS_USES)) {
        parent = lllys_parent(parent);
    }

    if (parent && !lllyp_get_yang_data_template_name(target)) {
        LOGERR(parent->module->ctx, LLLY_EINVAL, "Target not a top-level data tree.");
        return -1;
    }

    /* get know if we are converting data into a different context */
    if (ctx && target->schema->module->ctx != ctx) {
        /* target's data tree context differs from the target context, move the target
         * data tree into the target context */

        /* get the first target's top-level and store it as the result */
        for (; target->prev->next; target = target->prev);
        *trg = target;

        for (node = NULL, trg_merge_start = target; target; target = target->next) {
            node2 = lllyd_dup_to_ctx(target, 1, ctx);
            if (!node2) {
                goto error;
            }
            if (node) {
                if (lllyd_insert_after(node->prev, node2)) {
                    goto error;
                }
            } else {
                node = node2;
            }
        }
        target = node;
        node = NULL;
    } else if (src->schema->module->ctx != target->schema->module->ctx) {
        /* the source data will be converted into the target's context during the merge */
        ctx = target->schema->module->ctx;
    } else if (ctx == src->schema->module->ctx) {
        /* no conversion is needed */
        ctx = NULL;
    }

    /* find source top-level schema node */
    for (src_snode = src->schema, src_depth = 0;
         (src_snode = lllys_parent(src_snode)) && src_snode->nodetype != LLLYS_EXT;
         ++src_depth);

    /* find first shared missing schema parent of the subtrees */
    trg_merge_start = target;
    depth = 0;
    first_iter = 1;
    if (src_depth) {
        /* we are going to create missing parents in the following loop,
         * but we will need to know a dflt flag for them. In case the newly
         * created parent is going to have at least one non-default child,
         * it will be also non-default, otherwise it will be the default node */
        if (options & LLLYD_OPT_NOSIBLINGS) {
            dflt = src->dflt;
        } else {
            LLLY_TREE_FOR(src, iter) {
                if (!iter->dflt) {
                    /* non default sibling -> parent is going to be
                     * created also as non-default */
                    dflt = 0;
                    break;
                }
            }
        }
    }
    while (1) {
        /* going from down (source root) to up (top-level or the common node with target */
        do {
            for (src_snode = src->schema, i = 0; i < src_depth - depth; src_snode = lllys_parent(src_snode), ++i);
            ++depth;
        } while (src_snode != src->schema && (src_snode->nodetype & (LLLYS_CHOICE | LLLYS_CASE | LLLYS_USES)));

        if (src_snode == src->schema) {
            break;
        }

        if (src_snode->nodetype != LLLYS_CONTAINER) {
            /* we would have to create a list (the only data node with children except container), impossible */
            LOGERR(ctx, LLLY_EINVAL, "Cannot create %s \"%s\" for the merge.", strnodetype(src_snode->nodetype), src_snode->name);
            goto error;
        }

        /* have we created any missing containers already? if we did,
         * it is totally useless to search for match, there won't ever be */
        if (!src_merge_start) {
            if (first_iter) {
                node = trg_merge_start;
                first_iter = 0;
            } else {
                node = trg_merge_start->child;
            }

            /* find it in target data nodes */
            LLLY_TREE_FOR(node, node) {
                if (ctx) {
                    /* we have the schema nodes in the different context */
                    sch = lllys_get_schema_inctx(src_snode, ctx);
                    if (!sch) {
                        LOGERR(ctx, LLLY_EINVAL, "Target context does not contain schema node for the data node being "
                               "merged (%s:%s).", lllys_node_module(src_snode)->name, src_snode->name);
                        goto error;
                    }
                } else {
                    /* the context is same and comparison of the schema nodes will works fine */
                    sch = src_snode;
                }

                if (node->schema == sch) {
                    trg_merge_start = node;
                    break;
                }
            }

            if (!(options & LLLYD_OPT_DESTRUCT)) {
                /* the source tree will be duplicated, so to save some work in case
                 * of different target context, create also the parents nodes in the
                 * correct context */
                src_snode = sch;
            }
        } else if (ctx && !(options & LLLYD_OPT_DESTRUCT)) {
            /* get the schema node in the correct (target) context, same as above,
             * this is done to save some work and have the source in the same context
             * when the provided source tree is below duplicated in the target context
             * and connected into the parents created here */
            src_snode = lllys_get_schema_inctx(src_snode, ctx);
            if (!src_snode) {
                LOGERR(ctx, LLLY_EINVAL, "Target context does not contain schema node for the data node being "
                       "merged (%s:%s).", lllys_node_module(src_snode)->name, src_snode->name);
                goto error;
            }
        }

        if (!node) {
            /* it is not there, create it */
            node2 = _lyd_new(NULL, src_snode, dflt);
            if (!src_merge_start) {
                src_merge_start = node2;
            } else {
                if (lllyd_insert(node2, src_merge_start)) {
                    goto error;
                }
                src_merge_start = node2;
            }
        }
    }

    /* process source according to options */
    if (options & LLLYD_OPT_DESTRUCT) {
        LLLY_TREE_FOR(src, iter) {
            check_leaf_list_backlinks((struct lllyd_node *)iter);
            if (options & LLLYD_OPT_NOSIBLINGS) {
                break;
            }
        }

        node = (struct lllyd_node *)src;
        if ((node->prev != node) && (options & LLLYD_OPT_NOSIBLINGS)) {
            node2 = node->prev;
            lllyd_unlink(node);
            lllyd_free_withsiblings(node2);
        }
    } else {
        node = NULL;
        for (; src; src = src->next) {
            /* because we already have to duplicate it, do it in the correct context */
            node2 = lllyd_dup_to_ctx(src, 1, ctx);
            if (!node2) {
                lllyd_free_withsiblings(node);
                goto error;
            }
            if (node) {
                if (lllyd_insert_after(node->prev, node2)) {
                    lllyd_free_withsiblings(node);
                    goto error;
                }
            } else {
                node = node2;
            }

            if (options & LLLYD_OPT_NOSIBLINGS) {
                break;
            }
        }
    }

    if (src_merge_start) {
        /* insert data into the created parents */
        /* first, get the lowest created parent, we don't have to check the nodetype since we are
         * creating only a simple chain of containers */
        for (node2 = src_merge_start; node2->child; node2 = node2->child);
        node2->child = node;
        LLLY_TREE_FOR(node, node) {
            node->parent = node2;
        }
    } else {
        src_merge_start = node;
    }

    if (!first_iter) {
        /* !! src_merge start is a child(ren) of trg_merge_start */
        ret = lllyd_merge_parent_children(trg_merge_start, src_merge_start, options);
    } else {
        /* !! src_merge start is a (top-level) sibling(s) of trg_merge_start */
        ret = lllyd_merge_siblings(trg_merge_start, src_merge_start, options);
    }
    /* it was freed whatever the return value */
    src_merge_start = NULL;
    if (ret == 2) {
        /* clear remporary LLLYD_VAL_INUSE validation flags */
        LLLY_TREE_DFS_BEGIN(target, node2, node) {
            node->validity &= ~LLLYD_VAL_INUSE;
            LLLY_TREE_DFS_END(target, node2, node);
        }
        ret = 0;
    } else if (ret) {
        goto error;
    }

    if (target->schema->nodetype == LLLYS_RPC) {
        lllyd_schema_sort(target, 1);
    }

    /* update the pointer to the target tree if needed */
    if (*trg != target) {
        lllyd_free_withsiblings(*trg);
        (*trg) = target;
    }
    return ret;

error:
    if (*trg != target) {
        /* target is duplication of the original target in different context,
         * free it due to the error */
        lllyd_free_withsiblings(target);
    }
    lllyd_free_withsiblings(src_merge_start);
    return -1;
}

API int
lllyd_merge(struct lllyd_node *target, const struct lllyd_node *source, int options)
{
    FUN_IN;

    if (!target || !source) {
        LOGARG;
        return -1;
    }

    return lllyd_merge_to_ctx(&target, source, options, target->schema->module->ctx);
}

API void
lllyd_free_diff(struct lllyd_difflist *diff)
{
    FUN_IN;

    if (diff) {
        free(diff->type);
        free(diff->first);
        free(diff->second);
        free(diff);
    }
}

static int
lllyd_difflist_add(struct lllyd_difflist *diff, unsigned int *size, unsigned int index,
                 LLLYD_DIFFTYPE type, struct lllyd_node *first, struct lllyd_node *second)
{
    void *new;
    struct llly_ctx *ctx;

    assert(diff);
    assert(size && *size);
    assert(first || second);

    ctx = (first ? first->schema->module->ctx : (second ? second->schema->module->ctx : NULL));

    if (index + 1 == *size) {
        /* it's time to enlarge */
        *size = *size + 16;
        new = realloc(diff->type, *size * sizeof *diff->type);
        LLLY_CHECK_ERR_RETURN(!new, LOGMEM(ctx), EXIT_FAILURE);
        diff->type = new;

        new = realloc(diff->first, *size * sizeof *diff->first);
        LLLY_CHECK_ERR_RETURN(!new, LOGMEM(ctx), EXIT_FAILURE);
        diff->first = new;

        new = realloc(diff->second, *size * sizeof *diff->second);
        LLLY_CHECK_ERR_RETURN(!new, LOGMEM(ctx), EXIT_FAILURE);
        diff->second = new;
    }

    /* insert the item */
    diff->type[index] = type;
    diff->first[index] = first;
    diff->second[index] = second;

    /* terminate the arrays */
    index++;
    diff->type[index] = LLLYD_DIFF_END;
    diff->first[index] = NULL;
    diff->second[index] = NULL;

    return EXIT_SUCCESS;
}

struct diff_ordered_dist {
    struct diff_ordered_dist *next;
    int dist;
};
struct diff_ordered_item {
    struct lllyd_node *first;
    struct lllyd_node *second;
    struct diff_ordered_dist *dist;
};
struct diff_ordered {
    struct lllys_node *schema;
    struct lllyd_node *parent;
    unsigned int count;
    struct diff_ordered_item *items; /* array */
    struct diff_ordered_dist *dist;  /* linked list (1-way, ring) */
    struct diff_ordered_dist *dist_last;  /* aux pointer for faster insertion sort */
};

static int
diff_ordset_insert(struct lllyd_node *node, struct llly_set *ordset)
{
    unsigned int i;
    struct diff_ordered *new_ordered, *iter;

    for (i = 0; i < ordset->number; i++) {
        iter = (struct diff_ordered *)ordset->set.g[i];
        if (iter->schema == node->schema && iter->parent == node->parent) {
            break;
        }
    }
    if (i == ordset->number) {
        /* not seen user-ordered list */
        new_ordered = calloc(1, sizeof *new_ordered);
        LLLY_CHECK_ERR_RETURN(!new_ordered, LOGMEM(node->schema->module->ctx), EXIT_FAILURE);
        new_ordered->schema = node->schema;
        new_ordered->parent = node->parent;

        llly_set_add(ordset, new_ordered, LLLY_SET_OPT_USEASLIST);
    }
    ((struct diff_ordered *)ordset->set.g[i])->count++;

    return EXIT_SUCCESS;
}

static void
diff_ordset_free(struct llly_set *set)
{
    unsigned int i, j;
    struct diff_ordered *ord;

    if (!set) {
        return;
    }

    for (i = 0; i < set->number; i++) {
        ord = (struct diff_ordered *)set->set.g[i];
        for (j = 0; j < ord->count; j++) {
            free(ord->items[j].dist);
        }
        free(ord->items);
        free(ord);
    }

    llly_set_free(set);
}

/*
 * -1 - error
 *  0 - ok
 *  1 - first and second not the same
 */
static int
lllyd_diff_compare(struct lllyd_node *first, struct lllyd_node *second, int options)
{
    int rc;

    if (first->dflt && !(options & LLLYD_DIFFOPT_WITHDEFAULTS)) {
        /* the second one cannot be default (see lllyd_diff()),
         * so the nodes differs (first one is default node) */
        return 1;
    }

    if (first->schema->nodetype & (LLLYS_LEAFLIST | LLLYS_LIST)) {
        if (first->validity & LLLYD_VAL_INUSE) {
            /* this node was already matched, it cannot be matched twice (except for state leaf-/lists,
             * which we want to keep the count on this way) */
            return 1;
        }

        rc = lllyd_list_equal(first, second, (options & LLLYD_DIFFOPT_WITHDEFAULTS ? 1 : 0));
        if (rc == -1) {
            return -1;
        } else if (!rc) {
            /* list instances differs */
            return 1;
        }
        /* matches */
    }

    return 0;
}

/*
 * -1 - error
 *  0 - ok
 */
static int
lllyd_diff_match(struct lllyd_node *first, struct lllyd_node *second, struct lllyd_difflist *diff, unsigned int *size,
               unsigned int *i, struct llly_set *matchset, struct llly_set *ordset, int options)
{
    switch (first->schema->nodetype) {
    case LLLYS_LEAFLIST:
    case LLLYS_LIST:
        /* additional work for future move matching in case of user ordered lists */
        if (first->schema->flags & LLLYS_USERORDERED) {
            diff_ordset_insert(first, ordset);
        }

        /* falls through */
    case LLLYS_CONTAINER:
    case LLLYS_RPC:
    case LLLYS_ACTION:
    case LLLYS_NOTIF:
        assert(!(second->validity & LLLYD_VAL_INUSE));
        second->validity |= LLLYD_VAL_INUSE;
        /* remember the matching node in first for keeping correct pointer in first
         * for comparing when passing through the second tree in lllyd_diff().
         * Duplicities are not allowed actually, but they cannot happen since single
         * node can match only one node in the other tree */
        llly_set_add(matchset, first, LLLY_SET_OPT_USEASLIST);
        break;
    case LLLYS_LEAF:
        /* check for leaf's modification */
        if (!lllyd_leaf_val_equal(first, second, 0) || ((options & LLLYD_DIFFOPT_WITHDEFAULTS) && (first->dflt != second->dflt))) {
            if (lllyd_difflist_add(diff, size, (*i)++, LLLYD_DIFF_CHANGED, first, second)) {
               return -1;
            }
        }
        break;
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        /* check for anydata/anyxml's modification */
        if (!lllyd_anydata_equal(first, second) && lllyd_difflist_add(diff, size, (*i)++, LLLYD_DIFF_CHANGED, first, second)) {
            return -1;
        }
        break;
    default:
        LOGINT(first->schema->module->ctx);
        return -1;
    }

    /* mark both that they have matching instance in the other tree */
    assert(!(first->validity & LLLYD_VAL_INUSE));
    first->validity |= LLLYD_VAL_INUSE;

    return 0;
}

/* @brief compare if the nodes are equivalent including checking the list's keys
 * Go through the nodes and their parents and in the case of list, compare its keys.
 *
 * @return 0 different, 1 equivalent
 */
static int
lllyd_diff_equivnode(struct lllyd_node *first, struct lllyd_node *second)
{
    struct lllyd_node *iter1, *iter2;

    for (iter1 = first, iter2 = second; iter1 && iter2; iter1 = iter1->parent, iter2 = iter2->parent) {
        if (iter1->schema->module->ctx == iter2->schema->module->ctx) {
            if (iter1->schema != iter2->schema) {
                return 0;
            }
        } else {
            if (!llly_strequal(iter1->schema->name, iter2->schema->name, 0)) {
                /* comparing the names is fine, even if they are, in fact, 2 different nodes
                 * with equal names, some of their parents will differ */
                return 0;
            }
        }
        if (iter1->schema->nodetype == LLLYS_LIST) {
            /* compare keys */
            if (lllyd_list_equal(iter1, iter2, 0) != 1) {
                return 0;
            }
        }
    }

    if (iter1 != iter2) {
        /* we are supposed to be in root (NULL) in both trees */
        return 0;
    }

    return 1;
}

static int
lllyd_diff_move_preprocess(struct diff_ordered *ordered, struct lllyd_node *first, struct lllyd_node *second)
{
    struct llly_ctx *ctx = first->schema->module->ctx;
    struct lllyd_node *iter;
    unsigned int pos = 0;
    int abs_dist;
    struct diff_ordered_dist *dist_aux;
    struct diff_ordered_dist *dist_iter, *dist_last;
    char *str = NULL;

    /* ordered->count was zeroed and now it is incremented with each added
     * item's information, so it is actually position of the second node
     */

    /* get the position of the first node */
    for (iter = first->prev; iter->next; iter = iter->prev) {
        if (!(iter->validity & LLLYD_VAL_INUSE)) {
            /* skip deleted nodes */
            continue;
        }
        if (iter->schema == first->schema) {
            pos++;
        }
    }
    if (pos != ordered->count) {
        LOGDBG(LLLY_LDGDIFF, "detected moved element \"%s\" from %d to %d (distance %d)",
               str = lllyd_path(first), pos, ordered->count, ordered->count - pos);
        free(str);
    }

    /* store information, count distance */
    ordered->items[pos].dist = dist_aux = calloc(1, sizeof *dist_aux);
    LLLY_CHECK_ERR_RETURN(!dist_aux, LOGMEM(ctx), EXIT_FAILURE);
    ordered->items[pos].dist->dist = ordered->count - pos;
    abs_dist = abs(ordered->items[pos].dist->dist);
    ordered->items[pos].first = first;
    ordered->items[pos].second = second;
    ordered->count++;

    /* insert sort of distances, higher first */
    for (dist_iter = ordered->dist, dist_last = NULL;
            dist_iter;
            dist_last = dist_iter, dist_iter = dist_iter->next) {
        if (abs_dist >= abs(dist_iter->dist)) {
            /* found correct place */
            dist_aux->next = dist_iter;
            if (dist_last) {
                dist_last->next = dist_aux;
            }
            break;
        } else if (dist_iter->next == ordered->dist) {
            /* last item */
            dist_aux->next = ordered->dist; /* ring list */
            ordered->dist_last = dist_aux;
            break;
        }
    }
    if (dist_aux->next == ordered->dist) {
        if (ordered->dist_last == dist_aux) {
            /* last item */
            if (!ordered->dist) {
                /* the only item */
                dist_aux->next = dist_aux;
                ordered->dist = ordered->dist_last = dist_aux;
            }
        } else {
            /* first item */
            ordered->dist = dist_aux;
            if (dist_aux->next) {
                /* more than one item, update the last one's next */
                ordered->dist_last->next = dist_aux;
            } else {
                /* the only item */
                ordered->dist_last = dist_aux;
                dist_aux->next = dist_aux; /* ring list */
            }
        }
    }

    return 0;
}

static struct lllyd_difflist *
lllyd_diff_init_difflist(struct llly_ctx *ctx, unsigned int *size)
{
    struct lllyd_difflist *result;

    result = malloc(sizeof *result);
    LLLY_CHECK_ERR_RETURN(!result, LOGMEM(ctx); *size = 0, NULL);

    *size = 1;
    result->type = calloc(*size, sizeof *result->type);
    result->first = calloc(*size, sizeof *result->first);
    result->second = calloc(*size, sizeof *result->second);
    if (!result->type || !result->first || !result->second) {
        LOGMEM(ctx);
        free(result->second);
        free(result->first);
        free(result->type);
        free(result);
        *size = 0;
        return NULL;
    }

    return result;
}

API struct lllyd_difflist *
lllyd_diff(struct lllyd_node *first, struct lllyd_node *second, int options)
{
    FUN_IN;

    struct llly_ctx *ctx;
    int rc;
    struct lllyd_node *elem1, *elem2, *iter, *aux, *parent = NULL, *next1, *next2;
    struct lllyd_difflist *result, *result2 = NULL;
    void *new;
    unsigned int size, size2, index = 0, index2 = 0, i, j, k;
    struct matchlist_s {
        struct matchlist_s *prev;
        struct llly_set *match;
        unsigned int i;
    } *matchlist = NULL, *mlaux;
    struct llly_set *ordset = NULL;
    struct diff_ordered *ordered;
    struct diff_ordered_dist *dist_aux, *dist_iter;
    struct diff_ordered_item item_aux;

    if (!first) {
        /* all nodes in second were created,
         * but the second must be top level */
        if (second && second->parent) {
            LOGERR(second->schema->module->ctx, LLLY_EINVAL, "%s: \"first\" parameter is NULL and \"second\" is not top level.", __func__);
            return NULL;
        }
        result = lllyd_diff_init_difflist(NULL, &size);
        LLLY_TREE_FOR(second, iter) {
            if (!iter->dflt || (options & LLLYD_DIFFOPT_WITHDEFAULTS)) { /* skip the implicit nodes */
                if (lllyd_difflist_add(result, &size, index++, LLLYD_DIFF_CREATED, NULL, iter)) {
                    goto error;
                }
            }
            if (options & LLLYD_DIFFOPT_NOSIBLINGS) {
                break;
            }
        }
        return result;
    } else if (!second) {
        /* all nodes from first were deleted */
        result = lllyd_diff_init_difflist(first->schema->module->ctx, &size);
        LLLY_TREE_FOR(first, iter) {
            if (!iter->dflt || (options & LLLYD_DIFFOPT_WITHDEFAULTS)) { /* skip the implicit nodes */
                if (lllyd_difflist_add(result, &size, index++, LLLYD_DIFF_DELETED, iter, NULL)) {
                    goto error;
                }
            }
            if (options & LLLYD_DIFFOPT_NOSIBLINGS) {
                break;
            }
        }
        return result;
    }

    ctx = first->schema->module->ctx;

    if (options & LLLYD_DIFFOPT_NOSIBLINGS) {
        /* both trees must start at the same (schema) node */
        if (first->schema != second->schema) {
            LOGERR(ctx, LLLY_EINVAL, "%s: incompatible trees to compare with LLLYD_OPT_NOSIBLINGS option.", __func__);
            return NULL;
        }
        /* use first's and second's child to make comparison the same as without LLLYD_OPT_NOSIBLINGS */
        first = first->child;
        second = second->child;
    } else {
        /* go to the first sibling in both trees */
        if (first->parent) {
            first = first->parent->child;
        } else {
            while (first->prev->next) {
                first = first->prev;
            }
        }

        if (second->parent) {
            second = second->parent->child;
        } else {
            for (; second->prev->next; second = second->prev);
        }

        /* check that both has the same (schema) parent or that they are top-level nodes */
        if ((first->parent && second->parent && first->parent->schema != second->parent->schema) ||
                (!first->parent && first->parent != second->parent)) {
            LOGERR(ctx, LLLY_EINVAL, "%s: incompatible trees with different parents.", __func__);
            return NULL;
        }
    }
    if (first == second) {
        LOGERR(ctx, LLLY_EINVAL, "%s: comparing the same tree does not make sense.", __func__);
        return NULL;
    }

    /* initiate resulting structure */
    result = lllyd_diff_init_difflist(ctx, &size);
    LLLY_CHECK_ERR_GOTO(!result, , error);

    /* the records about created and moved items are created in
     * bad order, so the records about created nodes (and their
     * possible moving) is stored separately and added to the
     * main result at the end.
     */
    result2 = lllyd_diff_init_difflist(ctx, &size2);
    LLLY_CHECK_ERR_GOTO(!result2, , error);

    matchlist = malloc(sizeof *matchlist);
    LLLY_CHECK_ERR_GOTO(!matchlist, LOGMEM(ctx), error);

    matchlist->i = 0;
    matchlist->match = llly_set_new();
    matchlist->prev = NULL;

    ordset = llly_set_new();
    LLLY_CHECK_ERR_GOTO(!ordset, , error);

    /*
     * compare trees
     */
    /* 1) newly created nodes + changed leafs/anyxmls */
    next1 = first;
    for (elem2 = next2 = second; elem2; elem2 = next2) {
        /* keep right pointer for searching in the first tree */
        elem1 = next1;

        if (elem2->dflt && !(options & LLLYD_DIFFOPT_WITHDEFAULTS)) {
            /* skip default elements, they could not be created or changed, just deleted */
            goto cmp_continue;
        }

#ifdef LLLY_ENABLED_CACHE
        struct lllyd_node **iter_p;

        if (elem1 && elem1->parent && elem1->parent->ht) {
            iter = NULL;
            if (!lllyht_find(elem1->parent->ht, &elem2, elem2->hash, (void **)&iter_p)) {
                iter = *iter_p;
                /* we found a match */
                if (iter->dflt && !(options & LLLYD_DIFFOPT_WITHDEFAULTS)) {
                    /* the second one cannot be default (see lllyd_diff()),
                     * so the nodes differs (first one is default node) */
                    iter = NULL;
                }
                while (iter && (iter->validity & LLLYD_VAL_INUSE)) {
                    /* state lists, find one not-already-found */
                    assert((iter->schema->nodetype & (LLLYS_LIST | LLLYS_LEAFLIST)) && (iter->schema->flags & LLLYS_CONFIG_R));
                    if (lllyht_find_next(elem1->parent->ht, &iter, iter->hash, (void **)&iter_p)) {
                        iter = NULL;
                    } else {
                        iter = *iter_p;
                    }
                }
            }
        } else
#endif
        {
            /* search for elem2 instance in the first */
            LLLY_TREE_FOR(elem1, iter) {
                if (iter->schema != elem2->schema) {
                    continue;
                }

                /* elem2 instance found */
                rc = lllyd_diff_compare(iter, elem2, options);
                if (rc == -1) {
                    goto error;
                } else if (rc == 0) {
                    /* match */
                    break;
                } /* else, continue */
            }
        }
        /* we have a match */
        if (iter && lllyd_diff_match(iter, elem2, result, &size, &index, matchlist->match, ordset, options)) {
            goto error;
        }

        if (!iter) {
            /* elem2 not found in the first tree */
            if (lllyd_difflist_add(result2, &size2, index2++, LLLYD_DIFF_CREATED, elem1 ? elem1->parent : parent, elem2)) {
                goto error;
            }

            if (elem1 && (elem2->schema->flags & LLLYS_USERORDERED)) {
                /* store the correct place where the node is supposed to be moved after creation */
                /* if elem1 does not exist, all nodes were created and they will be created in
                 * correct order, so it is not needed to detect moves */
                for (aux = elem2->prev; aux->next; aux = aux->prev) {
                    if (aux->schema == elem2->schema) {
                        /* predecessor found */
                        break;
                    }
                }
                if (!aux->next) {
                    /* predecessor not found */
                    aux = NULL;
                }
                if (lllyd_difflist_add(result2, &size2, index2++, LLLYD_DIFF_MOVEDAFTER2, aux, elem2)) {
                    goto error;
                }
            }
        }

cmp_continue:
        /* select element for the next run                                    1     2
         * - first, process all siblings of a single parent                  / \   / \
         * - then, go to children (deep)                                    3   4 7   8
         * - return to the parent's next sibling children                  / \
         *                                                                5   6
         */
        /* siblings first */
        next1 = elem1;
        next2 = elem2->next;

        if (!next2) {
            /* children */

            /* first pass of the siblings done, some additional work for future
             * detection of move may be needed */
            for (i = ordset->number; i > 0; i--) {
                ordered = (struct diff_ordered *)ordset->set.g[i - 1];
                if (ordered->items) {
                    /* already preprocessed ordered structure */
                    break;
                }
                ordered->items = calloc(ordered->count, sizeof *ordered->items);
                LLLY_CHECK_ERR_GOTO(!ordered->items, LOGMEM(ctx), error);
                ordered->dist = NULL;
                /* zero the count to be used as a node position in lllyd_diff_move_preprocess() */
                ordered->count = 0;
            }

            /* first, get the first sibling */
            if (elem2->parent == second->parent) {
                elem2 = second;
            } else {
                elem2 = elem2->parent->child;
            }

            /* and then find the first child */
            LLLY_TREE_FOR(elem2, iter) {
                if (!(iter->validity & LLLYD_VAL_INUSE)) {
                    /* the iter is not present in both trees */
                    continue;
                } else if (matchlist->i == matchlist->match->number) {
                    if (iter == elem2) {
                        /* we already went through all the matching nodes and now we are just supposed to stop
                         * the loop with no iter */
                        iter = NULL;
                        break;
                    } else {
                        /* we have started with some not processed data in matchlist, but now we have
                         * the INUSE iter and no nodes in matchlist to find its equivalent,
                         * so something went wrong somewhere */
                        LOGINT(ctx);
                        goto error;
                    }
                }

                iter->validity &= ~LLLYD_VAL_INUSE;
                if ((iter->schema->nodetype & (LLLYS_LEAFLIST | LLLYS_LIST)) && (iter->schema->flags & LLLYS_USERORDERED)) {
                    for (j = ordset->number; j > 0; j--) {
                        ordered = (struct diff_ordered *)ordset->set.g[j - 1];
                        if (ordered->schema != iter->schema || !lllyd_diff_equivnode(ordered->parent, iter->parent)) {
                            continue;
                        }

                        /* store necessary information for move detection */
                        lllyd_diff_move_preprocess(ordered, matchlist->match->set.d[matchlist->i], iter);
                        break;
                    }
                }

                if (((iter->schema->nodetype == LLLYS_CONTAINER) || ((iter->schema->nodetype == LLLYS_LIST)
                        && ((struct lllys_node_list *)iter->schema)->keys_size)) && iter->child) {
                    while (matchlist->i < matchlist->match->number && matchlist->match->set.d[matchlist->i]->schema != iter->schema) {
                        matchlist->i++;
                    }
                    if (matchlist->i == matchlist->match->number) {
                        /* we have the INUSE iter, so we have to find its equivalent in match list */
                        LOGINT(ctx);
                        goto error;
                    }
                    next1 = matchlist->match->set.d[matchlist->i]->child;
                    if (!next1) {
                        parent = matchlist->match->set.d[matchlist->i];
                    }
                    matchlist->i++;
                    next2 = iter->child;
                    break;
                }
                matchlist->i++;
            }

            if (!iter) {
                /* no child/data on next level */
                if (elem2 == second) {
                    /* done */
                    break;
                }
            } else {
                /* create new matchlist item */
                mlaux = malloc(sizeof *mlaux);
                LLLY_CHECK_ERR_GOTO(!mlaux, LOGMEM(ctx), error);
                mlaux->i = 0;
                mlaux->match = llly_set_new();
                mlaux->prev = matchlist;
                matchlist = mlaux;
            }
        }

        while (!next2) {
            /* parent */

            /* clean the last match set */
            llly_set_clean(matchlist->match);
            matchlist->i = 0;

            /* try to go to a cousin - child of the next parent's sibling */
            mlaux = matchlist->prev;
            LLLY_TREE_FOR(elem2->parent->next, iter) {
                if (!(iter->validity & LLLYD_VAL_INUSE)) {
                    continue;
                } else if (mlaux->i == mlaux->match->number) {
                    if (iter == elem2->parent->next) {
                        /* we already went through all the matching nodes and now we are just supposed to stop
                         * the loop with no iter */
                        iter = NULL;
                        break;
                    } else {
                        /* we have started with some not processed data in matchlist, but now we have
                         * the INUSE iter and no nodes in matchlist to find its equivalent,
                         * so something went wrong somewhere */
                        LOGINT(ctx);
                        goto error;
                    }
                }

                iter->validity &= ~LLLYD_VAL_INUSE;
                if ((iter->schema->nodetype & (LLLYS_LEAFLIST | LLLYS_LIST)) && (iter->schema->flags & LLLYS_USERORDERED)) {
                    for (j = ordset->number ; j > 0; j--) {
                        ordered = (struct diff_ordered *)ordset->set.g[j - 1];
                        if (ordered->schema != iter->schema || !lllyd_diff_equivnode(ordered->parent, iter->parent)) {
                            continue;
                        }

                        /* store necessary information for move detection */
                        lllyd_diff_move_preprocess(ordered, mlaux->match->set.d[mlaux->i], iter);
                        break;
                    }
                }

                if (((iter->schema->nodetype == LLLYS_CONTAINER) || ((iter->schema->nodetype == LLLYS_LIST)
                        && ((struct lllys_node_list *)iter->schema)->keys_size)) && iter->child) {
                    while (mlaux->i < mlaux->match->number && mlaux->match->set.d[mlaux->i]->schema != iter->schema) {
                        mlaux->i++;
                    }
                    if (mlaux->i == mlaux->match->number) {
                        /* we have the INUSE iter, so we have to find its equivalent in match list */
                        LOGINT(ctx);
                        goto error;
                    }
                    next1 = mlaux->match->set.d[mlaux->i]->child;
                    if (!next1) {
                        parent = mlaux->match->set.d[mlaux->i];
                    }
                    mlaux->i++;
                    next2 = iter->child;
                    break;
                }
                mlaux->i++;
            }

            /* if no cousin exists, continue next loop on higher level */
            if (!iter) {
                elem2 = elem2->parent;

                /* remove matchlist item */
                llly_set_free(matchlist->match);
                mlaux = matchlist;
                matchlist = matchlist->prev;
                free(mlaux);

                if (!matchlist->prev) { /* elem2->parent == second->parent */
                    /* done */
                    break;
                }
            }
        }
    }

    llly_set_free(matchlist->match);
    free(matchlist);
    matchlist = NULL;

    /* 2) deleted nodes */
    LLLY_TREE_DFS_BEGIN(first, next1, elem1) {
        /* search for elem1s deleted in the second */
        if (elem1->validity & LLLYD_VAL_INUSE) {
            /* erase temporary LLLYD_VAL_INUSE flag and continue into children */
            elem1->validity &= ~LLLYD_VAL_INUSE;
        } else if (!elem1->dflt || (options & LLLYD_DIFFOPT_WITHDEFAULTS)) {
            /* elem1 has no matching node in second, add it into result */
            if (lllyd_difflist_add(result, &size, index++, LLLYD_DIFF_DELETED, elem1, NULL)) {
                goto error;
            }

            /* skip subtree processing of data missing in the second tree */
            goto dfs_nextsibling;
        }

        /* modified LLLY_TREE_DFS_END() */
        /* select element for the next run - children first */
        if ((elem1->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA)) || ((elem1->schema->nodetype == LLLYS_LIST)
                && !((struct lllys_node_list *)elem1->schema)->keys_size)) {
            next1 = NULL;
        } else {
            next1 = elem1->child;
        }
        if (!next1) {
dfs_nextsibling:
            /* try siblings */
            next1 = elem1->next;
        }
        while (!next1) {
            /* parent is already processed, go to its sibling */

            elem1 = elem1->parent;
            if (elem1 == first->parent) {
                /* we are done, no next element to process */
                break;
            }

            next1 = elem1->next;
        }
    }

    /* 3) moved nodes (when user-ordered) */
    for (i = 0; i < ordset->number; i++) {
        ordered = (struct diff_ordered *)ordset->set.g[i];
        if (!ordered->dist->dist) {
            /* the dist list is sorted here, but the biggest dist is 0,
             * so nothing changed in order of these items between first
             * and second. We can continue with another user-ordered list.
             */
            continue;
        }

        /* get needed movements
         * - from the biggest distances try to apply node movements
         * on first tree node until they will be ordered as in the
         * second tree - i.e. until there will be no position difference
         */

        for (dist_iter = ordered->dist; ; dist_iter = dist_iter->next) {
            /* dist list is sorted at the beginning, since applying a move causes
             * just a small change in other distances, we assume that the biggest
             * dist is the next one (note that dist list is implemented as ring
             * list). This way we avoid sorting distances after each move. The loop
             * stops when all distances are zero.
             */
            dist_aux = dist_iter;
            while (!dist_iter->dist) {
                /* no dist, so no move. Try another, but when
                 * there is no dist at all, stop the loop
                 */
                dist_iter = dist_iter->next;
                if (dist_iter == dist_aux) {
                    /* all dist we zeroed */
                    goto movedone;
                }
            }
            /* something to move */

            /* get the item to move */
            for (k = 0; k < ordered->count; k++) {
                if (ordered->items[k].dist == dist_iter) {
                    break;
                }
            }

            /* apply the move (distance) */
            memcpy(&item_aux, &ordered->items[k], sizeof item_aux);
            if (dist_iter->dist > 0) {
                /* move to right (other move to left) */
                while (dist_iter->dist) {
                    memcpy(&ordered->items[k], &ordered->items[k + 1], sizeof *ordered->items);
                    ordered->items[k].dist->dist++; /* update moved item distance */
                    dist_iter->dist--;
                    k++;
                }
            } else {
                /* move to left (other move to right) */
                while (dist_iter->dist) {
                    memcpy(&ordered->items[k], &ordered->items[k - 1], sizeof *ordered->items);
                    ordered->items[k].dist->dist--; /* update moved item distance */
                    dist_iter->dist++;
                    k--;
                }
            }
            memcpy(&ordered->items[k], &item_aux, sizeof *ordered->items);

            /* store the transaction into the difflist */
            if (lllyd_difflist_add(result, &size, index++, LLLYD_DIFF_MOVEDAFTER1, item_aux.first,
                                 (k > 0) ? ordered->items[k - 1].first : NULL)) {
                goto error;
            }
            continue;

movedone:
            break;
        }
    }

    diff_ordset_free(ordset);
    ordset = NULL;

    if (index2) {
        /* append result2 with newly created
         * (and possibly moved) nodes */
        if (index + index2 + 1 >= size) {
            /* result must be enlarged */
            size = index + index2 + 1;
            new = realloc(result->type, size * sizeof *result->type);
            LLLY_CHECK_ERR_GOTO(!new, LOGMEM(ctx), error);
            result->type = new;

            new = realloc(result->first, size * sizeof *result->first);
            LLLY_CHECK_ERR_GOTO(!new, LOGMEM(ctx), error);
            result->first = new;

            new = realloc(result->second, size * sizeof *result->second);
            LLLY_CHECK_ERR_GOTO(!new, LOGMEM(ctx), error);
            result->second = new;
        }

        /* append */
        memcpy(&result->type[index], result2->type, (index2 + 1) * sizeof *result->type);
        memcpy(&result->first[index], result2->first, (index2 + 1) * sizeof *result->first);
        memcpy(&result->second[index], result2->second, (index2 + 1) * sizeof *result->second);
    }
    lllyd_free_diff(result2);

    return result;

error:
    while (matchlist) {
        mlaux = matchlist;
        matchlist = mlaux->prev;
        llly_set_free(mlaux->match);
        free(mlaux);

    }
    diff_ordset_free(ordset);

    lllyd_free_diff(result);
    lllyd_free_diff(result2);

    return NULL;
}

static void
lllyd_insert_setinvalid(struct lllyd_node *node)
{
    struct lllyd_node *next, *elem, *parent_list;

    assert(node);

    /* overall validity of the node itself */
    node->validity = llly_new_node_validity(node->schema);

    /* explore changed unique leaves */
    /* first, get know if there is a list in parents chain */
    for (parent_list = node->parent;
         parent_list && parent_list->schema->nodetype != LLLYS_LIST;
         parent_list = parent_list->parent);
    if (parent_list && !(parent_list->validity & LLLYD_VAL_UNIQUE)) {
        /* there is a list, so check if we inserted a leaf supposed to be unique */
        for (elem = node; elem; elem = next) {
            if (elem->schema->nodetype == LLLYS_LIST) {
                /* stop searching to the depth, children would be unique to a list in subtree */
                goto nextsibling;
            }

            if (elem->schema->nodetype == LLLYS_LEAF && (elem->schema->flags & LLLYS_UNIQUE)) {
                /* set flag to list for future validation */
                parent_list->validity |= LLLYD_VAL_UNIQUE;
                break;
            }

            if (elem->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA)) {
                if (elem == node) {
                    /* stop the loop */
                    break;
                }
                goto nextsibling;
            }

            /* select next elem to process */
            /* go into children */
            next = elem->child;
            /* go through siblings */
            if (!next) {
nextsibling:
                next = elem->next;
                if (!next) {
                    /* no sibling */
                    if (elem == node) {
                        /* we are done, back in start node */
                        break;
                    }
                }
            }
            /* go back to parents */
            while (!next) {
                elem = elem->parent;
                if (elem->parent == node->parent) {
                    /* we are done, back in start node */
                    break;
                }
                /* parent was actually already processed, so go to the parent's sibling */
                next = elem->parent->next;
            }
        }
    }

    if (node->parent) {
        /* if the inserted node is list/leaflist with constraint on max instances or extension validation callback,
         * invalidate the parent to make it validate this */
        if ((node->schema->nodetype & LLLYS_LEAFLIST) && ((struct lllys_node_leaflist *)node->schema)->max) {
            node->parent->validity |= LLLYD_VAL_MAND;
        } else if ((node->schema->nodetype & LLLYS_LIST) && ((struct lllys_node_list *)node->schema)->max) {
            node->parent->validity |= LLLYD_VAL_MAND;
        } else {
            /* invalidate all parents that have an extension with a validation
             * callback for their whole subtree */
            next = node->parent;
            while (next) {
                if ((next->schema->flags & LLLYS_VALID_EXT) && (next->schema->flags & LLLYS_VALID_EXT_SUBTREE))
                    next->validity |= LLLYD_VAL_MAND;
                next = next->parent;
            }
        }
    }
}

static void
lllyd_replace(struct lllyd_node *orig, struct lllyd_node *repl)
{
    struct lllyd_node *iter, *last;

    if (!repl) {
        /* remove the old one */
        goto finish;
    }

    if (repl->parent || repl->prev->next) {
        /* isolate the new node */
        repl->next = NULL;
        repl->prev = repl;
        last = repl;
    } else {
        /* get the last node of a possible list of nodes to be inserted */
        for (last = repl; last->next; last = last->next) {
            /* part of the parent changes */
            last->parent = orig->parent;
        }
    }

    /* parent */
    if (orig->parent && (orig->parent->child == orig)) {
        orig->parent->child = repl;
    }

    /* predecessor */
    if (orig->prev == orig) {
        /* the old was alone */
        goto finish;
    }
    if (orig->prev->next) {
        orig->prev->next = repl;
    }
    repl->prev = orig->prev;
    orig->prev = orig;

    /* successor */
    if (orig->next) {
        orig->next->prev = last;
        last->next = orig->next;
        orig->next = NULL;
    } else {
        /* fix the last pointer */
        if (repl->parent) {
            repl->parent->child->prev = last;
        } else {
            /* get the first sibling */
            for (iter = repl; iter->prev != orig; iter = iter->prev);
            iter->prev = last;
        }
    }

finish:
    /* remove the old one */
    lllyd_free(orig);
}

int
lllyd_insert_common(struct lllyd_node *parent, struct lllyd_node **sibling, struct lllyd_node *node, int invalidate)
{
    struct lllys_node *par1, *par2;
    const struct lllys_node *siter;
    struct lllyd_node *start, *iter, *ins, *next1, *next2;
    int invalid = 0, isrpc = 0, clrdflt = 0;
    struct llly_set *llists = NULL;
    int i;
    uint8_t pos;
    int stype = LLLYS_INPUT | LLLYS_OUTPUT;

    assert(parent || sibling);

    /* get first sibling */
    if (parent) {
        start = parent->child;
    } else {
        for (start = *sibling; start->prev->next; start = start->prev);
    }

    /* check placing the node to the appropriate place according to the schema */
    if (!start) {
        if (!parent) {
            /* empty tree to insert */
            if (node->parent || node->prev->next) {
                /* unlink the node first */
                lllyd_unlink_internal(node, 1);
            } /* else insert also node's siblings */
            *sibling = node;
            return EXIT_SUCCESS;
        }
        par1 = parent->schema;
        if (par1->nodetype & (LLLYS_RPC | LLLYS_ACTION)) {
            /* it is not clear if the tree being created is going to
             * be rpc (LLLYS_INPUT) or rpc-reply (LLLYS_OUTPUT) so we have to
             * compare against LLLYS_RPC or LLLYS_ACTION in par2
             */
            stype = LLLYS_RPC | LLLYS_ACTION;
        }
    } else if (parent && (parent->schema->nodetype & (LLLYS_RPC | LLLYS_ACTION))) {
        par1 = parent->schema;
        stype = LLLYS_RPC | LLLYS_ACTION;
    } else {
        for (par1 = lllys_parent(start->schema);
             par1 && !(par1->nodetype & (LLLYS_CONTAINER | LLLYS_LIST | LLLYS_INPUT | LLLYS_OUTPUT | LLLYS_NOTIF));
             par1 = lllys_parent(par1));
    }
    for (par2 = lllys_parent(node->schema);
         par2 && !(par2->nodetype & (LLLYS_CONTAINER | LLLYS_LIST | stype | LLLYS_NOTIF));
         par2 = lllys_parent(par2));
    if (par1 != par2) {
        LOGERR(parent->schema->module->ctx, LLLY_EINVAL, "Cannot insert, different parents (\"%s\" and \"%s\").",
               (par1 ? par1->name : "<top-lvl>"), (par2 ? par2->name : "<top-lvl>"));
        return EXIT_FAILURE;
    }

    if (invalidate) {
        invalid = isrpc = lllyp_is_rpc_action(node->schema);
        if (!parent || node->parent != parent || isrpc) {
            /* it is not just moving under a parent node or it is in an RPC where
             * nodes order matters, so the validation will be necessary */
            invalid++;
        }
    }

    /* unlink only if it is not a list of siblings without a parent and node is not the first sibling */
    if (node->parent || node->prev->next) {
        /* do it permanent if the parents are not exact same or if it is top-level */
        lllyd_unlink_internal(node, invalid);
    }

    llists = llly_set_new();

    /* process the nodes to insert one by one */
    LLLY_TREE_FOR_SAFE(node, next1, ins) {
        if (invalid == 1) {
            /* auto delete nodes from other cases, if any;
             * this is done only if node->parent != parent */
            if (lllyv_multicases(ins, NULL, &start, 1, NULL)) {
                goto error;
            }
        }

        /* isolate the node to be handled separately */
        ins->prev = ins;
        ins->next = NULL;

        iter = NULL;
        if (!ins->dflt) {
            clrdflt = 1;
        }

        /* are we inserting list key? */
        if (!ins->dflt && ins->schema->nodetype == LLLYS_LEAF && lllys_is_key((struct lllys_node_leaf *)ins->schema, &pos)) {
            /* yes, we have a key, get know its position */
            for (i = 0, iter = parent->child;
                    iter && i < pos && iter->schema->nodetype == LLLYS_LEAF;
                    i++, iter = iter->next);
            if (iter) {
                /* insert list's key to the correct position - before the iter */
                if (parent->child == iter) {
                    parent->child = ins;
                }
                if (iter->prev->next) {
                    iter->prev->next = ins;
                }
                ins->prev = iter->prev;
                iter->prev = ins;
                ins->next = iter;

                /* update start element */
                if (parent->child != start) {
                    start = parent->child;
                }
            }

            /* try to find previously present default instance to replace */
        } else if (ins->schema->nodetype == LLLYS_LEAFLIST) {
            i = (int)llists->number;
            if ((llly_set_add(llists, ins->schema, 0) != i) || ins->dflt) {
                /* each leaf-list must be cleared only once (except when looking for exact same existing dflt nodes) */
                LLLY_TREE_FOR_SAFE(start, next2, iter) {
                    if (iter->schema == ins->schema) {
                        if ((ins->dflt && (!iter->dflt || ((iter->schema->flags & LLLYS_CONFIG_W) &&
                                                           !strcmp(((struct lllyd_node_leaf_list *)iter)->value_str,
                                                                  ((struct lllyd_node_leaf_list *)ins)->value_str))))
                                || (!ins->dflt && iter->dflt)) {
                            if (iter == start) {
                                start = next2;
                            }
                            lllyd_free(iter);
                        }
                    }
                }
            }
        } else if (ins->schema->nodetype == LLLYS_LEAF || (ins->schema->nodetype == LLLYS_CONTAINER
                        && !((struct lllys_node_container *)ins->schema)->presence)) {
            LLLY_TREE_FOR(start, iter) {
                if (iter->schema == ins->schema) {
                    if (ins->dflt || iter->dflt) {
                        /* replace existing (either explicit or default) node with the new (either explicit or default) node */
                        lllyd_replace(iter, ins);
                    } else {
                        /* keep both explicit nodes, let the caller solve it later */
                        iter = NULL;
                    }
                    break;
                }
            }
        }

        if (!iter) {
            if (!start) {
                /* add as the only child of the parent */
                start = ins;
                if (parent) {
                    parent->child = ins;
                }
            } else if (isrpc) {
                /* add to the specific position in rpc/rpc-reply/action */
                for (par1 = ins->schema->parent; !(par1->nodetype & (LLLYS_INPUT | LLLYS_OUTPUT)); par1 = lllys_parent(par1));
                siter = NULL;
                LLLY_TREE_FOR(start, iter) {
                    while ((siter = lllys_getnext(siter, par1, lllys_node_module(par1), 0))) {
                        if (iter->schema == siter || ins->schema == siter) {
                            break;
                        }
                    }
                    if (ins->schema == siter) {
                        if ((siter->nodetype & (LLLYS_LEAFLIST | LLLYS_LIST)) && iter->schema == siter) {
                            /* we are inserting leaflist/list instance, but since there are already
                             * some instances of the same leaflist/list, we want to insert the new one
                             * as the last instance, so here we have to move on */
                            while (iter && iter->schema == siter) {
                                iter = iter->next;
                            }
                            if (!iter) {
                                break;
                            }
                        }
                        /* we have the correct place for new node (before the iter) */
                        if (iter == start) {
                            start = ins;
                            if (parent) {
                                parent->child = ins;
                            }
                        } else {
                            iter->prev->next = ins;
                        }
                        ins->prev = iter->prev;
                        iter->prev = ins;
                        ins->next = iter;

                        /* we are done */
                        break;
                    }
                }
                if (!iter) {
                    /* add as the last child of the parent */
                    start->prev->next = ins;
                    ins->prev = start->prev;
                    start->prev = ins;
                }
            } else {
                /* add as the last child of the parent */
                start->prev->next = ins;
                ins->prev = start->prev;
                start->prev = ins;
            }
        }

#ifdef LLLY_ENABLED_CACHE
        lllyd_unlink_hash(ins, ins->parent);
#endif

        ins->parent = parent;

#ifdef LLLY_ENABLED_CACHE
        lllyd_insert_hash(ins);
#endif

        if (invalidate) {
            check_leaf_list_backlinks(ins);
        }

        if (invalid) {
            lllyd_insert_setinvalid(ins);
        }
    }
    llly_set_free(llists);

    if (clrdflt) {
        /* remove the dflt flag from parents */
        for (iter = parent; iter && iter->dflt; iter = iter->parent) {
            iter->dflt = 0;
        }
    }

    if (sibling) {
        *sibling = start;
    }
    return EXIT_SUCCESS;

error:
    llly_set_free(llists);
    return EXIT_FAILURE;
}

API int
lllyd_insert(struct lllyd_node *parent, struct lllyd_node *node)
{
    FUN_IN;

    if (!node || !parent || (parent->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA))) {
        LOGARG;
        return EXIT_FAILURE;
    }

    return lllyd_insert_common(parent, NULL, node, 1);
}

API int
lllyd_insert_sibling(struct lllyd_node **sibling, struct lllyd_node *node)
{
    FUN_IN;

    if (!sibling || !node) {
        LOGARG;
        return EXIT_FAILURE;
    }

    return lllyd_insert_common((*sibling) ? (*sibling)->parent : NULL, sibling, node, 1);

}

int
lllyd_insert_nextto(struct lllyd_node *sibling, struct lllyd_node *node, int before, int invalidate)
{
    struct llly_ctx *ctx;
    struct lllys_node *par1, *par2;
    struct lllyd_node *iter, *start = NULL, *ins, *next1, *next2, *last;
    struct lllyd_node *orig_parent = NULL, *orig_prev = NULL, *orig_next = NULL;
    int invalid = 0;
    char *str;

    assert(sibling);
    assert(node);

    ctx = sibling->schema->module->ctx;

    if (sibling == node) {
        return EXIT_SUCCESS;
    }

    /* check placing the node to the appropriate place according to the schema */
    for (par1 = lllys_parent(sibling->schema);
         par1 && !(par1->nodetype & (LLLYS_CONTAINER | LLLYS_LIST | LLLYS_INPUT | LLLYS_OUTPUT | LLLYS_ACTION | LLLYS_NOTIF));
         par1 = lllys_parent(par1));
    for (par2 = lllys_parent(node->schema);
         par2 && !(par2->nodetype & (LLLYS_CONTAINER | LLLYS_LIST | LLLYS_INPUT | LLLYS_OUTPUT | LLLYS_ACTION | LLLYS_NOTIF));
         par2 = lllys_parent(par2));
    if (par1 != par2) {
        LOGERR(ctx, LLLY_EINVAL, "Cannot insert, different parents (\"%s\" and \"%s\").",
               (par1 ? par1->name : "<top-lvl>"), (par2 ? par2->name : "<top-lvl>"));
        return EXIT_FAILURE;
    }

    if (invalidate && ((node->parent != sibling->parent) || (invalid = lllyp_is_rpc_action(node->schema)) || !node->parent)) {
        /* a) it is not just moving under a parent node (invalid = 1) or
         * b) it is in an RPC where nodes order matters (invalid = 2) or
         * c) it is top-level where we don't know if it is the same tree (invalid = 1),
         * so the validation will be necessary */
        if (!node->parent && !invalid) {
            /* c) search in siblings */
            for (iter = node->prev; iter != node; iter = iter->prev) {
                if (iter == sibling) {
                    break;
                }
            }
            if (iter == node) {
                /* node and siblings are not currently in the same data tree */
                invalid++;
            }
        } else { /* a) and b) */
            invalid++;
        }
    }

    /* unlink only if it is not a list of siblings without a parent or node is not the first sibling,
     * always unlink if just moving a node */
    if ((!invalid) || node->parent || node->prev->next) {
        /* remember the original position to be able to revert
         * unlink in case of error */
        orig_parent = node->parent;
        if (node->prev != node) {
            orig_prev = node->prev;
        }
        orig_next = node->next;
        lllyd_unlink_internal(node, invalid);
    }

    /* find first sibling node */
    if (sibling->parent) {
        start = sibling->parent->child;
    } else {
        for (start = sibling; start->prev->next; start = start->prev);
    }

    /* process the nodes one by one to clean the current tree */
    if (!invalid) {
        /* just moving one sibling */
        last = node;
        node->parent = sibling->parent;
    } else {
        LLLY_TREE_FOR_SAFE(node, next1, ins) {
            lllyd_insert_setinvalid(ins);

            if (invalid == 1) {
                /* auto delete nodes from other cases */
                if (lllyv_multicases(ins, NULL, &start, 1, sibling) == 2) {
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_LYD, sibling, "Insert request refers node (%s) that is going to be auto-deleted.",
                        llly_errpath(ctx));
                    goto error;
                }
            }

            /* try to find previously present default instance to remove because of
            * inserting the specified node */
            if (ins->schema->nodetype == LLLYS_LEAFLIST) {
                LLLY_TREE_FOR_SAFE(start, next2, iter) {
                    if (iter->schema == ins->schema) {
                        if ((ins->dflt && (!iter->dflt || ((iter->schema->flags & LLLYS_CONFIG_W) &&
                                                        !strcmp(((struct lllyd_node_leaf_list *)iter)->value_str,
                                                                ((struct lllyd_node_leaf_list *)ins)->value_str))))
                                || (!ins->dflt && iter->dflt)) {
                            /* iter will get deleted */
                            if (iter == sibling) {
                                LOGERR(ctx, LLLY_EINVAL, "Insert request refers node (%s) that is going to be auto-deleted.",
                                    str = lllyd_path(sibling));
                                free(str);
                                goto error;
                            }
                            if (iter == start) {
                                start = next2;
                            }
                            lllyd_free(iter);
                        }
                    }
                }
            } else if (ins->schema->nodetype == LLLYS_LEAF ||
                    (ins->schema->nodetype == LLLYS_CONTAINER && !((struct lllys_node_container *)ins->schema)->presence)) {
                LLLY_TREE_FOR(start, iter) {
                    if (iter->schema == ins->schema) {
                        if (iter->dflt || ins->dflt) {
                            /* iter gets deleted */
                            if (iter == sibling) {
                                LOGERR(ctx, LLLY_EINVAL, "Insert request refers node (%s) that is going to be auto-deleted.",
                                    str = lllyd_path(sibling));
                                free(str);
                                goto error;
                            }
                            if (iter == start) {
                                start = iter->next;
                            }
                            lllyd_free(iter);
                        }
                        break;
                    }
                }
            }

            ins->parent = sibling->parent;
            last = ins;
        }
    }

    /* insert the (list of) node(s) to the specified position */
    if (before) {
        if (sibling->prev->next) {
            /* adding into the middle */
            sibling->prev->next = node;
        } else if (sibling->parent) {
            /* at the beginning */
            sibling->parent->child = node;
        }
        node->prev = sibling->prev;
        sibling->prev = last;
        last->next = sibling;
    } else { /* after */
        if (sibling->next) {
            /* adding into the middle - fix the prev pointer of the node after inserted nodes */
            last->next = sibling->next;
            sibling->next->prev = last;
        } else {
            /* at the end - fix the prev pointer of the first node */
            start->prev = last;
        }
        sibling->next = node;
        node->prev = sibling;
    }

#ifdef LLLY_ENABLED_CACHE
    /* now that all the nodes are correctly inserted, fix hashes (node was already unlinked) */
    lllyd_insert_hash(node);

    /* relink all following nodes */
    iter = node;
    for (iter = node; iter != last; iter = iter->next) {
        lllyd_unlink_hash(iter, iter->parent);
        lllyd_insert_hash(iter);
    }
#endif

    if (invalidate) {
        LLLY_TREE_FOR(node, next1) {
            check_leaf_list_backlinks(next1);
            if (next1 == last) {
                break;
            }
        }
    }

    return EXIT_SUCCESS;

error:
    /* insert back to the original position */
    if (orig_prev) {
        lllyd_insert_after(orig_prev, node);
    } else if (orig_next) {
        lllyd_insert_before(orig_next, node);
    } else if (orig_parent) {
        /* there were no siblings */
        orig_parent->child = node;
        node->parent = orig_parent;
    }
    return EXIT_FAILURE;
}

API int
lllyd_insert_before(struct lllyd_node *sibling, struct lllyd_node *node)
{
    FUN_IN;

    if (!node || !sibling) {
        LOGARG;
        return EXIT_FAILURE;
    }

    return lllyd_insert_nextto(sibling, node, 1, 1);
}

API int
lllyd_insert_after(struct lllyd_node *sibling, struct lllyd_node *node)
{
    FUN_IN;

    if (!node || !sibling) {
        LOGARG;
        return EXIT_FAILURE;
    }

    return lllyd_insert_nextto(sibling, node, 0, 1);
}

static uint32_t
lllys_module_pos(struct lllys_module *module)
{
    int i;
    uint32_t pos = 1;

    for (i = 0; i < module->ctx->models.used; ++i) {
        if (module->ctx->models.list[i] == module) {
            return pos;
        }
        ++pos;
    }

    LOGINT(module->ctx);
    return 0;
}

static int
lllys_module_node_pos_r(struct lllys_node *first_sibling, struct lllys_node *target, uint32_t *pos)
{
    const struct lllys_node *next = NULL;

    /* the schema nodes are actually from data, lllys_getnext skips non-data schema nodes for us (we know the parent will not be uses) */
    while ((next = lllys_getnext(next, lllys_parent(first_sibling), lllys_node_module(first_sibling), LLLYS_GETNEXT_NOSTATECHECK))) {
        ++(*pos);
        if (target == next) {
            return 0;
        }
    }

    LOGINT(first_sibling->module->ctx);
    return 1;
}

static int
lllyd_node_pos_cmp(const void *item1, const void *item2)
{
    uint32_t mpos1, mpos2;
    struct lllyd_node_pos *np1, *np2;

    np1 = (struct lllyd_node_pos *)item1;
    np2 = (struct lllyd_node_pos *)item2;

    /* different modules? */
    if (lllys_node_module(np1->node->schema) != lllys_node_module(np2->node->schema)) {
        mpos1 = lllys_module_pos(lllys_node_module(np1->node->schema));
        mpos2 = lllys_module_pos(lllys_node_module(np2->node->schema));
        /* if lllys_module_pos failed, there is nothing we can do anyway,
         * at least internal error will be printed */

        if (mpos1 > mpos2) {
            return 1;
        } else {
            return -1;
        }
    }

    if (np1->pos > np2->pos) {
        return 1;
    } else if (np1->pos < np2->pos) {
        return -1;
    }
    return 0;
}

API int
lllyd_schema_sort(struct lllyd_node *sibling, int recursive)
{
    FUN_IN;

    uint32_t len, i;
    struct lllyd_node *node;
    struct lllys_node *first_ssibling = NULL;
    struct lllyd_node_pos *array;

    if (!sibling) {
        LOGARG;
        return -1;
    }

    /* something actually to sort */
    if (sibling->prev != sibling) {

        /* find the beginning */
        sibling = lllyd_first_sibling(sibling);

        /* count siblings */
        len = 0;
        for (node = sibling; node; node = node->next) {
            ++len;
        }

        array = malloc(len * sizeof *array);
        LLLY_CHECK_ERR_RETURN(!array, LOGMEM(sibling->schema->module->ctx), -1);

        /* fill arrays with positions and corresponding nodes */
        for (i = 0, node = sibling; i < len; ++i, node = node->next) {
            array[i].pos = 0;

            /* we need to repeat this for every module */
            if (!first_ssibling || (lllyd_node_module(node) != lllys_node_module(first_ssibling))) {
                /* find the data node schema parent */
                first_ssibling = node->schema;
                while (lllys_parent(first_ssibling)
                        && (lllys_parent(first_ssibling)->nodetype & (LLLYS_CHOICE | LLLYS_CASE | LLLYS_USES))) {
                    first_ssibling = lllys_parent(first_ssibling);
                }

                /* find the beginning */
                if (lllys_parent(first_ssibling)) {
                    first_ssibling = lllys_parent(first_ssibling)->child;
                } else {
                    while (first_ssibling->prev->next) {
                        first_ssibling = first_ssibling->prev;
                    }
                }
            }

            if (lllys_module_node_pos_r(first_ssibling, node->schema, &array[i].pos)) {
                free(array);
                return -1;
            }

            array[i].node = node;
        }

        /* sort the arrays */
        qsort(array, len, sizeof *array, lllyd_node_pos_cmp);

        /* adjust siblings based on the sorted array */
        for (i = 0; i < len; ++i) {
            /* parent child */
            if (i == 0) {
                /* adjust sibling so that it still points to the beginning */
                sibling = array[i].node;
                if (array[i].node->parent) {
                    array[i].node->parent->child = array[i].node;
                }
            }

            /* prev */
            if (i > 0) {
                array[i].node->prev = array[i - 1].node;
            } else {
                array[i].node->prev = array[len - 1].node;
            }

            /* next */
            if (i < len - 1) {
                array[i].node->next = array[i + 1].node;
            } else {
                array[i].node->next = NULL;
            }
        }
        free(array);
    }

    /* sort all the children recursively */
    if (recursive) {
        LLLY_TREE_FOR(sibling, node) {
            if ((node->schema->nodetype & (LLLYS_CONTAINER | LLLYS_LIST | LLLYS_RPC | LLLYS_ACTION | LLLYS_NOTIF))
                    && node->child && lllyd_schema_sort(node->child, recursive)) {
                return -1;
            }
        }
    }

    return EXIT_SUCCESS;
}

static int
_lyd_validate(struct lllyd_node **node, struct lllyd_node *data_tree, struct llly_ctx *ctx, const struct lllys_module **modules,
              int mod_count, struct lllyd_difflist **diff, int options)
{
    struct lllyd_node *root, *next1, *next2, *iter, *act_notif = NULL;
    int ret = EXIT_FAILURE;
    unsigned int i;
    struct unres_data *unres = NULL;
    const struct lllys_module *yanglib_mod;

    unres = calloc(1, sizeof *unres);
    LLLY_CHECK_ERR_RETURN(!unres, LOGMEM(NULL), EXIT_FAILURE);

    if (diff) {
        unres->store_diff = 1;
        unres->diff = lllyd_diff_init_difflist(ctx, &unres->diff_size);
    }

    if ((options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY)) && *node && ((*node)->schema->nodetype != LLLYS_RPC)) {
        options |= LLLYD_OPT_ACT_NOTIF;
    }
    if ((options & (LLLYD_OPT_NOTIF | LLLYD_OPT_NOTIF_FILTER)) && *node && ((*node)->schema->nodetype != LLLYS_NOTIF)) {
        options |= LLLYD_OPT_ACT_NOTIF;
    }

    LLLY_TREE_FOR_SAFE(*node, next1, root) {
        if (modules) {
            for (i = 0; i < (unsigned)mod_count; ++i) {
                if (lllyd_node_module(root) == modules[i]) {
                    break;
                }
            }
            if (i == (unsigned)mod_count) {
                /* skip data that should not be validated */
                continue;
            }
        }

        LLLY_TREE_DFS_BEGIN(root, next2, iter) {
            if (iter->parent && (iter->schema->nodetype & (LLLYS_ACTION | LLLYS_NOTIF))) {
                if (!(options & LLLYD_OPT_ACT_NOTIF) || act_notif) {
                    LOGVAL(ctx, LLLYE_INELEM, LLLY_VLOG_LYD, iter, iter->schema->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "Unexpected %s node \"%s\".",
                           (options & LLLYD_OPT_RPC ? "action" : "notification"), iter->schema->name);
                    goto cleanup;
                }
                act_notif = iter;
            }

            if (lllyv_data_context(iter, options, unres) || lllyv_data_content(iter, options, unres)) {
                goto cleanup;
            }

            /* empty non-default, non-presence container without attributes, make it default */
            if (!iter->dflt && (iter->schema->nodetype == LLLYS_CONTAINER) && !iter->child
                        && !((struct lllys_node_container *)iter->schema)->presence && !iter->attr) {
                iter->dflt = 1;
            }

            LLLY_TREE_DFS_END(root, next2, iter);
        }

        if (options & LLLYD_OPT_NOSIBLINGS) {
            break;
        }

    }

    if (options & LLLYD_OPT_ACT_NOTIF) {
        if (!act_notif) {
            LOGVAL(ctx, LLLYE_MISSELEM, LLLY_VLOG_LYD, *node, (options & LLLYD_OPT_RPC ? "action" : "notification"), (*node)->schema->name);
            goto cleanup;
        }
        options &= ~LLLYD_OPT_ACT_NOTIF;
    }

    if (*node) {
        /* check for uniqueness of top-level lists/leaflists because
         * only the inner instances were tested in lllyv_data_content() */
        yanglib_mod = llly_ctx_get_module(ctx ? ctx : (*node)->schema->module->ctx, "ietf-yang-library", NULL, 1);
        LLLY_TREE_FOR(*node, root) {
            if ((options & LLLYD_OPT_DATA_ADD_YANGLIB) && yanglib_mod && (root->schema->module == yanglib_mod)) {
                /* ietf-yang-library data present, so ignore the option to add them */
                options &= ~LLLYD_OPT_DATA_ADD_YANGLIB;
            }

            if (!(root->schema->nodetype & (LLLYS_LIST | LLLYS_LEAFLIST)) || !(root->validity & LLLYD_VAL_DUP)) {
                continue;
            }

            if (options & LLLYD_OPT_TRUSTED) {
                /* just clear the flag */
                root->validity &= ~LLLYD_VAL_DUP;
                continue;
            }

            if (lllyv_data_dup(root, *node)) {
                goto cleanup;
            }
        }
    }

    /* add missing ietf-yang-library if requested */
    if (options & LLLYD_OPT_DATA_ADD_YANGLIB) {
        if (!(*node)) {
            (*node) = llly_ctx_info(ctx);
        } else if (lllyd_merge((*node), llly_ctx_info(ctx), LLLYD_OPT_DESTRUCT | LLLYD_OPT_EXPLICIT)) {
            LOGERR(ctx, LLLY_EINT, "Adding ietf-yang-library data failed.");
            goto cleanup;
        }
    }

    /* add default values, resolve unres and check for mandatory nodes in final tree */
    if (lllyd_defaults_add_unres(node, options, ctx, modules, mod_count, data_tree, act_notif, unres, 1)) {
        goto cleanup;
    }
    if (act_notif) {
        if (lllyd_check_mandatory_tree(act_notif, ctx, modules, mod_count, options)) {
            goto cleanup;
        }
    } else {
        if (lllyd_check_mandatory_tree(*node, ctx, modules, mod_count, options)) {
            goto cleanup;
        }
    }

    if ((options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY)) && *node && lllyd_schema_sort(*node, 1)) {
        /* rpc and rpc-reply must be sorted */
        goto cleanup;
    }

    /* consolidate diff if created */
    if (diff) {
        assert(unres->store_diff);

        for (i = 0; i < unres->diff_idx; ++i) {
            if (unres->diff->type[i] == LLLYD_DIFF_CREATED) {
                if (unres->diff->second[i]->parent) {
                    unres->diff->first[i] = (struct lllyd_node *)lllyd_path(unres->diff->second[i]->parent);
                }
                unres->diff->second[i] = lllyd_dup(unres->diff->second[i], LLLYD_DUP_OPT_RECURSIVE);
            }
        }

        *diff = unres->diff;
        unres->diff = 0;
        unres->diff_idx = 0;
    }

    ret = EXIT_SUCCESS;

cleanup:
    if (unres) {
        free(unres->node);
        free(unres->type);
        for (i = 0; i < unres->diff_idx; ++i) {
            if (unres->diff->type[i] == LLLYD_DIFF_DELETED) {
                lllyd_free_withsiblings(unres->diff->first[i]);
                free(unres->diff->second[i]);
            }
        }
        lllyd_free_diff(unres->diff);
        free(unres);
    }

    return ret;
}

API int
lllyd_validate(struct lllyd_node **node, int options, void *var_arg, ...)
{
    FUN_IN;

    struct lllyd_node *iter, *data_tree = NULL;
    struct lllyd_difflist **diff = NULL;
    struct llly_ctx *ctx = NULL;
    va_list ap;

    if (!node) {
        LOGARG;
        return EXIT_FAILURE;
    }

    if (lllyp_data_check_options(NULL, options, __func__)) {
        return EXIT_FAILURE;
    }

    data_tree = *node;

    if ((!(options & LLLYD_OPT_TYPEMASK)
            || (options & (LLLYD_OPT_CONFIG | LLLYD_OPT_GET | LLLYD_OPT_GETCONFIG | LLLYD_OPT_EDIT))) && !(*node)) {
        /* get context with schemas from the var_arg */
        ctx = (struct llly_ctx *)var_arg;
        if (!ctx) {
            LOGERR(NULL, LLLY_EINVAL, "%s: invalid variable parameter (struct llly_ctx *ctx).", __func__);
            return EXIT_FAILURE;
        }

        /* LLLYD_OPT_NOSIBLINGS has no meaning here */
        options &= ~LLLYD_OPT_NOSIBLINGS;
    } else if (options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY | LLLYD_OPT_NOTIF)) {
        /* LLLYD_OPT_NOSIBLINGS cannot be set in this case */
        if (options & LLLYD_OPT_NOSIBLINGS) {
            LOGERR(NULL, LLLY_EINVAL, "%s: invalid parameter (variable arg const struct lllyd_node *data_tree with LLLYD_OPT_NOSIBLINGS).", __func__);
            return EXIT_FAILURE;
        } else if (!(*node)) {
            LOGARG;
            return EXIT_FAILURE;
        }

        /* get the additional data tree if given */
        data_tree = (struct lllyd_node *)var_arg;
        if (data_tree) {
            if (options & LLLYD_OPT_NOEXTDEPS) {
                LOGERR(NULL, LLLY_EINVAL, "%s: invalid parameter (variable arg const struct lllyd_node *data_tree and LLLYD_OPT_NOEXTDEPS set).",
                       __func__);
                return EXIT_FAILURE;
            }

            LLLY_TREE_FOR(data_tree, iter) {
                if (iter->parent) {
                    /* a sibling is not top-level */
                    LOGERR(NULL, LLLY_EINVAL, "%s: invalid variable parameter (const struct lllyd_node *data_tree).", __func__);
                    return EXIT_FAILURE;
                }
            }

            /* move it to the beginning */
            for (; data_tree->prev->next; data_tree = data_tree->prev);
        }
    } else if (options & LLLYD_OPT_DATA_TEMPLATE) {
        /* get context with schemas from the var_arg */
        if (*node && ((*node)->prev->next || (*node)->next)) {
            /* not allow sibling in top-level */
            LOGERR(NULL, LLLY_EINVAL, "%s: invalid variable parameter (struct lllyd_node *node).", __func__);
            return EXIT_FAILURE;
        }
    }

    if (options & LLLYD_OPT_VAL_DIFF) {
        va_start(ap, var_arg);
        diff = va_arg(ap, struct lllyd_difflist **);
        va_end(ap);
        if (!diff) {
            LOGERR(ctx, LLLY_EINVAL, "%s: invalid variable parameter (struct lllyd_difflist **).", __func__);
            return EXIT_FAILURE;
        }
    }

    if (*node) {
        if (!ctx) {
            ctx = (*node)->schema->module->ctx;
        }
        if (!(options & LLLYD_OPT_NOSIBLINGS)) {
            /* check that the node is the first sibling */
            while ((*node)->prev->next) {
                *node = (*node)->prev;
            }
        }
    }

    return _lyd_validate(node, data_tree, ctx, NULL, 0, diff, options);
}

API int
lllyd_validate_modules(struct lllyd_node **node, const struct lllys_module **modules, int mod_count, int options, ...)
{
    FUN_IN;

    struct llly_ctx *ctx;
    struct lllyd_difflist **diff = NULL;
    va_list ap;

    if (!node || !modules || !mod_count) {
        LOGARG;
        return EXIT_FAILURE;
    }

    ctx = modules[0]->ctx;

    if (*node && !(options & LLLYD_OPT_NOSIBLINGS)) {
        /* check that the node is the first sibling */
        while ((*node)->prev->next) {
            *node = (*node)->prev;
        }
    }

    if (lllyp_data_check_options(ctx, options, __func__)) {
        return EXIT_FAILURE;
    }

    if ((options & LLLYD_OPT_TYPEMASK) && !(options & (LLLYD_OPT_CONFIG | LLLYD_OPT_GET | LLLYD_OPT_GETCONFIG | LLLYD_OPT_EDIT))) {
        LOGERR(NULL, LLLY_EINVAL, "%s: options include a forbidden data type.", __func__);
        return EXIT_FAILURE;
    }

    if (options & LLLYD_OPT_VAL_DIFF) {
        va_start(ap, options);
        diff = va_arg(ap, struct lllyd_difflist **);
        va_end(ap);
        if (!diff) {
            LOGERR(ctx, LLLY_EINVAL, "%s: invalid variable parameter (struct lllyd_difflist **).", __func__);
            return EXIT_FAILURE;
        }
    }

    return _lyd_validate(node, *node, ctx, modules, mod_count, diff, options);
}

API int
lllyd_validate_value(struct lllys_node *node, const char *value)
{
    FUN_IN;

    struct lllyd_node_leaf_list leaf;
    struct lllys_node_leaf *sleaf = (struct lllys_node_leaf*)node;
    int ret = EXIT_SUCCESS;

    if (!node || !(node->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
        LOGARG;
        return EXIT_FAILURE;
    }

    if (!value) {
        value = "";
    }

    /* dummy leaf */
    memset(&leaf, 0, sizeof leaf);
    leaf.value_str = lllydict_insert(node->module->ctx, value, 0);

repeat:
    leaf.value_type = sleaf->type.base;
    leaf.schema = node;

    if (leaf.value_type == LLLY_TYPE_LEAFREF) {
        if (!sleaf->type.info.lref.target) {
            /* it should either be unresolved leafref (leaf.value_type are ORed flags) or it will be resolved */
            LOGINT(node->module->ctx);
            ret = EXIT_FAILURE;
            goto cleanup;
        }
        sleaf = sleaf->type.info.lref.target;
        goto repeat;
    } else {
        if (!lllyp_parse_value(&sleaf->type, &leaf.value_str, NULL, &leaf, NULL, NULL, 0, 0, 0)) {
            ret = EXIT_FAILURE;
            goto cleanup;
        }
    }

cleanup:
    lllydict_remove(node->module->ctx, leaf.value_str);
    return ret;
}

/* create an attribute copy */
static struct lllyd_attr *
lllyd_dup_attr(struct llly_ctx *ctx, struct lllyd_node *parent, struct lllyd_attr *attr)
{
    struct lllyd_attr *ret;

    /* allocate new attr */
    if (!parent->attr) {
        parent->attr = malloc(sizeof *parent->attr);
        ret = parent->attr;
    } else {
        for (ret = parent->attr; ret->next; ret = ret->next);
        ret->next = calloc(1, sizeof *ret);
        ret = ret->next;
    }
    LLLY_CHECK_ERR_RETURN(!ret, LOGMEM(ctx), NULL);

    /* fill new attr except */
    ret->parent = parent;
    ret->next = NULL;
    ret->annotation = attr->annotation;
    ret->name = lllydict_insert(ctx, attr->name, 0);
    ret->value_str = lllydict_insert(ctx, attr->value_str, 0);
    ret->value_type = attr->value_type;
    ret->value_flags = attr->value_flags;
    switch (ret->value_type) {
    case LLLY_TYPE_BINARY:
    case LLLY_TYPE_STRING:
        /* value_str pointer is shared in these cases */
        ret->value.string = ret->value_str;
        break;
    case LLLY_TYPE_LEAFREF:
        lllyp_parse_value(*((struct lllys_type **)lllys_ext_complex_get_substmt(LLLY_STMT_TYPE, ret->annotation, NULL)),
                             &ret->value_str, NULL, NULL, ret, NULL, 1, 0, 0);
        break;
    case LLLY_TYPE_INST:
        ret->value.instance = NULL;
        break;
    case LLLY_TYPE_UNION:
        /* unresolved union (this must be non-validated tree), duplicate the stored string (duplicated
         * because of possible change of the value in case of instance-identifier) */
        ret->value.string = lllydict_insert(ctx, attr->value.string, 0);
        break;
    case LLLY_TYPE_ENUM:
    case LLLY_TYPE_IDENT:
    case LLLY_TYPE_BITS:
        /* in case of duplicating bits (no matter if in the same context or not) or enum and identityref into
         * a different context, searching for the type and duplicating the data is almost as same as resolving
         * the string value, so due to a simplicity, parse the value for the duplicated leaf */
        lllyp_parse_value(*((struct lllys_type **)lllys_ext_complex_get_substmt(LLLY_STMT_TYPE, ret->annotation, NULL)),
                             &ret->value_str, NULL, NULL, ret, NULL, 1, 0, 0);
        break;
    default:
        ret->value = attr->value;
        break;
    }
    return ret;
}

int
lllyd_unlink_internal(struct lllyd_node *node, int permanent)
{
    struct lllyd_node *iter;

    if (!node) {
        LOGARG;
        return EXIT_FAILURE;
    }

    if (permanent) {
        check_leaf_list_backlinks(node);
    }

    /* unlink from siblings */
    if (node->prev->next) {
        node->prev->next = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        /* unlinking the last node */
        if (node->parent) {
            iter = node->parent->child;
        } else {
            iter = node->prev;
            while (iter->prev != node) {
                iter = iter->prev;
            }
        }
        /* update the "last" pointer from the first node */
        iter->prev = node->prev;
    }

    /* unlink from parent */
    if (node->parent) {
        if (node->parent->child == node) {
            /* the node is the first child */
            node->parent->child = node->next;
        }

#ifdef LLLY_ENABLED_CACHE
        /* do not remove from parent hash table if freeing the whole subtree */
        if (permanent != 2) {
            lllyd_unlink_hash(node, node->parent);
        }
#endif

        node->parent = NULL;
    }

    node->next = NULL;
    node->prev = node;

    return EXIT_SUCCESS;
}

API int
lllyd_unlink(struct lllyd_node *node)
{
    FUN_IN;

    return lllyd_unlink_internal(node, 1);
}

/*
 * - in leaflist it must be added with value_str
 */
static int
_lyd_dup_node_common(struct lllyd_node *new_node, const struct lllyd_node *orig, struct llly_ctx *ctx, int options)
{
    struct lllyd_attr *attr;

    new_node->attr = NULL;
    if (!(options & LLLYD_DUP_OPT_NO_ATTR)) {
        LLLY_TREE_FOR(orig->attr, attr) {
            lllyd_dup_attr(ctx, new_node, attr);
        }
    }
    new_node->next = NULL;
    new_node->prev = new_node;
    new_node->parent = NULL;
    new_node->validity = llly_new_node_validity(new_node->schema);
    new_node->dflt = orig->dflt;
    if (options & LLLYD_DUP_OPT_WITH_WHEN) {
        new_node->when_status = orig->when_status;
    } else {
        new_node->when_status = orig->when_status & LLLYD_WHEN;
    }
#ifdef LLLY_ENABLED_CACHE
    /* just copy the hash, it will not change */
    if ((new_node->schema->nodetype != LLLYS_LIST) || lllyd_list_has_keys(new_node)) {
        new_node->hash = orig->hash;
    }
#endif

#ifdef LLLY_ENABLED_LYD_PRIV
    if (ctx->priv_dup_clb) {
        new_node->priv = ctx->priv_dup_clb(orig->priv);
    }
#endif

    return EXIT_SUCCESS;
}

static struct lllyd_node *
_lyd_dup_node(const struct lllyd_node *node, const struct lllys_node *schema, struct llly_ctx *ctx, int options)
{
    struct lllyd_node *new_node = NULL;
    struct lllys_node_leaf *sleaf;
    struct lllyd_node_leaf_list *new_leaf;
    struct lllyd_node_anydata *new_any, *old_any;
    const struct lllys_type *type;
    int r;

    /* fill specific part */
    switch (node->schema->nodetype) {
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
        new_leaf = calloc(1, sizeof *new_leaf);
        new_node = (struct lllyd_node *)new_leaf;
        LLLY_CHECK_ERR_GOTO(!new_node, LOGMEM(ctx), error);
        new_node->schema = (struct lllys_node *)schema;

        new_leaf->value_str = lllydict_insert(ctx, ((struct lllyd_node_leaf_list *)node)->value_str, 0);
        new_leaf->value_type = ((struct lllyd_node_leaf_list *)node)->value_type;
        new_leaf->value_flags = ((struct lllyd_node_leaf_list *)node)->value_flags;
        if (_lyd_dup_node_common(new_node, node, ctx, options)) {
            goto error;
        }

        /* get schema from the correct context */
        sleaf = (struct lllys_node_leaf *)new_leaf->schema;

        switch (new_leaf->value_type) {
        case LLLY_TYPE_BINARY:
        case LLLY_TYPE_STRING:
            /* value_str pointer is shared in these cases */
            new_leaf->value.string = new_leaf->value_str;
            break;
        case LLLY_TYPE_LEAFREF:
            new_leaf->validity |= LLLYD_VAL_LEAFREF;
            lllyp_parse_value(&sleaf->type, &new_leaf->value_str, NULL, new_leaf, NULL, NULL, 1, node->dflt, 0);
            break;
        case LLLY_TYPE_INST:
            new_leaf->value.instance = NULL;
            break;
        case LLLY_TYPE_UNION:
            /* unresolved union (this must be non-validated tree), duplicate the stored string (duplicated
             * because of possible change of the value in case of instance-identifier) */
            new_leaf->value.string = lllydict_insert(ctx, ((struct lllyd_node_leaf_list *)node)->value.string, 0);
            break;
        case LLLY_TYPE_ENUM:
        case LLLY_TYPE_IDENT:
        case LLLY_TYPE_BITS:
            /* in case of duplicating bits (no matter if in the same context or not) or enum and identityref into
             * a different context, searching for the type and duplicating the data is almost as same as resolving
             * the string value, so due to a simplicity, parse the value for the duplicated leaf */
            if (!lllyp_parse_value(&sleaf->type, &new_leaf->value_str, NULL, new_leaf, NULL, NULL, 1, node->dflt, 0)) {
                goto error;
            }
            break;
        default:
            new_leaf->value = ((struct lllyd_node_leaf_list *)node)->value;
            break;
        }

        if (new_leaf->value_flags & LLLY_VALUE_USER) {
            /* get the real type */
            type = lllyd_leaf_type(new_leaf);
            if (!type || !type->der || !type->der->module) {
                LOGINT(ctx);
                goto error;
            }

            r = lllytype_store(type->der->module, type->der->name, &new_leaf->value_str, &new_leaf->value);
            if (r == -1) {
                goto error;
            } else if (r) {
                LOGINT(ctx);
                goto error;
            }
        }
        break;
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        old_any = (struct lllyd_node_anydata *)node;
        new_any = calloc(1, sizeof *new_any);
        new_node = (struct lllyd_node *)new_any;
        LLLY_CHECK_ERR_GOTO(!new_node, LOGMEM(ctx), error);
        new_node->schema = (struct lllys_node *)schema;

        if (_lyd_dup_node_common(new_node, node, ctx, options)) {
            goto error;
        }

        new_any->value_type = old_any->value_type;
        if (!(void*)old_any->value.tree) {
            /* no value to duplicate */
            break;
        }
        /* duplicate the value */
        switch (old_any->value_type) {
        case LLLYD_ANYDATA_CONSTSTRING:
        case LLLYD_ANYDATA_SXML:
        case LLLYD_ANYDATA_JSON:
            new_any->value.str = lllydict_insert(ctx, old_any->value.str, 0);
            break;
        case LLLYD_ANYDATA_DATATREE:
            new_any->value.tree = lllyd_dup_withsiblings_to_ctx(old_any->value.tree, 1, ctx);
            break;
        case LLLYD_ANYDATA_XML:
            new_any->value.xml = lllyxml_dup_elem(ctx, old_any->value.xml, NULL, 1, 1);
            break;
        case LLLYD_ANYDATA_LYB:
            r = lllyd_lyb_data_length(old_any->value.mem);
            if (r == -1) {
                LOGERR(ctx, LLLY_EINVAL, "Invalid LLLYB data.");
                goto error;
            }
            new_any->value.mem = malloc(r);
            LLLY_CHECK_ERR_GOTO(!new_any->value.mem, LOGMEM(ctx), error);
            memcpy(new_any->value.mem, old_any->value.mem, r);
            break;
        case LLLYD_ANYDATA_STRING:
        case LLLYD_ANYDATA_SXMLD:
        case LLLYD_ANYDATA_JSOND:
        case LLLYD_ANYDATA_LYBD:
            /* dynamic strings are used only as input parameters */
            assert(0);
            break;
        }
        break;
    case LLLYS_CONTAINER:
    case LLLYS_LIST:
    case LLLYS_NOTIF:
    case LLLYS_RPC:
    case LLLYS_ACTION:
        new_node = calloc(1, sizeof *new_node);
        LLLY_CHECK_ERR_GOTO(!new_node, LOGMEM(ctx), error);
        new_node->schema = (struct lllys_node *)schema;

        if (_lyd_dup_node_common(new_node, node, ctx, options)) {
            goto error;
        }
        break;
    default:
        LOGINT(ctx);
        goto error;
    }

    return new_node;

error:
    lllyd_free(new_node);
    return NULL;
}

static int
lllyd_dup_keys(struct lllyd_node *new_list, const struct lllyd_node *old_list, struct lllys_node *skip_key,
        struct llly_ctx *log_ctx, int options)
{
    struct lllys_node_list *slist;
    struct lllyd_node *key, *key_dup;
    uint16_t i;

    if (new_list->schema->nodetype != LLLYS_LIST) {
        return 0;
    }

    slist = (struct lllys_node_list *)new_list->schema;
    for (key = old_list->child, i = 0; key && (i < slist->keys_size); ++i, key = key->next) {
        if (key->schema != (struct lllys_node *)slist->keys[i]) {
            LOGVAL(log_ctx, LLLYE_PATH_INKEY, LLLY_VLOG_LYD, new_list, slist->keys[i]->name);
            return -1;
        }
        if (key->schema == skip_key) {
            continue;
        }

        key_dup = lllyd_dup(key, options & LLLYD_DUP_OPT_NO_ATTR);
        LLLY_CHECK_ERR_RETURN(!key_dup, LOGMEM(log_ctx), -1);

        if (lllyd_insert(new_list, key_dup)) {
            lllyd_free(key_dup);
            return -1;
        }
    }
    if (!key && (i < slist->keys_size)) {
        LOGVAL(log_ctx, LLLYE_PATH_INKEY, LLLY_VLOG_LYD, new_list, slist->keys[i]->name);
        return -1;
    }

    return 0;
}

API struct lllyd_node *
lllyd_dup_to_ctx(const struct lllyd_node *node, int options, struct llly_ctx *ctx)
{
    FUN_IN;

    struct llly_ctx *log_ctx;
    struct lllys_node *schema;
    const char *yang_data_name;
    const struct lllys_module *trg_mod;
    const struct lllyd_node *next, *elem;
    struct lllyd_node *ret, *parent, *new_node = NULL;

    if (!node) {
        LOGARG;
        return NULL;
    }

    /* fix options */
    if ((options & LLLYD_DUP_OPT_RECURSIVE) && (options & LLLYD_DUP_OPT_WITH_KEYS)) {
        options &= ~LLLYD_DUP_OPT_WITH_KEYS;
    }

    log_ctx = (ctx ? ctx : node->schema->module->ctx);
    if (ctx == node->schema->module->ctx) {
        /* target context is actually the same as the source context,
         * ignore the target context */
        ctx = NULL;
    }

    ret = NULL;
    parent = NULL;

    /* LLLY_TREE_DFS */
    for (elem = next = node; elem; elem = next) {

        /* find the correct schema */
        if (ctx) {
            schema = NULL;
            if (parent) {
                trg_mod = lllyp_get_module(parent->schema->module, NULL, 0, lllyd_node_module(elem)->name,
                                         strlen(lllyd_node_module(elem)->name), 1);
                if (!trg_mod) {
                    LOGERR(log_ctx, LLLY_EINVAL, "Target context does not contain model for the data node being duplicated (%s).",
                                lllyd_node_module(elem)->name);
                    goto error;
                }
                /* we know its parent, so we can start with it */
                lllys_getnext_data(trg_mod, parent->schema, elem->schema->name, strlen(elem->schema->name),
                                 elem->schema->nodetype, 0, (const struct lllys_node **)&schema);
            } else {
                /* we have to search in complete context */
                schema = lllyd_get_schema_inctx(elem, ctx);
            }

            if (!schema) {
                yang_data_name = lllyp_get_yang_data_template_name(elem);
                if (yang_data_name) {
                    LOGERR(log_ctx, LLLY_EINVAL, "Target context does not contain schema node for the data node being duplicated "
                                        "(%s:#%s/%s).", lllyd_node_module(elem)->name, yang_data_name, elem->schema->name);
                } else {
                    LOGERR(log_ctx, LLLY_EINVAL, "Target context does not contain schema node for the data node being duplicated "
                                        "(%s:%s).", lllyd_node_module(elem)->name, elem->schema->name);
                }
                goto error;
            }
        } else {
            schema = elem->schema;
        }

        /* make node copy */
        new_node = _lyd_dup_node(elem, schema, log_ctx, options);
        if (!new_node) {
            goto error;
        }

        if (parent && lllyd_insert(parent, new_node)) {
            goto error;
        }

        if (!ret) {
            ret = new_node;
        }

        if (!(options & (LLLYD_DUP_OPT_RECURSIVE | LLLYD_DUP_OPT_WITH_KEYS))) {
            /* no more descendants copied */
            break;
        }

        if (options & LLLYD_DUP_OPT_WITH_KEYS) {
            /* copy only descendant keys */
            if (lllyd_dup_keys(new_node, elem, NULL, log_ctx, options)) {
                goto error;
            }
            break;
        }

        /* LLLY_TREE_DFS_END */
        /* select element for the next run - children first,
         * child exception for lllyd_node_leaf and lllyd_node_leaflist */
        if (elem->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA)) {
            next = NULL;
        } else {
            next = elem->child;
        }
        if (!next) {
            if (elem->parent == node->parent) {
                break;
            }
            /* no children, so try siblings */
            next = elem->next;
        } else {
            parent = new_node;
        }
        new_node = NULL;

        while (!next) {
            /* no siblings, go back through parents */
            elem = elem->parent;
            if (elem->parent == node->parent) {
                break;
            }
            if (!parent) {
                LOGINT(log_ctx);
                goto error;
            }
            parent = parent->parent;
            /* parent is already processed, go to its sibling */
            next = elem->next;
        }
    }

    /* dup all the parents */
    if (options & LLLYD_DUP_OPT_WITH_PARENTS) {
        parent = ret;
        if (lllys_is_key((struct lllys_node_leaf *)ret->schema, NULL)) {
            /* this key was being duplicated so do not add it twice */
            schema = ret->schema;
        } else {
            schema = NULL;
        }
        for (elem = node->parent; elem; elem = elem->parent) {
            new_node = lllyd_dup(elem, options & LLLYD_DUP_OPT_NO_ATTR);
            LLLY_CHECK_ERR_GOTO(!new_node, LOGMEM(log_ctx), error);

            /* dup all list keys */
            if (lllyd_dup_keys(new_node, elem, schema, log_ctx, options)) {
                goto error;
            }

            /* link together */
            if (lllyd_insert(new_node, parent)) {
                ret = parent;
                goto error;
            }
            parent = new_node;
        }
    }

    return ret;

error:
    lllyd_free(ret);
    return NULL;
}

API struct lllyd_node *
lllyd_dup(const struct lllyd_node *node, int options)
{
    FUN_IN;

    return lllyd_dup_to_ctx(node, options, NULL);
}

static struct lllyd_node *
lllyd_dup_withsiblings_r(const struct lllyd_node *first, struct lllyd_node *parent_dup, int options, struct llly_ctx *ctx)
{
    struct lllyd_node *first_dup = NULL, *prev_dup = NULL, *last_dup;
    const struct lllyd_node *next;

    assert(first);

    /* duplicate and connect all siblings */
    LLLY_TREE_FOR(first, next) {
        last_dup = _lyd_dup_node(next, next->schema, ctx, options);
        if (!last_dup) {
            goto error;
        }

        /* the whole data tree is exactly the same so we can safely copy the validation flags */
        last_dup->validity = next->validity;
        last_dup->when_status = next->when_status;

        last_dup->parent = parent_dup;
        /* connect to the parent or the siblings */
        if (!first_dup) {
            first_dup = last_dup;
            if (parent_dup) {
                parent_dup->child = first_dup;
            }
        } else {
            assert(prev_dup);
            prev_dup->next = last_dup;
            last_dup->prev = prev_dup;
        }

#ifdef LLLY_ENABLED_CACHE
        /* copy hash */
        if ((last_dup->schema->nodetype != LLLYS_LIST) || lllyd_list_has_keys(last_dup)) {
            last_dup->hash = next->hash;
        }

        /* insert into parent */
        lllyd_insert_hash(last_dup);
#endif

        if ((next->schema->nodetype & (LLLYS_LIST | LLLYS_CONTAINER | LLLYS_RPC | LLLYS_ACTION | LLLYS_NOTIF)) && next->child) {
            /* recursively duplicate all children */
            if (!lllyd_dup_withsiblings_r(next->child, last_dup, options, ctx)) {
                goto error;
            }
        }

        prev_dup = last_dup;
    }

    /* correctly set last sibling */
    assert(!prev_dup->next);
    first_dup->prev = prev_dup;

    return first_dup;

error:
    /* disconnect and free */
    if (first_dup) {
        first_dup->parent = NULL;
        lllyd_free_withsiblings(first_dup);
    }
    return NULL;
}

static struct lllyd_node *
lllyd_dup_withsiblings_to_ctx(const struct lllyd_node *node, int options, struct llly_ctx *ctx)
{
    const struct lllyd_node *iter;
    struct lllyd_node *ret, *ret_iter, *tmp;

    if (!node) {
        return NULL;
    }

    /* find first sibling */
    while (node->prev->next) {
        node = node->prev;
    }

    if (node->parent) {
        ret = lllyd_dup_to_ctx(node, options, ctx);
        if (!ret) {
            return NULL;
        }

        /* copy following siblings */
        ret_iter = ret;
        LLLY_TREE_FOR(node->next, iter) {
            tmp = lllyd_dup_to_ctx(iter, options, ctx);
            if (!tmp) {
                lllyd_free_withsiblings(ret);
                return NULL;
            }

            if (lllyd_insert_after(ret_iter, tmp)) {
                lllyd_free_withsiblings(ret);
                return NULL;
            }
            ret_iter = ret_iter->next;
            assert(ret_iter == tmp);
        }
    } else {
        /* duplicating top-level siblings, we can duplicate much more efficiently */
        ret = lllyd_dup_withsiblings_r(node, NULL, options, ctx);
    }

    return ret;
}

API struct lllyd_node *
lllyd_dup_withsiblings(const struct lllyd_node *node, int options)
{
    FUN_IN;

    if (!node) {
        return NULL;
    }

    return lllyd_dup_withsiblings_to_ctx(node, options, lllyd_node_module(node)->ctx);
}

API void
lllyd_free_attr(struct llly_ctx *ctx, struct lllyd_node *parent, struct lllyd_attr *attr, int recursive)
{
    FUN_IN;

    struct lllyd_attr *iter;
    struct lllys_type **type;

    if (!ctx || !attr) {
        return;
    }

    if (parent) {
        if (parent->attr == attr) {
            if (recursive) {
                parent->attr = NULL;
            } else {
                parent->attr = attr->next;
            }
        } else {
            for (iter = parent->attr; iter->next != attr; iter = iter->next);
            if (iter->next) {
                if (recursive) {
                    iter->next = NULL;
                } else {
                    iter->next = attr->next;
                }
            }
        }
    }

    if (!recursive) {
        attr->next = NULL;
    }

    for(iter = attr; iter; ) {
        attr = iter;
        iter = iter->next;

        lllydict_remove(ctx, attr->name);
        type = lllys_ext_complex_get_substmt(LLLY_STMT_TYPE, attr->annotation, NULL);
        assert(type);
        lllyd_free_value(attr->value, attr->value_type, attr->value_flags, *type, attr->value_str, NULL, NULL, NULL);
        lllydict_remove(ctx, attr->value_str);
        free(attr);
    }
}

const struct lllyd_node *
lllyd_attr_parent(const struct lllyd_node *root, struct lllyd_attr *attr)
{
    const struct lllyd_node *next, *elem;
    struct lllyd_attr *node_attr;

    LLLY_TREE_DFS_BEGIN(root, next, elem) {
        for (node_attr = elem->attr; node_attr; node_attr = node_attr->next) {
            if (node_attr == attr) {
                return elem;
            }
        }
        LLLY_TREE_DFS_END(root, next, elem)
    }

    return NULL;
}

API struct lllyd_attr *
lllyd_insert_attr(struct lllyd_node *parent, const struct lllys_module *mod, const char *name, const char *value)
{
    FUN_IN;

    struct lllyd_attr *a, *iter;
    struct llly_ctx *ctx;
    const struct lllys_module *module;
    const char *p;
    char *aux;
    int pos, i;

    if (!parent || !name || !value) {
        LOGARG;
        return NULL;
    }
    ctx = parent->schema->module->ctx;

    if ((p = strchr(name, ':'))) {
        /* search for the namespace */
        aux = strndup(name, p - name);
        if (!aux) {
            LOGMEM(ctx);
            return NULL;
        }
        module = llly_ctx_get_module(ctx, aux, NULL, 1);
        free(aux);
        name = p + 1;

        if (!module) {
            /* module not found */
            LOGERR(ctx, LLLY_EINVAL, "Attribute prefix does not match any implemented schema in the context.");
            return NULL;
        }
    } else if (mod) {
        module = mod;
    } else if (!mod && (!strcmp(name, "type") || !strcmp(name, "select")) && !strcmp(parent->schema->name, "filter")) {
        /* special case of inserting unqualified filter attributes "type" and "select" */
        module = llly_ctx_get_module(ctx, "ietf-netconf", NULL, 1);
        if (!module) {
            LOGERR(ctx, LLLY_EINVAL, "Attribute prefix does not match any implemented schema in the context.");
            return NULL;
        }
    } else {
        /* no prefix -> module is the same as for the parent */
        module = lllyd_node_module(parent);
    }

    pos = -1;
    do {
        if ((unsigned int)(pos + 1) < module->ext_size) {
            i = lllys_ext_instance_presence(&ctx->models.list[0]->extensions[0],
                                          &module->ext[pos + 1], module->ext_size - (pos + 1));
            pos = (i == -1) ? -1 : pos + 1 + i;
        } else {
            pos = -1;
        }
        if (pos == -1) {
            LOGERR(ctx, LLLY_EINVAL, "Attribute does not match any annotation instance definition.");
            return NULL;
        }
    } while (!llly_strequal(module->ext[pos]->arg_value, name, 0));

    a = calloc(1, sizeof *a);
    LLLY_CHECK_ERR_RETURN(!a, LOGMEM(ctx), NULL);
    a->parent = parent;
    a->next = NULL;
    a->annotation = (struct lllys_ext_instance_complex *)module->ext[pos];
    a->name = lllydict_insert(ctx, name, 0);
    a->value_str = lllydict_insert(ctx, value, 0);
    if (!lllyp_parse_value(*((struct lllys_type **)lllys_ext_complex_get_substmt(LLLY_STMT_TYPE, a->annotation, NULL)),
                         &a->value_str, NULL, NULL, a, NULL, 1, 0, 0)) {
        lllyd_free_attr(ctx, NULL, a, 0);
        return NULL;
    }

    if (!parent->attr) {
        parent->attr = a;
    } else {
        for (iter = parent->attr; iter->next; iter = iter->next);
        iter->next = a;
    }

    return a;
}

void
lllyd_free_value(lllyd_val value, LLLY_DATA_TYPE value_type, uint8_t value_flags, struct lllys_type *type, const char *value_str,
               lllyd_val *old_val, LLLY_DATA_TYPE *old_val_type, uint8_t *old_val_flags)
{
    if (old_val) {
        *old_val = value;
        *old_val_type = value_type;
        *old_val_flags = value_flags;
        /* we only backup the values for now */
        return;
    }

    /* otherwise the value is correctly freed */
    if (value_flags & LLLY_VALUE_USER) {
        lllytype_free(type, value, value_str);
    } else {
        switch (value_type) {
        case LLLY_TYPE_BITS:
            if (value.bit) {
                free(value.bit);
            }
            break;
        case LLLY_TYPE_INST:
            if (!(value_flags & LLLY_VALUE_UNRES)) {
                break;
            }
            /* fallthrough */
        case LLLY_TYPE_UNION:
            /* unresolved union leaf */
            lllydict_remove(type->parent->module->ctx, value.string);
            break;
        default:
            break;
        }
    }
}

static void
_lyd_free_node(struct lllyd_node *node)
{
    struct lllyd_node_leaf_list *leaf;

    if (!node) {
        return;
    }

    switch (node->schema->nodetype) {
    case LLLYS_CONTAINER:
    case LLLYS_LIST:
    case LLLYS_RPC:
    case LLLYS_ACTION:
    case LLLYS_NOTIF:
#ifdef LLLY_ENABLED_CACHE
        /* it should be empty because all the children are freed already (only if in debug mode) */
        lllyht_free(node->ht);
#endif
        break;
    case LLLYS_ANYDATA:
    case LLLYS_ANYXML:
        switch (((struct lllyd_node_anydata *)node)->value_type) {
        case LLLYD_ANYDATA_CONSTSTRING:
        case LLLYD_ANYDATA_SXML:
        case LLLYD_ANYDATA_JSON:
            lllydict_remove(node->schema->module->ctx, ((struct lllyd_node_anydata *)node)->value.str);
            break;
        case LLLYD_ANYDATA_DATATREE:
            lllyd_free_withsiblings(((struct lllyd_node_anydata *)node)->value.tree);
            break;
        case LLLYD_ANYDATA_XML:
            lllyxml_free_withsiblings(node->schema->module->ctx, ((struct lllyd_node_anydata *)node)->value.xml);
            break;
        case LLLYD_ANYDATA_LYB:
            free(((struct lllyd_node_anydata *)node)->value.mem);
            break;
        case LLLYD_ANYDATA_STRING:
        case LLLYD_ANYDATA_SXMLD:
        case LLLYD_ANYDATA_JSOND:
        case LLLYD_ANYDATA_LYBD:
            /* dynamic strings are used only as input parameters */
            assert(0);
            break;
        }
        break;
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
        leaf = (struct lllyd_node_leaf_list *)node;
        lllyd_free_value(leaf->value, leaf->value_type, leaf->value_flags, &((struct lllys_node_leaf *)leaf->schema)->type,
                       leaf->value_str, NULL, NULL, NULL);
        lllydict_remove(leaf->schema->module->ctx, leaf->value_str);
        break;
    default:
        assert(0);
    }

    lllyd_free_attr(node->schema->module->ctx, node, node->attr, 1);
    free(node);
}

static void
lllyd_free_internal_r(struct lllyd_node *node, int top)
{
    struct lllyd_node *next, *iter;

    if (!node) {
        return;
    }

    /* if freeing top-level, always remove it from the parent hash table */
    lllyd_unlink_internal(node, (top ? 1 : 2));

    if (!(node->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA))) {
        /* free children */
        LLLY_TREE_FOR_SAFE(node->child, next, iter) {
            lllyd_free_internal_r(iter, 0);
        }
    }

    _lyd_free_node(node);
}

API void
lllyd_free(struct lllyd_node *node)
{
    FUN_IN;

    lllyd_free_internal_r(node, 1);
}

static void
lllyd_free_withsiblings_r(struct lllyd_node *first)
{
    struct lllyd_node *next, *node;

    LLLY_TREE_FOR_SAFE(first, next, node) {
        if (node->schema->nodetype & (LLLYS_CONTAINER | LLLYS_LIST | LLLYS_RPC | LLLYS_ACTION | LLLYS_NOTIF)) {
            lllyd_free_withsiblings_r(node->child);
        }
        _lyd_free_node(node);
    }
}

API void
lllyd_free_withsiblings(struct lllyd_node *node)
{
    FUN_IN;

    struct lllyd_node *iter, *aux;

    if (!node) {
        return;
    }

    if (node->parent) {
        /* optimization - avoid freeing (unlinking) the last node of the siblings list */
        /* so, first, free the node's predecessors to the beginning of the list ... */
        for(iter = node->prev; iter->next; iter = aux) {
            aux = iter->prev;
            lllyd_free(iter);
        }
        /* ... then, the node is the first in the siblings list, so free them all */
        LLLY_TREE_FOR_SAFE(node, aux, iter) {
            lllyd_free(iter);
        }
    } else {
        /* node is top-level so we are freeing the whole data tree, we can just free nodes without any unlinking */
        while (node->prev->next) {
            /* find the first sibling */
            node = node->prev;
        }

        /* free it all */
        lllyd_free_withsiblings_r(node);
    }
}

/**
 * Expectations:
 * - list exists in data tree
 * - the leaf (defined by the unique_expr) is not instantiated under the list
 */
int
lllyd_get_unique_default(const char* unique_expr, struct lllyd_node *list, const char **dflt)
{
    struct llly_ctx *ctx = list->schema->module->ctx;
    const struct lllys_node *parent;
    const struct lllys_node_leaf *sleaf = NULL;
    struct lllys_tpdf *tpdf;
    struct lllyd_node *last, *node;
    struct llly_set *s, *r;
    unsigned int i;
    enum int_log_opts prev_ilo;

    assert(unique_expr && list && dflt);
    *dflt = NULL;

    if (resolve_descendant_schema_nodeid(unique_expr, list->schema->child, LLLYS_LEAF, 1, &parent) || !parent) {
        /* error, but unique expression was checked when the schema was parsed so this should not happened */
        LOGINT(ctx);
        return -1;
    }

    sleaf = (struct lllys_node_leaf *)parent;
    if (sleaf->dflt) {
        /* leaf has a default value */
        *dflt = sleaf->dflt;
    } else if (!(sleaf->flags & LLLYS_MAND_TRUE)) {
        /* get the default value from the type */
        for (tpdf = sleaf->type.der; tpdf && !(*dflt); tpdf = tpdf->type.der) {
            *dflt = tpdf->dflt;
        }
    }

    if (!(*dflt)) {
        return 0;
    }

    /* it has default value, but check if it can appear in the data tree under the list */
    s = llly_set_new();
    for (parent = lllys_parent((struct lllys_node *)sleaf); parent != list->schema; parent = lllys_parent(parent)) {
        if (!(parent->nodetype & (LLLYS_CONTAINER | LLLYS_CASE | LLLYS_CHOICE | LLLYS_USES))) {
            /* This should be already detected when parsing schema */
            LOGINT(ctx);
            llly_set_free(s);
            return -1;
        }
        llly_set_add(s, (void *)parent, LLLY_SET_OPT_USEASLIST);
    }

    llly_ilo_change(NULL, ILO_IGNORE, &prev_ilo, NULL);
    for (i = 0, last = list; i < s->number; i++) {
        parent = s->set.s[i]; /* shortcut */

        switch (parent->nodetype) {
        case LLLYS_CONTAINER:
            if (last) {
                /* find instance in the data */
                r = lllyd_find_path(last, parent->name);
                if (!r || r->number > 1) {
                    llly_set_free(r);
                    *dflt = NULL;
                    goto end;
                }
                if (r->number) {
                    last = r->set.d[0];
                } else {
                    last = NULL;
                }
                llly_set_free(r);
            }
            if (((struct lllys_node_container *)parent)->presence) {
                /* not-instantiated presence container on path */
                *dflt = NULL;
                goto end;
            }
            break;
        case LLLYS_CHOICE :
            /* check presence of another case */
            if (!last) {
                continue;
            }

            /* remember the case to be searched in choice by lllyv_multicases() */
            if (i + 1 == s->number) {
                parent = (struct lllys_node *)sleaf;
            } else if (s->set.s[i + 1]->nodetype == LLLYS_CASE && (i + 2 < s->number) &&
                    s->set.s[i + 2]->nodetype == LLLYS_CHOICE) {
                /* nested choices are covered by lllyv_multicases, we just have to pass
                 * the lowest choice */
                i++;
                continue;
            } else {
                parent = s->set.s[i + 1];
            }
            node = last->child;
            if (lllyv_multicases(NULL, (struct lllys_node *)parent, &node, 0, NULL)) {
                /* another case is present */
                *dflt = NULL;
                goto end;
            }
            break;
        default:
            /* LLLYS_CASE, LLLYS_USES */
            continue;
        }
    }

end:
    llly_ilo_restore(NULL, prev_ilo, NULL, 0);
    llly_set_free(s);
    return 0;
}

API char *
lllyd_path(const struct lllyd_node *node)
{
    FUN_IN;

    char *buf = NULL;

    if (!node) {
        LOGARG;
        return NULL;
    }

    if (llly_vlog_build_path(LLLY_VLOG_LYD, node, &buf, 0, 0)) {
        return NULL;
    }

    return buf;
}

int
lllyd_build_relative_data_path(const struct lllys_module *module, const struct lllyd_node *node, const char *schema_id,
                             char *buf)
{
    const struct lllys_node *snode, *schema;
    const char *mod_name, *name;
    int mod_name_len, name_len, len = 0;
    int r, is_relative = -1;

    assert(schema_id && buf);
    schema = node->schema;

    while (*schema_id) {
        if ((r = parse_schema_nodeid(schema_id, &mod_name, &mod_name_len, &name, &name_len, &is_relative, NULL, NULL, 0)) < 1) {
            LOGINT(module->ctx);
            return -1;
        }
        schema_id += r;

        snode = NULL;
        while ((snode = lllys_getnext(snode, schema, NULL, LLLYS_GETNEXT_WITHCHOICE | LLLYS_GETNEXT_WITHCASE | LLLYS_GETNEXT_NOSTATECHECK))) {
            r = schema_nodeid_siblingcheck(snode, module, mod_name, mod_name_len, name, name_len);
            if (r == 0) {
                schema = snode;
                break;
            } else if (r == 1) {
                continue;
            } else {
                return -1;
            }
        }
        /* no match */
        if (!snode || (!schema_id[0] && snode->nodetype != LLLYS_LEAF)) {
            LOGINT(module->ctx);
            return -1;
        }

        if (!(snode->nodetype & (LLLYS_CHOICE | LLLYS_CASE))) {
            len += sprintf(&buf[len], "%s%s", (len ? "/" : ""), snode->name);
        }
    }

    return len;
}

API struct llly_set *
lllyd_find_path(const struct lllyd_node *ctx_node, const char *path)
{
    FUN_IN;

    struct lllyxp_set xp_set;
    struct llly_set *set;
    char *yang_xpath;
    const char * node_mod_name, *mod_name, *name;
    int mod_name_len, name_len, is_relative = -1;
    uint32_t i;

    if (!ctx_node || !path) {
        LOGARG;
        return NULL;
    }

    if (parse_schema_nodeid(path, &mod_name, &mod_name_len, &name, &name_len, &is_relative, NULL, NULL, 1) > 0) {
        if (name[0] == '#' && !is_relative) {
            node_mod_name = lllyd_node_module(ctx_node)->name;
            if (strncmp(mod_name, node_mod_name, mod_name_len) || node_mod_name[mod_name_len]) {
                return NULL;
            }
            path = name + name_len;
        }
    }

    /* transform JSON into YANG XPATH */
    yang_xpath = transform_json2xpath(lllyd_node_module(ctx_node), path);
    if (!yang_xpath) {
        return NULL;
    }

    memset(&xp_set, 0, sizeof xp_set);

    if (lllyxp_eval(yang_xpath, ctx_node, LLLYXP_NODE_ELEM, lllyd_node_module(ctx_node), &xp_set, 0) != EXIT_SUCCESS) {
        free(yang_xpath);
        return NULL;
    }
    free(yang_xpath);

    set = llly_set_new();
    LLLY_CHECK_ERR_RETURN(!set, LOGMEM(ctx_node->schema->module->ctx), NULL);

    if (xp_set.type == LLLYXP_SET_NODE_SET) {
        for (i = 0; i < xp_set.used; ++i) {
            if (xp_set.val.nodes[i].type == LLLYXP_NODE_ELEM) {
                if (llly_set_add(set, xp_set.val.nodes[i].node, LLLY_SET_OPT_USEASLIST) < 0) {
                    llly_set_free(set);
                    set = NULL;
                    break;
                }
            }
        }
    }
    /* free xp_set content */
    lllyxp_set_cast(&xp_set, LLLYXP_SET_EMPTY, ctx_node, NULL, 0);

    return set;
}

API struct llly_set *
lllyd_find_instance(const struct lllyd_node *data, const struct lllys_node *schema)
{
    FUN_IN;

    struct llly_set *ret, *ret_aux, *spath;
    const struct lllys_node *siter;
    struct lllyd_node *iter;
    unsigned int i, j;

    if (!data || !schema ||
            !(schema->nodetype & (LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LIST | LLLYS_LEAFLIST | LLLYS_ANYDATA | LLLYS_NOTIF | LLLYS_RPC | LLLYS_ACTION))) {
        LOGARG;
        return NULL;
    }

    ret = llly_set_new();
    spath = llly_set_new();
    if (!ret || !spath) {
        LOGMEM(schema->module->ctx);
        goto error;
    }

    /* find data root */
    while (data->parent) {
        /* vertical move (up) */
        data = data->parent;
    }
    while (data->prev->next) {
        /* horizontal move (left) */
        data = data->prev;
    }

    /* build schema path */
    for (siter = schema; siter; ) {
        if (siter->nodetype == LLLYS_AUGMENT) {
            siter = ((struct lllys_node_augment *)siter)->target;
            continue;
        } else if (siter->nodetype & (LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LIST | LLLYS_LEAFLIST | LLLYS_ANYDATA | LLLYS_NOTIF | LLLYS_RPC | LLLYS_ACTION)) {
            /* standard data node */
            llly_set_add(spath, (void*)siter, LLLY_SET_OPT_USEASLIST);

        } /* else skip the rest node types */
        siter = siter->parent;
    }
    if (!spath->number) {
        /* no valid path */
        goto error;
    }

    /* start searching */
    LLLY_TREE_FOR((struct lllyd_node *)data, iter) {
        if (iter->schema == spath->set.s[spath->number - 1]) {
            llly_set_add(ret, iter, LLLY_SET_OPT_USEASLIST);
        }
    }
    for (i = spath->number - 1; i; i--) {
        if (!ret->number) {
            /* nothing found */
            break;
        }

        ret_aux = llly_set_new();
        if (!ret_aux) {
            LOGMEM(schema->module->ctx);
            goto error;
        }
        for (j = 0; j < ret->number; j++) {
            LLLY_TREE_FOR(ret->set.d[j]->child, iter) {
                if (iter->schema == spath->set.s[i - 1]) {
                    llly_set_add(ret_aux, iter, LLLY_SET_OPT_USEASLIST);
                }
            }
        }
        llly_set_free(ret);
        ret = ret_aux;
    }

    llly_set_free(spath);
    return ret;

error:
    llly_set_free(ret);
    llly_set_free(spath);

    return NULL;
}

API struct lllyd_node *
lllyd_first_sibling(struct lllyd_node *node)
{
    FUN_IN;

    struct lllyd_node *start;

    if (!node) {
        return NULL;
    }

    /* get the first sibling */
    if (node->parent) {
        start = node->parent->child;
    } else {
        for (start = node; start->prev->next; start = start->prev);
    }

    return start;
}

API struct llly_set *
llly_set_new(void)
{
    FUN_IN;

    struct llly_set *new;

    new = calloc(1, sizeof(struct llly_set));
    LLLY_CHECK_ERR_RETURN(!new, LOGMEM(NULL), NULL);
    return new;
}

API void
llly_set_free(struct llly_set *set)
{
    FUN_IN;

    if (!set) {
        return;
    }

    free(set->set.g);
    free(set);
}

API int
llly_set_contains(const struct llly_set *set, void *node)
{
    FUN_IN;

    unsigned int i;

    if (!set) {
        return -1;
    }

    for (i = 0; i < set->number; i++) {
        if (set->set.g[i] == node) {
            /* object found */
            return i;
        }
    }

    /* object not found */
    return -1;
}

API struct llly_set *
llly_set_dup(const struct llly_set *set)
{
    FUN_IN;

    struct llly_set *new;

    if (!set) {
        return NULL;
    }

    new = malloc(sizeof *new);
    LLLY_CHECK_ERR_RETURN(!new, LOGMEM(NULL), NULL);
    new->number = set->number;
    new->size = set->size;
    new->set.g = malloc(new->size * sizeof *(new->set.g));
    LLLY_CHECK_ERR_RETURN(!new->set.g, LOGMEM(NULL); free(new), NULL);
    memcpy(new->set.g, set->set.g, new->size * sizeof *(new->set.g));

    return new;
}

API int
llly_set_add(struct llly_set *set, void *node, int options)
{
    FUN_IN;

    unsigned int i;
    void **new;

    if (!set) {
        LOGARG;
        return -1;
    }

    if (!(options & LLLY_SET_OPT_USEASLIST)) {
        /* search for duplication */
        for (i = 0; i < set->number; i++) {
            if (set->set.g[i] == node) {
                /* already in set */
                return i;
            }
        }
    }

    if (set->size == set->number) {
        new = realloc(set->set.g, (set->size + 8) * sizeof *(set->set.g));
        LLLY_CHECK_ERR_RETURN(!new, LOGMEM(NULL), -1);
        set->size += 8;
        set->set.g = new;
    }

    set->set.g[set->number++] = node;

    return set->number - 1;
}

API int
llly_set_merge(struct llly_set *trg, struct llly_set *src, int options)
{
    FUN_IN;

    unsigned int i, ret;
    void **new;

    if (!trg) {
        LOGARG;
        return -1;
    }

    if (!src) {
        return 0;
    }

    if (!(options & LLLY_SET_OPT_USEASLIST)) {
        /* remove duplicates */
        i = 0;
        while (i < src->number) {
            if (llly_set_contains(trg, src->set.g[i]) > -1) {
                llly_set_rm_index(src, i);
            } else {
                ++i;
            }
        }
    }

    /* allocate more memory if needed */
    if (trg->size < trg->number + src->number) {
        new = realloc(trg->set.g, (trg->number + src->number) * sizeof *(trg->set.g));
        LLLY_CHECK_ERR_RETURN(!new, LOGMEM(NULL), -1);
        trg->size = trg->number + src->number;
        trg->set.g = new;
    }

    /* copy contents from src into trg */
    memcpy(trg->set.g + trg->number, src->set.g, src->number * sizeof *(src->set.g));
    ret = src->number;
    trg->number += ret;

    /* cleanup */
    llly_set_free(src);
    return ret;
}

API int
llly_set_rm_index(struct llly_set *set, unsigned int index)
{
    FUN_IN;

    if (!set || (index + 1) > set->number) {
        LOGARG;
        return EXIT_FAILURE;
    }

    if (index == set->number - 1) {
        /* removing last item in set */
        set->set.g[index] = NULL;
    } else {
        /* removing item somewhere in a middle, so put there the last item */
        set->set.g[index] = set->set.g[set->number - 1];
        set->set.g[set->number - 1] = NULL;
    }
    set->number--;

    return EXIT_SUCCESS;
}

API int
llly_set_rm(struct llly_set *set, void *node)
{
    FUN_IN;

    unsigned int i;

    if (!set || !node) {
        LOGARG;
        return EXIT_FAILURE;
    }

    /* get index */
    for (i = 0; i < set->number; i++) {
        if (set->set.g[i] == node) {
            break;
        }
    }
    if (i == set->number) {
        /* node is not in set */
        LOGARG;
        return EXIT_FAILURE;
    }

    return llly_set_rm_index(set, i);
}

API int
llly_set_clean(struct llly_set *set)
{
    FUN_IN;

    if (!set) {
        return EXIT_FAILURE;
    }

    set->number = 0;
    return EXIT_SUCCESS;
}

API int
lllyd_wd_default(struct lllyd_node_leaf_list *node)
{
    FUN_IN;

    struct lllys_node_leaf *leaf;
    struct lllys_node_leaflist *llist;
    struct lllyd_node *iter;
    struct lllys_tpdf *tpdf;
    const char *dflt = NULL, **dflts = NULL;
    uint8_t dflts_size = 0, c, i;

    if (!node || !(node->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
        return 0;
    }

    if (node->dflt) {
        return 1;
    }

    if (node->schema->nodetype == LLLYS_LEAF) {
        leaf = (struct lllys_node_leaf *)node->schema;

        /* get know if there is a default value */
        if (leaf->dflt) {
            /* leaf has a default value */
            dflt = leaf->dflt;
        } else if (!(leaf->flags & LLLYS_MAND_TRUE)) {
            /* get the default value from the type */
            for (tpdf = leaf->type.der; tpdf && !dflt; tpdf = tpdf->type.der) {
                dflt = tpdf->dflt;
            }
        }
        if (!dflt) {
            /* no default value */
            return 0;
        }

        /* compare the default value with the value of the leaf */
        if (!llly_strequal(dflt, node->value_str, 1)) {
            return 0;
        }
    } else if (node->schema->module->version >= LLLYS_VERSION_1_1) { /* LLLYS_LEAFLIST */
        llist = (struct lllys_node_leaflist *)node->schema;

        /* get know if there is a default value */
        if (llist->dflt_size) {
            /* there are default values */
            dflts_size = llist->dflt_size;
            dflts = llist->dflt;
        } else if (!llist->min) {
            /* get the default value from the type */
            for (tpdf = llist->type.der; tpdf && !dflts; tpdf = tpdf->type.der) {
                if (tpdf->dflt) {
                    dflts = &tpdf->dflt;
                    dflts_size = 1;
                    break;
                }
            }
        }

        if (!dflts_size) {
            /* no default values to use */
            return 0;
        }

        /* compare the default value with the value of the leaf */
        /* first, find the first leaf-list's sibling */
        iter = (struct lllyd_node *)node;
        if (iter->parent) {
            iter = iter->parent->child;
        } else {
            for (; iter->prev->next; iter = iter->prev);
        }
        for (c = 0; iter; iter = iter->next) {
            if (iter->schema != node->schema) {
                continue;
            }
            if (c == dflts_size) {
                /* to many leaf-list instances */
                return 0;
            }

            if (llist->flags & LLLYS_USERORDERED) {
                /* we have strict order */
                if (!llly_strequal(dflts[c], ((struct lllyd_node_leaf_list *)iter)->value_str, 1)) {
                    return 0;
                }
            } else {
                /* node's value is supposed to match with one of the default values */
                for (i = 0; i < dflts_size; i++) {
                    if (llly_strequal(dflts[i], ((struct lllyd_node_leaf_list *)iter)->value_str, 1)) {
                        break;
                    }
                }
                if (i == dflts_size) {
                    /* values do not match */
                    return 0;
                }
            }
            c++;
        }
        if (c != dflts_size) {
            /* different sets of leaf-list instances */
            return 0;
        }
    } else {
        return 0;
    }

    /* all checks ok */
    return 1;
}

int
unres_data_diff_new(struct unres_data *unres, struct lllyd_node *subtree, struct lllyd_node *parent, int created)
{
    char *parent_xpath = NULL;

    if (created) {
        return lllyd_difflist_add(unres->diff, &unres->diff_size, unres->diff_idx++, LLLYD_DIFF_CREATED, NULL, subtree);
    } else {
        if (parent) {
            parent_xpath = lllyd_path(parent);
            LLLY_CHECK_ERR_RETURN(!parent_xpath, LOGMEM(lllyd_node_module(subtree)->ctx), -1);
        }
        return lllyd_difflist_add(unres->diff, &unres->diff_size, unres->diff_idx++, LLLYD_DIFF_DELETED,
                                subtree, (struct lllyd_node *)parent_xpath);
    }
}

void
unres_data_diff_rem(struct unres_data *unres, unsigned int idx)
{
    if (unres->diff->type[idx] == LLLYD_DIFF_DELETED) {
        lllyd_free_withsiblings(unres->diff->first[idx]);
        free(unres->diff->second[idx]);
    }

    /* replace by last real value */
    if (idx < unres->diff_idx - 1) {
        unres->diff->type[idx] = unres->diff->type[unres->diff_idx - 1];
        unres->diff->first[idx] = unres->diff->first[unres->diff_idx - 1];
        unres->diff->second[idx] = unres->diff->second[unres->diff_idx - 1];
    }

    /* move the end */
    assert(unres->diff->type[unres->diff_idx] == LLLYD_DIFF_END);
    unres->diff->type[unres->diff_idx - 1] = unres->diff->type[unres->diff_idx];
    --unres->diff_idx;
}

API void
lllyd_free_val_diff(struct lllyd_difflist *diff)
{
    FUN_IN;

    uint32_t i;

    if (!diff) {
        return;
    }

    for (i = 0; diff->type[i] != LLLYD_DIFF_END; ++i) {
        switch (diff->type[i]) {
        case LLLYD_DIFF_CREATED:
            free(diff->first[i]);
            lllyd_free_withsiblings(diff->second[i]);
            break;
        case LLLYD_DIFF_DELETED:
            lllyd_free_withsiblings(diff->first[i]);
            free(diff->second[i]);
            break;
        default:
            /* what to do? */
            break;
        }
    }

    lllyd_free_diff(diff);
}

static int
lllyd_wd_add_leaf(struct lllyd_node **tree, struct lllyd_node *last_parent, struct lllys_node_leaf *leaf, struct unres_data *unres,
                int check_when_must)
{
    struct lllyd_node *dummy = NULL, *current;
    struct lllys_tpdf *tpdf;
    const char *dflt = NULL;
    int ret;

    /* get know if there is a default value */
    if (leaf->dflt) {
        /* leaf has a default value */
        dflt = leaf->dflt;
    } else if (!(leaf->flags & LLLYS_MAND_TRUE)) {
        /* get the default value from the type */
        for (tpdf = leaf->type.der; tpdf && !dflt; tpdf = tpdf->type.der) {
            dflt = tpdf->dflt;
        }
    }
    if (!dflt) {
        /* no default value */
        return EXIT_SUCCESS;
    }

    /* create the node */
    if (!(dummy = lllyd_new_dummy(*tree, last_parent, (struct lllys_node*)leaf, dflt, 1))) {
        goto error;
    }

    if (unres->store_diff) {
        /* remember this subtree in the diff */
        if (unres_data_diff_new(unres, dummy, NULL, 1)) {
            goto error;
        }
    }

    if (!dummy->parent && (*tree)) {
        /* connect dummy nodes into the data tree (at the end of top level nodes) */
        if (lllyd_insert_sibling(tree, dummy)) {
            goto error;
        }
    }
    for (current = dummy; ; current = current->child) {
        /* remember the created data in unres */
        if (check_when_must) {
            if ((current->when_status & LLLYD_WHEN) && unres_data_add(unres, current, UNRES_WHEN) == -1) {
                goto error;
            }
            if (check_when_must == 2) {
                ret = resolve_applies_must(current);
                if ((ret & 0x1) && (unres_data_add(unres, current, UNRES_MUST) == -1)) {
                    goto error;
                }
                if ((ret & 0x2) && (unres_data_add(unres, current, UNRES_MUST_INOUT) == -1)) {
                    goto error;
                }
            }
        }

        /* clear dummy-node flag */
        current->validity &= ~LLLYD_VAL_INUSE;

        if (current->schema == (struct lllys_node *)leaf) {
            break;
        }
    }
    /* update parent's default flag if needed */
    lllyd_wd_update_parents(dummy);

    /* if necessary, remember the created data value in unres */
    if (((struct lllyd_node_leaf_list *)current)->value_type == LLLY_TYPE_LEAFREF) {
        if (unres_data_add(unres, current, UNRES_LEAFREF)) {
            goto error;
        }
    } else if (((struct lllyd_node_leaf_list *)current)->value_type == LLLY_TYPE_INST) {
        if (unres_data_add(unres, current, UNRES_INSTID)) {
            goto error;
        }
    }

    if (!(*tree)) {
        *tree = dummy;
    }
    return EXIT_SUCCESS;

error:
    lllyd_free(dummy);
    return EXIT_FAILURE;
}

static int
lllyd_wd_add_leaflist(struct lllyd_node **tree, struct lllyd_node *last_parent, struct lllys_node_leaflist *llist,
                    struct unres_data *unres, int check_when_must)
{
    struct lllyd_node *dummy, *current, *first = NULL;
    struct lllys_tpdf *tpdf;
    const char **dflt = NULL;
    uint8_t dflt_size = 0;
    int i, ret;

    if (llist->module->version < LLLYS_VERSION_1_1) {
        /* default values on leaf-lists are allowed from YANG 1.1 */
        return EXIT_SUCCESS;
    }

    /* get know if there is a default value */
    if (llist->dflt_size) {
        /* there are default values */
        dflt_size = llist->dflt_size;
        dflt = llist->dflt;
    } else if (!llist->min) {
        /* get the default value from the type */
        for (tpdf = llist->type.der; tpdf && !dflt; tpdf = tpdf->type.der) {
            if (tpdf->dflt) {
                dflt = &tpdf->dflt;
                dflt_size = 1;
                break;
            }
        }
    }

    if (!dflt_size) {
        /* no default values to use */
        return EXIT_SUCCESS;
    }

    for (i = 0; i < dflt_size; i++) {
        /* create the node */
        if (!(dummy = lllyd_new_dummy(*tree, last_parent, (struct lllys_node*)llist, dflt[i], 1))) {
            goto error;
        }

        if (unres->store_diff) {
            /* remember this subtree in the diff */
            if (unres_data_diff_new(unres, dummy, NULL, 1)) {
                goto error;
            }
        }

        if (!first) {
            first = dummy;
        } else if (!dummy->parent) {
            /* interconnect with the rest of leaf-lists */
            first->prev->next = dummy;
            dummy->prev = first->prev;
            first->prev = dummy;
        }

        for (current = dummy; ; current = current->child) {
            /* remember the created data in unres */
            if (check_when_must) {
                if ((current->when_status & LLLYD_WHEN) && unres_data_add(unres, current, UNRES_WHEN) == -1) {
                    goto error;
                }
                if (check_when_must == 2) {
                    ret = resolve_applies_must(current);
                    if ((ret & 0x1) && (unres_data_add(unres, current, UNRES_MUST) == -1)) {
                        goto error;
                    }
                    if ((ret & 0x2) && (unres_data_add(unres, current, UNRES_MUST_INOUT) == -1)) {
                        goto error;
                    }
                }
            }

            /* clear dummy-node flag */
            current->validity &= ~LLLYD_VAL_INUSE;

            if (current->schema == (struct lllys_node *)llist) {
                break;
            }
        }

        /* if necessary, remember the created data value in unres */
        if (((struct lllyd_node_leaf_list *)current)->value_type == LLLY_TYPE_LEAFREF) {
            if (unres_data_add(unres, current, UNRES_LEAFREF)) {
                goto error;
            }
        } else if (((struct lllyd_node_leaf_list *)current)->value_type == LLLY_TYPE_INST) {
            if (unres_data_add(unres, current, UNRES_INSTID)) {
                goto error;
            }
        }
    }

    /* insert into the tree */
    if (first && !first->parent && (*tree)) {
        /* connect dummy nodes into the data tree (at the end of top level nodes) */
        if (lllyd_insert_sibling(tree, first)) {
            goto error;
        }
    } else if (!(*tree)) {
        *tree = first;
    }

    /* update parent's default flag if needed */
    lllyd_wd_update_parents(first);

    return EXIT_SUCCESS;

error:
    lllyd_free_withsiblings(first);
    return EXIT_FAILURE;
}

static void
lllyd_wd_leaflist_cleanup(struct llly_set *set, struct unres_data *unres)
{
    unsigned int i;

    assert(set);

    /* if there is an instance without the dflt flag, we have to
     * remove all instances with the flag - an instance could be
     * explicitely added, so the default leaflists were invalidated */
    for (i = 0; i < set->number; i++) {
        if (!set->set.d[i]->dflt) {
            break;
        }
    }
    if (i < set->number) {
        for (i = 0; i < set->number; i++) {
            if (set->set.d[i]->dflt) {
                /* remove this default instance */
                if (unres->store_diff) {
                    /* just move it to diff if is being generated */
                    unres_data_diff_new(unres, set->set.d[i], set->set.d[i]->parent, 0);
                    lllyd_unlink(set->set.d[i]);
                } else {
                    lllyd_free(set->set.d[i]);
                }
            }
        }
    }
}

/**
 * @brief Process (add/clean flags) default nodes in the schema subtree
 *
 * @param[in,out] root Pointer to the root node of the complete data tree, the root node can be NULL if the data tree
 *                     is empty
 * @param[in] last_parent The closest parent in the data tree to the currently processed \p schema node
 * @param[in] subroot  The root node of a data subtree, the node is instance of the \p schema node, NULL in case the
 *                     schema node is not instantiated in the data tree
 * @param[in] schema The schema node to be processed
 * @param[in] toplevel Flag for processing top level schema nodes when \p last_parent and \p subroot are consider as
 *                     unknown
 * @param[in] options  Parser options to know the data tree type, see @ref parseroptions.
 * @param[in] unres    Unresolved data list, the newly added default nodes may need to add some unresolved items
 * @return EXIT_SUCCESS or EXIT_FAILURE
 */
static int
lllyd_wd_add_subtree(struct lllyd_node **root, struct lllyd_node *last_parent, struct lllyd_node *subroot,
                   struct lllys_node *schema, int toplevel, int options, struct unres_data *unres)
{
    struct llly_set *present = NULL;
    struct lllys_node *siter, *siter_prev;
    struct lllyd_node *iter;
    int i, check_when_must, storing_diff = 0;

    assert(root);

    if ((options & LLLYD_OPT_TYPEMASK) && (schema->flags & LLLYS_CONFIG_R)) {
        /* non LLLYD_OPT_DATA tree, status data are not expected here */
        return EXIT_SUCCESS;
    }

    if (options & (LLLYD_OPT_NOTIF_FILTER | LLLYD_OPT_EDIT | LLLYD_OPT_GET | LLLYD_OPT_GETCONFIG)) {
        check_when_must = 0; /* check neither */
    } else if (options & LLLYD_OPT_TRUSTED) {
        check_when_must = 1; /* check only when */
    } else {
        check_when_must = 2; /* check both when and must */
    }

    if (toplevel && (schema->nodetype & (LLLYS_LEAF | LLLYS_LIST | LLLYS_LEAFLIST | LLLYS_CONTAINER))) {
        /* search for the schema node instance */
        present = llly_set_new();
        if (!present) {
            goto error;
        }
        if ((*root) && lllyd_get_node_siblings(*root, schema, present)) {
            /* there are some instances */
            for (i = 0; i < (signed)present->number; i++) {
                if (schema->nodetype & LLLYS_LEAFLIST) {
                    lllyd_wd_leaflist_cleanup(present, unres);
                } else if (schema->nodetype != LLLYS_LEAF) {
                    if (lllyd_wd_add_subtree(root, present->set.d[i], present->set.d[i], schema, 0, options, unres)) {
                        goto error;
                    }
                } /* else LLLYS_LEAF - nothing to do */
            }
        } else {
            /* no instance */
            if (lllyd_wd_add_subtree(root, last_parent, NULL, schema, 0, options, unres)) {
                goto error;
            }
        }

        llly_set_free(present);
        return EXIT_SUCCESS;
    }

    /* skip disabled parts of schema */
    if (!subroot) {
        /* go through all the uses and check whether they are enabled */
        for (siter = schema->parent; siter && (siter->nodetype & (LLLYS_USES | LLLYS_CHOICE)); siter = siter->parent) {
            if (lllys_is_disabled(siter, 0)) {
                /* ignore disabled uses nodes */
                return EXIT_SUCCESS;
            }
        }

        /* check augment state */
        if (siter && siter->nodetype == LLLYS_AUGMENT) {
            if (lllys_is_disabled(siter, 0)) {
                /* ignore disabled augment */
                return EXIT_SUCCESS;
            }
        }

        /* check the node itself */
        if (lllys_is_disabled(schema, 0)) {
            /* ignore disabled data */
            return EXIT_SUCCESS;
        }
    }

    /* go recursively */
    switch (schema->nodetype) {
    case LLLYS_LIST:
        if (!subroot) {
            /* stop recursion */
            break;
        }
        /* falls through */
    case LLLYS_CONTAINER:
        if (!subroot) {
            /* container does not exists, continue only in case of non presence container */
            if (((struct lllys_node_container *)schema)->presence) {
                /* stop recursion */
                break;
            }
            /* always create empty NP container even if there is no default node,
             * because accroding to RFC, the empty NP container is always part of
             * accessible tree (e.g. for evaluating when and must conditions) */
            subroot = _lyd_new(last_parent, schema, 1);
            /* useless to set mand flag */
            subroot->validity &= ~LLLYD_VAL_MAND;

            if (unres->store_diff) {
                /* remember this container in the diff */
                if (unres_data_diff_new(unres, subroot, NULL, 1)) {
                    goto error;
                }

                /* do not store diff for recursive calls, created values will be connected to this one */
                storing_diff = 1;
                unres->store_diff = 0;
            }

            if (!last_parent) {
                if (*root) {
                    lllyd_insert_common((*root)->parent, root, subroot, 0);
                } else {
                    *root = subroot;
                }
            }
            last_parent = subroot;

            /* remember the created container in unres */
            if (check_when_must) {
                if ((subroot->when_status & LLLYD_WHEN) && unres_data_add(unres, subroot, UNRES_WHEN) == -1) {
                    goto error;
                }
                if (check_when_must == 2) {
                    i = resolve_applies_must(subroot);
                    if ((i & 0x1) && (unres_data_add(unres, subroot, UNRES_MUST) == -1)) {
                        goto error;
                    }
                    if ((i & 0x2) && (unres_data_add(unres, subroot, UNRES_MUST_INOUT) == -1)) {
                        goto error;
                    }
                }
            }
        } else if (!((struct lllys_node_container *)schema)->presence) {
            /* fix default flag on existing containers - set it on all non-presence containers and in case we will
             * have in recursion function some non-default node, it will unset it */
            subroot->dflt = 1;
        }
        /* falls through */
    case LLLYS_CASE:
    case LLLYS_USES:
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
    case LLLYS_NOTIF:

        /* recursion */
        present = llly_set_new();
        if (!present) {
            goto error;
        }
        LLLY_TREE_FOR(schema->child, siter) {
            if (siter->nodetype & (LLLYS_CHOICE | LLLYS_USES)) {
                /* go into without searching for data instance */
                if (lllyd_wd_add_subtree(root, last_parent, subroot, siter, toplevel, options, unres)) {
                    goto error;
                }
            } else if (siter->nodetype & (LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA)) {
                /* search for the schema node instance */
                if (subroot && lllyd_get_node_siblings(subroot->child, siter, present)) {
                    /* there are some instances in the data root */
                    if (siter->nodetype & LLLYS_LEAFLIST) {
                        /* already have some leaflists, check that they are all
                         * default, if not, remove the default leaflists */
                        lllyd_wd_leaflist_cleanup(present, unres);
                    } else if (siter->nodetype != LLLYS_LEAF) {
                        /* recursion */
                        for (i = 0; i < (signed)present->number; i++) {
                            if (lllyd_wd_add_subtree(root, present->set.d[i], present->set.d[i], siter, toplevel, options,
                                                   unres)) {
                                goto error;
                            }
                        }
                    } /* else LLLYS_LEAF - nothing to do */

                    /* fix default flag (2nd part) - for non-default node with default parent, unset the default flag
                     * from the parents (starting from subroot node) */
                    if (subroot->dflt) {
                        for (i = 0; i < (signed)present->number; i++) {
                            if (!present->set.d[i]->dflt) {
                                for (iter = subroot; iter && iter->dflt; iter = iter->parent) {
                                    iter->dflt = 0;
                                }
                                break;
                            }
                        }
                    }
                    llly_set_clean(present);
                } else {
                    /* no instance */
                    if (lllyd_wd_add_subtree(root, last_parent, NULL, siter, toplevel, options, unres)) {
                        goto error;
                    }
                }
            }
        }

        if (storing_diff) {
            /* continue generating the diff in functions above this one */
            unres->store_diff = 1;
        }
        break;
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
        if (subroot) {
            /* default shortcase of a choice */
            present = llly_set_new();
            if (!present) {
                goto error;
            }
            lllyd_get_node_siblings(subroot->child, schema, present);
            if (present->number) {
                /* the shortcase leaf(-list) exists, stop the processing and fix default flags */
                if (subroot->dflt) {
                    for (i = 0; i < (signed)present->number; i++) {
                        if (!present->set.d[i]->dflt) {
                            for (iter = subroot; iter && iter->dflt; iter = iter->parent) {
                                iter->dflt = 0;
                            }
                            break;
                        }
                    }
                }
                break;
            }
        }
        if (schema->nodetype == LLLYS_LEAF) {
            if (lllyd_wd_add_leaf(root, last_parent, (struct lllys_node_leaf*)schema, unres, check_when_must)) {
                return EXIT_FAILURE;
            }
        } else { /* LLLYS_LEAFLIST */
            if (lllyd_wd_add_leaflist(root, last_parent, (struct lllys_node_leaflist*)schema, unres, check_when_must)) {
                goto error;
            }
        }
        break;
    case LLLYS_CHOICE:
        /* get existing node in the data root from the choice */
        iter = NULL;
        if ((toplevel && (*root)) || (!toplevel && subroot)) {
            LLLY_TREE_FOR(toplevel ? (*root) : subroot->child, iter) {
                for (siter = lllys_parent(iter->schema), siter_prev = iter->schema;
                        siter && (siter->nodetype & (LLLYS_CASE | LLLYS_USES | LLLYS_CHOICE));
                        siter_prev = siter, siter = lllys_parent(siter)) {
                    if (siter == schema) {
                        /* we have the choice instance */
                        break;
                    }
                }
                if (siter == schema) {
                    /* we have the choice instance;
                     * the condition must be the same as in the loop because of
                     * choice's sibling nodes that break the loop, so siter is not NULL,
                     * but it is not the same as schema */
                    break;
                }
            }
        }
        if (!iter) {
            if (((struct lllys_node_choice *)schema)->dflt) {
                /* there is a default case */
                if (lllyd_wd_add_subtree(root, last_parent, subroot, ((struct lllys_node_choice *)schema)->dflt,
                                       toplevel, options, unres)) {
                    goto error;
                }
            }
        } else {
            /* one of the choice's cases is instantiated, continue into this case */
            /* since iter != NULL, siter must be also != NULL and we also know siter_prev
             * which points to the child of schema leading towards the instantiated data */
            assert(siter && siter_prev);
            if (lllyd_wd_add_subtree(root, last_parent, subroot, siter_prev, toplevel, options, unres)) {
                goto error;
            }
        }
        break;
    default:
        /* LLLYS_ANYXML, LLLYS_ANYDATA, LLLYS_USES, LLLYS_GROUPING - do nothing */
        break;
    }

    llly_set_free(present);
    return EXIT_SUCCESS;

error:
    llly_set_free(present);
    return EXIT_FAILURE;
}

/**
 * @brief Covering function to process (add/clean) default nodes in the data tree
 * @param[in,out] root Pointer to the root node of the complete data tree, the root node can be NULL if the data tree
 *                     is empty
 * @param[in] ctx      Context for the case the data tree is empty (in that case \p ctx must not be NULL)
 * @param[in] options  Parser options to know the data tree type, see @ref parseroptions.
 * @param[in] unres    Unresolved data list, the newly added default nodes may need to add some unresolved items
 * @return EXIT_SUCCESS or EXIT_FAILURE
 */
static int
lllyd_wd_add(struct lllyd_node **root, struct llly_ctx *ctx, const struct lllys_module **modules, int mod_count,
           struct unres_data *unres, int options)
{
    struct lllys_node *siter;
    int i;

    assert(root && !(options & LLLYD_OPT_ACT_NOTIF));
    assert(*root || ctx);
    assert(!(options & LLLYD_OPT_NOSIBLINGS) || *root);

    if (options & (LLLYD_OPT_EDIT | LLLYD_OPT_GET | LLLYD_OPT_GETCONFIG)) {
        /* no change supposed */
        return EXIT_SUCCESS;
    }

    if (!ctx) {
        ctx = (*root)->schema->module->ctx;
    }

    if (!(options & LLLYD_OPT_TYPEMASK) || (options & LLLYD_OPT_CONFIG)) {
        if (options & LLLYD_OPT_NOSIBLINGS) {
            if (lllyd_wd_add_subtree(root, NULL, NULL, (*root)->schema, 1, options, unres)) {
                return EXIT_FAILURE;
            }
        } else if (modules && mod_count) {
            for (i = 0; i < mod_count; ++i) {
                LLLY_TREE_FOR(modules[i]->data, siter) {
                    if (!(siter->nodetype & (LLLYS_CONTAINER | LLLYS_CHOICE | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA |
                                             LLLYS_USES))) {
                        continue;
                    }
                    if (lllyd_wd_add_subtree(root, NULL, NULL, siter, 1, options, unres)) {
                        return EXIT_FAILURE;
                    }
                }
            }
        } else {
            for (i = 0; i < ctx->models.used; i++) {
                /* skip not implemented and disabled modules */
                if (!ctx->models.list[i]->implemented || ctx->models.list[i]->disabled) {
                    continue;
                }
                LLLY_TREE_FOR(ctx->models.list[i]->data, siter) {
                    if (!(siter->nodetype & (LLLYS_CONTAINER | LLLYS_CHOICE | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA |
                                             LLLYS_USES))) {
                        continue;
                    }
                    if (lllyd_wd_add_subtree(root, NULL, NULL, siter, 1, options, unres)) {
                        return EXIT_FAILURE;
                    }
                }
            }
        }
    } else if (options & LLLYD_OPT_NOTIF) {
        if (!(*root) || ((*root)->schema->nodetype != LLLYS_NOTIF)) {
            LOGERR(ctx, LLLY_EINVAL, "Subtree is not a single notification.");
            return EXIT_FAILURE;
        }
        if (lllyd_wd_add_subtree(root, *root, *root, (*root)->schema, 0, options, unres)) {
            return EXIT_FAILURE;
        }
    } else if (options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY)) {
        if (!(*root) || !((*root)->schema->nodetype & (LLLYS_RPC | LLLYS_ACTION))) {
            LOGERR(ctx, LLLY_EINVAL, "Subtree is not a single RPC/action/reply.");
            return EXIT_FAILURE;
        }
        if (options & LLLYD_OPT_RPC) {
            for (siter = (*root)->schema->child; siter && siter->nodetype != LLLYS_INPUT; siter = siter->next);
        } else { /* LLLYD_OPT_RPCREPLY */
            for (siter = (*root)->schema->child; siter && siter->nodetype != LLLYS_OUTPUT; siter = siter->next);
        }
        if (siter) {
            if (lllyd_wd_add_subtree(root, *root, *root, siter, 0, options, unres)) {
                return EXIT_FAILURE;
            }
        }
    } else if (options & LLLYD_OPT_DATA_TEMPLATE) {
        if (lllyd_wd_add_subtree(root, NULL, NULL, (*root)->schema, 1, options, unres)) {
            return EXIT_FAILURE;
        }
    } else {
        LOGINT(ctx);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int
lllyd_defaults_add_unres(struct lllyd_node **root, int options, struct llly_ctx *ctx, const struct lllys_module **modules,
                       int mod_count, const struct lllyd_node *data_tree, struct lllyd_node *act_notif,
                       struct unres_data *unres, int wd)
{
    struct lllyd_node *msg_sibling = NULL, *msg_parent = NULL, *data_tree_sibling, *data_tree_parent;
    struct lllys_node *msg_op = NULL;
    struct llly_set *set;
    int ret = EXIT_FAILURE;

    assert(root && (*root || ctx) && unres && !(options & LLLYD_OPT_ACT_NOTIF));

    if (!ctx) {
        ctx = (*root)->schema->module->ctx;
    }

    if ((options & LLLYD_OPT_NOSIBLINGS) && !(*root)) {
        LOGERR(ctx, LLLY_EINVAL, "Cannot add default values for one module (LLLYD_OPT_NOSIBLINGS) without any data.");
        return EXIT_FAILURE;
    }

    if (options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY | LLLYD_OPT_NOTIF)) {
        if (!(*root)) {
            LOGERR(ctx, LLLY_EINVAL, "Cannot add default values to RPC, RPC reply, and notification without at least the empty container.");
            return EXIT_FAILURE;
        }
        if ((options & LLLYD_OPT_RPC) && !act_notif && ((*root)->schema->nodetype != LLLYS_RPC)) {
            LOGERR(ctx, LLLY_EINVAL, "Not valid RPC/action data.");
            return EXIT_FAILURE;
        }
        if ((options & LLLYD_OPT_RPCREPLY) && !act_notif && ((*root)->schema->nodetype != LLLYS_RPC)) {
            LOGERR(ctx, LLLY_EINVAL, "Not valid reply data.");
            return EXIT_FAILURE;
        }
        if ((options & LLLYD_OPT_NOTIF) && !act_notif && ((*root)->schema->nodetype != LLLYS_NOTIF)) {
            LOGERR(ctx, LLLY_EINVAL, "Not valid notification data.");
            return EXIT_FAILURE;
        }

        /* remember the operation/notification schema */
        msg_op = act_notif ? act_notif->schema : (*root)->schema;
    } else if (*root && (*root)->parent) {
        /* we have inner node, so it will be considered as
         * a root of subtree where to add default nodes and
         * no of its siblings will be affected */
        options |= LLLYD_OPT_NOSIBLINGS;
    }

    /* add missing default nodes */
    if (wd && lllyd_wd_add((act_notif ? &act_notif : root), ctx, modules, mod_count, unres, options)) {
        return EXIT_FAILURE;
    }

    /* check leafrefs and/or instids if any */
    if (unres && unres->count) {
        if (!(*root)) {
            LOGINT(ctx);
            return EXIT_FAILURE;
        }

        /* temporarily link the additional data tree to the RPC/action/notification */
        if (data_tree && (options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY | LLLYD_OPT_NOTIF))) {
            /* duplicate the message tree - if it gets deleted we would not be able to positively identify it */
            msg_parent = NULL;
            msg_sibling = *root;

            if (act_notif) {
                /* fun case */
                data_tree_parent = NULL;
                data_tree_sibling = (struct lllyd_node *)data_tree;
                while (data_tree_sibling) {
                    while (data_tree_sibling) {
                        if ((data_tree_sibling->schema == msg_sibling->schema)
                                && ((msg_sibling->schema->nodetype != LLLYS_LIST)
                                    || lllyd_list_equal(data_tree_sibling, msg_sibling, 0))) {
                            /* match */
                            break;
                        }

                        data_tree_sibling = data_tree_sibling->next;
                    }

                    if (data_tree_sibling) {
                        /* prepare for the new data_tree iteration */
                        data_tree_parent = data_tree_sibling;
                        data_tree_sibling = data_tree_sibling->child;

                        /* find new action sibling to search for later (skip list keys) */
                        msg_parent = msg_sibling;
                        assert(msg_sibling->child);
                        for (msg_sibling = msg_sibling->child;
                                msg_sibling->schema->nodetype == LLLYS_LEAF;
                                msg_sibling = msg_sibling->next) {
                            assert(msg_sibling->next);
                        }
                        if (msg_sibling->schema->nodetype & (LLLYS_ACTION | LLLYS_NOTIF)) {
                            /* we are done */
                            assert(act_notif->parent);
                            assert(act_notif->parent->schema == data_tree_parent->schema);
                            assert(msg_sibling == act_notif);
                            break;
                        }
                    }
                }

                /* loop ended after the first iteration, set the values correctly */
                if (!data_tree_parent) {
                    data_tree_sibling = (struct lllyd_node *)data_tree;
                }

            } else {
                /* easy case */
                data_tree_parent = NULL;
                data_tree_sibling = (struct lllyd_node *)data_tree;
            }

            /* unlink msg_sibling if needed (won't do anything otherwise) */
            lllyd_unlink_internal(msg_sibling, 0);

            /* now we can insert msg_sibling into data_tree_parent or next to data_tree_sibling */
            assert(data_tree_parent || data_tree_sibling);
            if (data_tree_parent) {
                if (lllyd_insert_common(data_tree_parent, NULL, msg_sibling, 0)) {
                    goto unlink_datatree;
                }
            } else {
                assert(!data_tree_sibling->parent);
                if (lllyd_insert_nextto(data_tree_sibling->prev, msg_sibling, 0, 0)) {
                    goto unlink_datatree;
                }
            }
        }

        if (resolve_unres_data(ctx, unres, root, options)) {
            goto unlink_datatree;
        }

        /* we are done */
        ret = EXIT_SUCCESS;

        /* check that the operation/notification tree was not removed */
        if (options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY | LLLYD_OPT_NOTIF)) {
            set = NULL;
            if (data_tree) {
                set = lllyd_find_instance(data_tree_parent ? data_tree_parent : data_tree_sibling, msg_op);
                assert(set && ((set->number == 0) || (set->number == 1)));
            } else if (*root) {
                set = lllyd_find_instance(*root, msg_op);
                assert(set && ((set->number == 0) || (set->number == 1)));
            }
            if (!set || !set->number) {
                /* it was removed, handle specially */
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_LYS, msg_op, "Operation/notification not supported because of the current configuration.");
                ret = EXIT_FAILURE;
            }
            llly_set_free(set);
        }

unlink_datatree:
        /* put the trees back in order */
        if (data_tree && (options & (LLLYD_OPT_RPC | LLLYD_OPT_RPCREPLY | LLLYD_OPT_NOTIF))) {
            /* unlink and insert it back, if there is a parent  */
            lllyd_unlink_internal(msg_sibling, 0);
            if (msg_parent) {
                lllyd_insert_common(msg_parent, NULL, msg_sibling, 0);
            }
        }
    } else {
        /* we are done */
        ret = EXIT_SUCCESS;
    }

    return ret;
}

API struct lllys_module *
lllyd_node_module(const struct lllyd_node *node)
{
    FUN_IN;

    if (!node) {
        return NULL;
    }

    return node->schema->module->type ? ((struct lllys_submodule *)node->schema->module)->belongsto : node->schema->module;
}

API double
lllyd_dec64_to_double(const struct lllyd_node *node)
{
    FUN_IN;

    if (!node || !(node->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))
            || (((struct lllys_node_leaf *)node->schema)->type.base != LLLY_TYPE_DEC64)) {
        LOGARG;
        return 0;
    }

    return atof(((struct lllyd_node_leaf_list *)node)->value_str);
}

API const struct lllys_type *
lllyd_leaf_type(const struct lllyd_node_leaf_list *leaf)
{
    FUN_IN;

    struct lllys_type *type;

    if (!leaf || !(leaf->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
        return NULL;
    }

    type = &((struct lllys_node_leaf *)leaf->schema)->type;

    do {
        if (type->base == LLLY_TYPE_LEAFREF) {
            type = &type->info.lref.target->type;
        } else if (type->base == LLLY_TYPE_UNION) {
            if (type->info.uni.has_ptr_type && leaf->validity) {
                /* we don't know what it will be after resolution (validation) */
                LOGVAL(leaf->schema->module->ctx, LLLYE_SPEC, LLLY_VLOG_LYD, leaf,
                       "Unable to determine the type of value \"%s\" from union type \"%s\" prior to validation.",
                       leaf->value_str, type->der->name);
                return NULL;
            }

            if (resolve_union((struct lllyd_node_leaf_list *)leaf, type, 0, 0, &type)) {
                /* resolve union failed */
                return NULL;
            }
        }
    } while (type->base == LLLY_TYPE_LEAFREF);

    return type;
}

#ifdef LLLY_ENABLED_LYD_PRIV

API void *
lllyd_set_private(const struct lllyd_node *node, void *priv)
{
    FUN_IN;

    void *prev;

    if (!node) {
        LOGARG;
        return NULL;
    }

    prev = node->priv;
    ((struct lllyd_node *)node)->priv = priv;

    return prev;
}

#endif
