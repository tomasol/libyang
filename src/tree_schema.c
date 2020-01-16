/**
 * @file tree_schema.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Manipulation with libyang schema data structures
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

#ifdef __APPLE__
#   include <sys/param.h>
#endif
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>

#include "common.h"
#include "context.h"
#include "parser.h"
#include "resolve.h"
#include "xml.h"
#include "xpath.h"
#include "xml_internal.h"
#include "tree_internal.h"
#include "validation.h"
#include "parser_yang.h"

static int lllys_type_dup(struct lllys_module *mod, struct lllys_node *parent, struct lllys_type *new, struct lllys_type *old,
                        int in_grp, int shallow, struct unres_schema *unres);

API const struct lllys_node_list *
lllys_is_key(const struct lllys_node_leaf *node, uint8_t *index)
{
    FUN_IN;

    struct lllys_node *parent = (struct lllys_node *)node;
    struct lllys_node_list *list;
    uint8_t i;

    if (!node || node->nodetype != LLLYS_LEAF) {
        return NULL;
    }

    do {
        parent = lllys_parent(parent);
    } while (parent && parent->nodetype == LLLYS_USES);

    if (!parent || parent->nodetype != LLLYS_LIST) {
        return NULL;
    }

    list = (struct lllys_node_list*)parent;
    for (i = 0; i < list->keys_size; i++) {
        if (list->keys[i] == node) {
            if (index) {
                (*index) = i;
            }
            return list;
        }
    }
    return NULL;
}

API const struct lllys_node *
lllys_is_disabled(const struct lllys_node *node, int recursive)
{
    FUN_IN;

    int i;

    if (!node) {
        return NULL;
    }

check:
    if (node->nodetype != LLLYS_INPUT && node->nodetype != LLLYS_OUTPUT) {
        /* input/output does not have if-feature, so skip them */

        /* check local if-features */
        for (i = 0; i < node->iffeature_size; i++) {
            if (!resolve_iffeature(&node->iffeature[i])) {
                return node;
            }
        }
    }

    if (!recursive) {
        return NULL;
    }

    /* go through parents */
    if (node->nodetype == LLLYS_AUGMENT) {
        /* go to parent actually means go to the target node */
        node = ((struct lllys_node_augment *)node)->target;
        if (!node) {
            /* unresolved augment, let's say it's enabled */
            return NULL;
        }
    } else if (node->nodetype == LLLYS_EXT) {
        return NULL;
    } else if (node->parent) {
        node = node->parent;
    } else {
        return NULL;
    }

    if (recursive == 2) {
        /* continue only if the node cannot have a data instance */
        if (node->nodetype & (LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST)) {
            return NULL;
        }
    }
    goto check;
}

API int
lllys_iffeature_value(const struct lllys_iffeature *iff)
{
    return resolve_iffeature((struct lllys_iffeature *)iff);
}

API const struct lllys_type *
lllys_getnext_union_type(const struct lllys_type *last, const struct lllys_type *type)
{
    FUN_IN;

    int found = 0;

    if (!type || (type->base != LLLY_TYPE_UNION)) {
        return NULL;
    }

    return lllyp_get_next_union_type((struct lllys_type *)type, (struct lllys_type *)last, &found);
}

int
lllys_get_sibling(const struct lllys_node *siblings, const char *mod_name, int mod_name_len, const char *name,
                int nam_len, LLLYS_NODE type, const struct lllys_node **ret)
{
    const struct lllys_node *node, *parent = NULL;
    const struct lllys_module *mod = NULL;
    const char *node_mod_name;

    assert(siblings && mod_name && name);
    assert(!(type & (LLLYS_USES | LLLYS_GROUPING)));

    /* fill the lengths in case the caller is so indifferent */
    if (!mod_name_len) {
        mod_name_len = strlen(mod_name);
    }
    if (!nam_len) {
        nam_len = strlen(name);
    }

    while (siblings && (siblings->nodetype == LLLYS_USES)) {
        siblings = siblings->child;
    }
    if (!siblings) {
        /* unresolved uses */
        return EXIT_FAILURE;
    }

    if (siblings->nodetype == LLLYS_GROUPING) {
        for (node = siblings; (node->nodetype == LLLYS_GROUPING) && (node->prev != siblings); node = node->prev);
        if (node->nodetype == LLLYS_GROUPING) {
            /* we went through all the siblings, only groupings there - no valid sibling */
            return EXIT_FAILURE;
        }
        /* update siblings to be valid */
        siblings = node;
    }

    /* set parent correctly */
    parent = lllys_parent(siblings);

    /* go up all uses */
    while (parent && (parent->nodetype == LLLYS_USES)) {
        parent = lllys_parent(parent);
    }

    if (!parent) {
        /* handle situation when there is a top-level uses referencing a foreign grouping */
        for (node = siblings; lllys_parent(node) && (node->nodetype == LLLYS_USES); node = lllys_parent(node));
        mod = lllys_node_module(node);
    }

    /* try to find the node */
    node = NULL;
    while ((node = lllys_getnext(node, parent, mod, LLLYS_GETNEXT_WITHCHOICE | LLLYS_GETNEXT_WITHCASE | LLLYS_GETNEXT_WITHINOUT))) {
        if (!type || (node->nodetype & type)) {
            /* module name comparison */
            node_mod_name = lllys_node_module(node)->name;
            if (!llly_strequal(node_mod_name, mod_name, 1) && (strncmp(node_mod_name, mod_name, mod_name_len) || node_mod_name[mod_name_len])) {
                continue;
            }

            /* direct name check */
            if (llly_strequal(node->name, name, 1) || (!strncmp(node->name, name, nam_len) && !node->name[nam_len])) {
                if (ret) {
                    *ret = node;
                }
                return EXIT_SUCCESS;
            }
        }
    }

    return EXIT_FAILURE;
}

int
lllys_getnext_data(const struct lllys_module *mod, const struct lllys_node *parent, const char *name, int nam_len,
                 LLLYS_NODE type, int getnext_opts, const struct lllys_node **ret)
{
    const struct lllys_node *node;

    assert((mod || parent) && name);
    assert(!(type & (LLLYS_AUGMENT | LLLYS_USES | LLLYS_GROUPING | LLLYS_CHOICE | LLLYS_CASE | LLLYS_INPUT | LLLYS_OUTPUT)));

    if (!mod) {
        mod = lllys_node_module(parent);
    }

    /* try to find the node */
    node = NULL;
    while ((node = lllys_getnext(node, parent, mod, getnext_opts))) {
        if (!type || (node->nodetype & type)) {
            /* module check */
            if (lllys_node_module(node) != lllys_main_module(mod)) {
                continue;
            }

            /* direct name check */
            if (!strncmp(node->name, name, nam_len) && !node->name[nam_len]) {
                if (ret) {
                    *ret = node;
                }
                return EXIT_SUCCESS;
            }
        }
    }

    return EXIT_FAILURE;
}

API const struct lllys_node *
lllys_getnext(const struct lllys_node *last, const struct lllys_node *parent, const struct lllys_module *module, int options)
{
    FUN_IN;

    const struct lllys_node *next, *aug_parent;
    struct lllys_node **snode;

    if ((!parent && !module) || (module && module->type) || (parent && (parent->nodetype == LLLYS_USES) && !(options & LLLYS_GETNEXT_PARENTUSES))) {
        LOGARG;
        return NULL;
    }

    if (!last) {
        /* first call */

        /* get know where to start */
        if (parent) {
            /* schema subtree */
            snode = lllys_child(parent, LLLYS_UNKNOWN);
            /* do not return anything if the augment does not have any children */
            if (!snode || !(*snode) || ((parent->nodetype == LLLYS_AUGMENT) && ((*snode)->parent != parent))) {
                return NULL;
            }
            next = last = *snode;
        } else {
            /* top level data */
            if (!(options & LLLYS_GETNEXT_NOSTATECHECK) && (module->disabled || !module->implemented)) {
                /* nothing to return from a disabled/imported module */
                return NULL;
            }
            next = last = module->data;
        }
    } else if ((last->nodetype == LLLYS_USES) && (options & LLLYS_GETNEXT_INTOUSES) && last->child) {
        /* continue with uses content */
        next = last->child;
    } else {
        /* continue after the last returned value */
        next = last->next;
    }

repeat:
    if (parent && (parent->nodetype == LLLYS_AUGMENT) && next) {
        /* do not return anything outside the parent augment */
        aug_parent = next->parent;
        do {
            while (aug_parent && (aug_parent->nodetype != LLLYS_AUGMENT)) {
                aug_parent = aug_parent->parent;
            }
            if (aug_parent) {
                if (aug_parent == parent) {
                    break;
                }
                aug_parent = ((struct lllys_node_augment *)aug_parent)->target;
            }

        } while (aug_parent);
        if (!aug_parent) {
            return NULL;
        }
    }
    while (next && (next->nodetype == LLLYS_GROUPING)) {
        if (options & LLLYS_GETNEXT_WITHGROUPING) {
            return next;
        }
        next = next->next;
    }

    if (!next) {     /* cover case when parent is augment */
        if (!last || last->parent == parent || lllys_parent(last) == parent) {
            /* no next element */
            return NULL;
        }
        last = lllys_parent(last);
        next = last->next;
        goto repeat;
    } else {
        last = next;
    }

    if (!(options & LLLYS_GETNEXT_NOSTATECHECK) && lllys_is_disabled(next, 0)) {
        next = next->next;
        goto repeat;
    }

    switch (next->nodetype) {
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
        if (options & LLLYS_GETNEXT_WITHINOUT) {
            return next;
        } else if (next->child) {
            next = next->child;
        } else {
            next = next->next;
        }
        goto repeat;

    case LLLYS_CASE:
        if (options & LLLYS_GETNEXT_WITHCASE) {
            return next;
        } else if (next->child) {
            next = next->child;
        } else {
            next = next->next;
        }
        goto repeat;

    case LLLYS_USES:
        /* go into */
        if (options & LLLYS_GETNEXT_WITHUSES) {
            return next;
        } else if (next->child) {
            next = next->child;
        } else {
            next = next->next;
        }
        goto repeat;

    case LLLYS_RPC:
    case LLLYS_ACTION:
    case LLLYS_NOTIF:
    case LLLYS_LEAF:
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
    case LLLYS_LIST:
    case LLLYS_LEAFLIST:
        return next;

    case LLLYS_CONTAINER:
        if (!((struct lllys_node_container *)next)->presence && (options & LLLYS_GETNEXT_INTONPCONT)) {
            if (next->child) {
                /* go into */
                next = next->child;
            } else {
                next = next->next;
            }
            goto repeat;
        } else {
            return next;
        }

    case LLLYS_CHOICE:
        if (options & LLLYS_GETNEXT_WITHCHOICE) {
            return next;
        } else if (next->child) {
            /* go into */
            next = next->child;
        } else {
            next = next->next;
        }
        goto repeat;

    default:
        /* we should not be here */
        return NULL;
    }
}

void
lllys_node_unlink(struct lllys_node *node)
{
    struct lllys_node *parent, *first, **pp = NULL;
    struct lllys_module *main_module;

    if (!node) {
        return;
    }

    /* unlink from data model if necessary */
    if (node->module) {
        /* get main module with data tree */
        main_module = lllys_node_module(node);
        if (main_module->data == node) {
            main_module->data = node->next;
        }
    }

    /* store pointers to important nodes */
    parent = node->parent;
    if (parent && (parent->nodetype == LLLYS_AUGMENT)) {
        /* handle augments - first, unlink it from the augment parent ... */
        if (parent->child == node) {
            parent->child = (node->next && node->next->parent == parent) ? node->next : NULL;
        }

        if (parent->flags & LLLYS_NOTAPPLIED) {
            /* data are not connected in the target, so we cannot continue with the target as a parent */
            parent = NULL;
        } else {
            /* data are connected in target, so we will continue with the target as a parent */
            parent = ((struct lllys_node_augment *)parent)->target;
        }
    }

    /* unlink from parent */
    if (parent) {
        if (parent->nodetype == LLLYS_EXT) {
            pp = (struct lllys_node **)lllys_ext_complex_get_substmt(lllys_snode2stmt(node->nodetype),
                                                                 (struct lllys_ext_instance_complex*)parent, NULL);
            if (*pp == node) {
                *pp = node->next;
            }
        } else if (parent->child == node) {
            parent->child = node->next;
        }
        node->parent = NULL;
    }

    /* unlink from siblings */
    if (node->prev == node) {
        /* there are no more siblings */
        return;
    }
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        /* unlinking the last element */
        if (parent) {
            if (parent->nodetype == LLLYS_EXT) {
                first = *(struct lllys_node **)pp;
            } else {
                first = parent->child;
            }
        } else {
            first = node;
            while (first->prev->next) {
                first = first->prev;
            }
        }
        first->prev = node->prev;
    }
    if (node->prev->next) {
        node->prev->next = node->next;
    }

    /* clean up the unlinked element */
    node->next = NULL;
    node->prev = node;
}

struct lllys_node_grp *
lllys_find_grouping_up(const char *name, struct lllys_node *start)
{
    struct lllys_node *par_iter, *iter, *stop;

    for (par_iter = start; par_iter; par_iter = par_iter->parent) {
        /* top-level augment, look into module (uses augment is handled correctly below) */
        if (par_iter->parent && !par_iter->parent->parent && (par_iter->parent->nodetype == LLLYS_AUGMENT)) {
            par_iter = lllys_main_module(par_iter->parent->module)->data;
            if (!par_iter) {
                break;
            }
        }

        if (par_iter->nodetype == LLLYS_EXT) {
            /* we are in a top-level extension, search grouping in top-level groupings */
            par_iter = lllys_main_module(par_iter->module)->data;
            if (!par_iter) {
                /* not connected yet, wait */
                return NULL;
            }
        } else if (par_iter->parent && (par_iter->parent->nodetype & (LLLYS_CHOICE | LLLYS_CASE | LLLYS_AUGMENT | LLLYS_USES))) {
            continue;
        }

        for (iter = par_iter, stop = NULL; iter; iter = iter->prev) {
            if (!stop) {
                stop = par_iter;
            } else if (iter == stop) {
                break;
            }
            if (iter->nodetype != LLLYS_GROUPING) {
                continue;
            }

            if (!strcmp(name, iter->name)) {
                return (struct lllys_node_grp *)iter;
            }
        }
    }

    return NULL;
}

/*
 * get next grouping in the root's subtree, in the
 * first call, tha last is NULL
 */
static struct lllys_node_grp *
lllys_get_next_grouping(struct lllys_node_grp *lastgrp, struct lllys_node *root)
{
    struct lllys_node *last = (struct lllys_node *)lastgrp;
    struct lllys_node *next;

    assert(root);

    if (!last) {
        last = root;
    }

    while (1) {
        if ((last->nodetype & (LLLYS_CONTAINER | LLLYS_CHOICE | LLLYS_LIST | LLLYS_GROUPING | LLLYS_INPUT | LLLYS_OUTPUT))) {
            next = last->child;
        } else {
            next = NULL;
        }
        if (!next) {
            if (last == root) {
                /* we are done */
                return NULL;
            }

            /* no children, go to siblings */
            next = last->next;
        }
        while (!next) {
            /* go back through parents */
            if (lllys_parent(last) == root) {
                /* we are done */
                return NULL;
            }
            next = last->next;
            last = lllys_parent(last);
        }

        if (next->nodetype == LLLYS_GROUPING) {
            return (struct lllys_node_grp *)next;
        }

        last = next;
    }
}

/* logs directly */
int
lllys_check_id(struct lllys_node *node, struct lllys_node *parent, struct lllys_module *module)
{
    struct lllys_node *start, *stop, *iter;
    struct lllys_node_grp *grp;
    int down, up;

    assert(node);

    if (!parent) {
        assert(module);
    } else {
        module = parent->module;
    }
    module = lllys_main_module(module);

    switch (node->nodetype) {
    case LLLYS_GROUPING:
        /* 6.2.1, rule 6 */
        if (parent) {
            start = *lllys_child(parent, LLLYS_GROUPING);
            if (!start) {
                down = 0;
                start = parent;
            } else {
                down = 1;
            }
            if (parent->nodetype == LLLYS_EXT) {
                up = 0;
            } else {
                up = 1;
            }
        } else {
            down = up = 1;
            start = module->data;
        }
        /* go up */
        if (up && lllys_find_grouping_up(node->name, start)) {
            LOGVAL(module->ctx, LLLYE_DUPID, LLLY_VLOG_LYS, node, "grouping", node->name);
            return EXIT_FAILURE;
        }
        /* go down, because grouping can be defined after e.g. container in which is collision */
        if (down) {
            for (iter = start, stop = NULL; iter; iter = iter->prev) {
                if (!stop) {
                    stop = start;
                } else if (iter == stop) {
                    break;
                }
                if (!(iter->nodetype & (LLLYS_CONTAINER | LLLYS_CHOICE | LLLYS_LIST | LLLYS_GROUPING | LLLYS_INPUT | LLLYS_OUTPUT))) {
                    continue;
                }

                grp = NULL;
                while ((grp = lllys_get_next_grouping(grp, iter))) {
                    if (llly_strequal(node->name, grp->name, 1)) {
                        LOGVAL(module->ctx, LLLYE_DUPID,LLLY_VLOG_LYS, node, "grouping", node->name);
                        return EXIT_FAILURE;
                    }
                }
            }
        }
        break;
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
    case LLLYS_LIST:
    case LLLYS_CONTAINER:
    case LLLYS_CHOICE:
    case LLLYS_RPC:
    case LLLYS_NOTIF:
    case LLLYS_ACTION:
    case LLLYS_ANYDATA:
        /* 6.2.1, rule 7 */
        if (parent) {
            iter = parent;
            while (iter && (iter->nodetype & (LLLYS_USES | LLLYS_CASE | LLLYS_CHOICE | LLLYS_AUGMENT))) {
                if (iter->nodetype == LLLYS_AUGMENT) {
                    if (((struct lllys_node_augment *)iter)->target) {
                        /* augment is resolved, go up */
                        iter = ((struct lllys_node_augment *)iter)->target;
                        continue;
                    }
                    /* augment is not resolved, this is the final parent */
                    break;
                }
                iter = iter->parent;
            }

            if (!iter) {
                stop = NULL;
                iter = module->data;
            } else if (iter->nodetype == LLLYS_EXT) {
                stop = iter;
                iter = (struct lllys_node *)lllys_child(iter, node->nodetype);
                if (iter) {
                    iter = *(struct lllys_node **)iter;
                }
            } else {
                stop = iter;
                iter = iter->child;
            }
        } else {
            stop = NULL;
            iter = module->data;
        }
        while (iter) {
            if (iter->nodetype & (LLLYS_USES | LLLYS_CASE)) {
                iter = iter->child;
                continue;
            }

            if (iter->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_CONTAINER | LLLYS_CHOICE | LLLYS_RPC | LLLYS_NOTIF | LLLYS_ACTION | LLLYS_ANYDATA)) {
                if (lllys_node_module(iter) == lllys_node_module(node) && llly_strequal(iter->name, node->name, 1)) {
                    LOGVAL(module->ctx, LLLYE_DUPID, LLLY_VLOG_LYS, node, strnodetype(node->nodetype), node->name);
                    return EXIT_FAILURE;
                }
            }

            /* special case for choice - we must check the choice's name as
             * well as the names of nodes under the choice
             */
            if (iter->nodetype == LLLYS_CHOICE) {
                iter = iter->child;
                continue;
            }

            /* go to siblings */
            if (!iter->next) {
                /* no sibling, go to parent's sibling */
                do {
                    /* for parent LLLYS_AUGMENT */
                    if (iter->parent == stop) {
                        iter = stop;
                        break;
                    }
                    iter = lllys_parent(iter);
                    if (iter && iter->next) {
                        break;
                    }
                } while (iter != stop);

                if (iter == stop) {
                    break;
                }
            }
            iter = iter->next;
        }
        break;
    case LLLYS_CASE:
        /* 6.2.1, rule 8 */
        if (parent) {
            start = *lllys_child(parent, LLLYS_CASE);
        } else {
            start = module->data;
        }

        LLLY_TREE_FOR(start, iter) {
            if (!(iter->nodetype & (LLLYS_ANYDATA | LLLYS_CASE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST))) {
                continue;
            }

            if (iter->module == node->module && llly_strequal(iter->name, node->name, 1)) {
                LOGVAL(module->ctx, LLLYE_DUPID, LLLY_VLOG_LYS, node, "case", node->name);
                return EXIT_FAILURE;
            }
        }
        break;
    default:
        /* no check needed */
        break;
    }

    return EXIT_SUCCESS;
}

/* logs directly */
int
lllys_node_addchild(struct lllys_node *parent, struct lllys_module *module, struct lllys_node *child, int options)
{
    struct llly_ctx *ctx = child->module->ctx;
    struct lllys_node *iter, **pchild, *log_parent;
    struct lllys_node_inout *in, *out;
    struct lllys_node_case *c;
    struct lllys_node_augment *aug;
    int type, shortcase = 0;
    void *p;
    struct lllyext_substmt *info = NULL;

    assert(child);

    if (parent) {
        type = parent->nodetype;
        module = parent->module;
        log_parent = parent;

        if (type == LLLYS_USES) {
            /* we are adding children to uses -> we must be copying grouping contents into it, so properly check the parent */
            while (log_parent && (log_parent->nodetype == LLLYS_USES)) {
                if (log_parent->nodetype == LLLYS_AUGMENT) {
                    aug = (struct lllys_node_augment *)log_parent;
                    if (!aug->target) {
                        /* unresolved augment, just pass the node type check */
                        goto skip_nodetype_check;
                    }
                    log_parent = aug->target;
                } else {
                    log_parent = log_parent->parent;
                }
            }
            if (log_parent) {
                type = log_parent->nodetype;
            } else {
                type = 0;
            }
        }
    } else {
        assert(module);
        assert(!(child->nodetype & (LLLYS_INPUT | LLLYS_OUTPUT)));
        type = 0;
        log_parent = NULL;
    }

    /* checks */
    switch (type) {
    case LLLYS_CONTAINER:
    case LLLYS_LIST:
    case LLLYS_GROUPING:
    case LLLYS_USES:
        if (!(child->nodetype &
                (LLLYS_ANYDATA | LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_GROUPING | LLLYS_LEAF |
                 LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_USES | LLLYS_ACTION | LLLYS_NOTIF))) {
            LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_LYS, log_parent, strnodetype(child->nodetype), strnodetype(log_parent->nodetype));
            return EXIT_FAILURE;
        }
        break;
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
    case LLLYS_NOTIF:
        if (!(child->nodetype &
                (LLLYS_ANYDATA | LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_GROUPING | LLLYS_LEAF |
                 LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_USES))) {
            LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_LYS, log_parent, strnodetype(child->nodetype), strnodetype(log_parent->nodetype));
            return EXIT_FAILURE;
        }
        break;
    case LLLYS_CHOICE:
        if (!(child->nodetype &
                (LLLYS_ANYDATA | LLLYS_CASE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_CHOICE))) {
            LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_LYS, log_parent, strnodetype(child->nodetype), "choice");
            return EXIT_FAILURE;
        }
        if (child->nodetype != LLLYS_CASE) {
            shortcase = 1;
        }
        break;
    case LLLYS_CASE:
        if (!(child->nodetype &
                (LLLYS_ANYDATA | LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_USES))) {
            LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_LYS, log_parent, strnodetype(child->nodetype), "case");
            return EXIT_FAILURE;
        }
        break;
    case LLLYS_RPC:
    case LLLYS_ACTION:
        if (!(child->nodetype & (LLLYS_INPUT | LLLYS_OUTPUT | LLLYS_GROUPING))) {
            LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_LYS, log_parent, strnodetype(child->nodetype), "rpc");
            return EXIT_FAILURE;
        }
        break;
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_LYS, log_parent, strnodetype(child->nodetype), strnodetype(log_parent->nodetype));
        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "The \"%s\" statement cannot have any data substatement.",
               strnodetype(log_parent->nodetype));
        return EXIT_FAILURE;
    case LLLYS_AUGMENT:
        if (!(child->nodetype &
                (LLLYS_ANYDATA | LLLYS_CASE | LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF
                | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_USES | LLLYS_ACTION | LLLYS_NOTIF))) {
            LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_LYS, log_parent, strnodetype(child->nodetype), strnodetype(log_parent->nodetype));
            return EXIT_FAILURE;
        }
        break;
    case LLLYS_UNKNOWN:
        /* top level */
        if (!(child->nodetype &
                (LLLYS_ANYDATA | LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_GROUPING
                | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_USES | LLLYS_RPC | LLLYS_NOTIF | LLLYS_AUGMENT))) {
            LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_LYS, log_parent, strnodetype(child->nodetype), "(sub)module");
            return EXIT_FAILURE;
        }
        break;
    case LLLYS_EXT:
        /* plugin-defined */
        p = lllys_ext_complex_get_substmt(lllys_snode2stmt(child->nodetype), (struct lllys_ext_instance_complex*)log_parent, &info);
        if (!p) {
            LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_LYS, log_parent, strnodetype(child->nodetype),
                   ((struct lllys_ext_instance_complex*)log_parent)->def->name);
            return EXIT_FAILURE;
        }
        /* TODO check cardinality */
        break;
    }

skip_nodetype_check:
    /* check identifier uniqueness */
    if (!(module->ctx->models.flags & LLLY_CTX_TRUSTED) && lllys_check_id(child, parent, module)) {
        return EXIT_FAILURE;
    }

    if (child->parent) {
        lllys_node_unlink(child);
    }

    if ((child->nodetype & (LLLYS_INPUT | LLLYS_OUTPUT)) && parent->nodetype != LLLYS_EXT) {
        /* find the implicit input/output node */
        LLLY_TREE_FOR(parent->child, iter) {
            if (iter->nodetype == child->nodetype) {
                break;
            }
        }
        assert(iter);

        /* switch the old implicit node (iter) with the new one (child) */
        if (parent->child == iter) {
            /* first child */
            parent->child = child;
        } else {
            iter->prev->next = child;
        }
        child->prev = iter->prev;
        child->next = iter->next;
        if (iter->next) {
            iter->next->prev = child;
        } else {
            /* last child */
            parent->child->prev = child;
        }
        child->parent = parent;

        /* isolate the node and free it */
        iter->next = NULL;
        iter->prev = iter;
        iter->parent = NULL;
        lllys_node_free(iter, NULL, 0);
    } else {
        if (shortcase) {
            /* create the implicit case to allow it to serve as a target of the augments,
             * it won't be printed, but it will be present in the tree */
            c = calloc(1, sizeof *c);
            LLLY_CHECK_ERR_RETURN(!c, LOGMEM(ctx), EXIT_FAILURE);
            c->name = lllydict_insert(module->ctx, child->name, 0);
            c->flags = LLLYS_IMPLICIT;
            if (!(options & (LLLYS_PARSE_OPT_CFG_IGNORE | LLLYS_PARSE_OPT_CFG_NOINHERIT))) {
                /* get config flag from parent */
                c->flags |= parent->flags & LLLYS_CONFIG_MASK;
            }
            c->module = module;
            c->nodetype = LLLYS_CASE;
            c->prev = (struct lllys_node*)c;
            lllys_node_addchild(parent, module, (struct lllys_node*)c, options);
            parent = (struct lllys_node*)c;
        }
        /* connect the child correctly */
        if (!parent) {
            if (module->data) {
                module->data->prev->next = child;
                child->prev = module->data->prev;
                module->data->prev = child;
            } else {
                module->data = child;
            }
        } else {
            pchild = lllys_child(parent, child->nodetype);
            assert(pchild);

            child->parent = parent;
            if (!(*pchild)) {
                /* the only/first child of the parent */
                *pchild = child;
                iter = child;
            } else {
                /* add a new child at the end of parent's child list */
                iter = (*pchild)->prev;
                iter->next = child;
                child->prev = iter;
            }
            while (iter->next) {
                iter = iter->next;
                iter->parent = parent;
            }
            (*pchild)->prev = iter;
        }
    }

    /* check config value (but ignore them in groupings and augments) */
    for (iter = parent; iter && !(iter->nodetype & (LLLYS_GROUPING | LLLYS_AUGMENT | LLLYS_EXT)); iter = iter->parent);
    if (parent && !iter) {
        for (iter = child; iter && !(iter->nodetype & (LLLYS_NOTIF | LLLYS_INPUT | LLLYS_OUTPUT | LLLYS_RPC)); iter = iter->parent);
        if (!iter && (parent->flags & LLLYS_CONFIG_R) && (child->flags & LLLYS_CONFIG_W)) {
            LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, child, "true", "config");
            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "State nodes cannot have configuration nodes as children.");
            return EXIT_FAILURE;
        }
    }

    /* propagate information about status data presence */
    if ((child->nodetype & (LLLYS_CONTAINER | LLLYS_CHOICE | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA)) &&
            (child->flags & LLLYS_INCL_STATUS)) {
        for(iter = parent; iter; iter = lllys_parent(iter)) {
            /* store it only into container or list - the only data inner nodes */
            if (iter->nodetype & (LLLYS_CONTAINER | LLLYS_LIST)) {
                if (iter->flags & LLLYS_INCL_STATUS) {
                    /* done, someone else set it already from here */
                    break;
                }
                /* set flag about including status data */
                iter->flags |= LLLYS_INCL_STATUS;
            }
        }
    }

    /* create implicit input/output nodes to have available them as possible target for augment */
    if ((child->nodetype & (LLLYS_RPC | LLLYS_ACTION)) && !child->child) {
        in = calloc(1, sizeof *in);
        out = calloc(1, sizeof *out);
        if (!in || !out) {
            LOGMEM(ctx);
            free(in);
            free(out);
            return EXIT_FAILURE;
        }
        in->nodetype = LLLYS_INPUT;
        in->name = lllydict_insert(child->module->ctx, "input", 5);
        out->nodetype = LLLYS_OUTPUT;
        out->name = lllydict_insert(child->module->ctx, "output", 6);
        in->module = out->module = child->module;
        in->parent = out->parent = child;
        in->flags = out->flags = LLLYS_IMPLICIT;
        in->next = (struct lllys_node *)out;
        in->prev = (struct lllys_node *)out;
        out->prev = (struct lllys_node *)in;
        child->child = (struct lllys_node *)in;
    }
    return EXIT_SUCCESS;
}

const struct lllys_module *
lllys_parse_mem_(struct llly_ctx *ctx, const char *data, LLLYS_INFORMAT format, const char *revision, int internal, int implement)
{
    char *enlarged_data = NULL;
    struct lllys_module *mod = NULL;
    unsigned int len;

    if (!ctx || !data) {
        LOGARG;
        return NULL;
    }

    if (!internal && format == LLLYS_IN_YANG) {
        /* enlarge data by 2 bytes for flex */
        len = strlen(data);
        enlarged_data = malloc((len + 2) * sizeof *enlarged_data);
        LLLY_CHECK_ERR_RETURN(!enlarged_data, LOGMEM(ctx), NULL);
        memcpy(enlarged_data, data, len);
        enlarged_data[len] = enlarged_data[len + 1] = '\0';
        data = enlarged_data;
    }

    switch (format) {
    case LLLYS_IN_YIN:
        mod = yin_read_module(ctx, data, revision, implement);
        break;
    case LLLYS_IN_YANG:
        mod = yang_read_module(ctx, data, 0, revision, implement);
        break;
    default:
        LOGERR(ctx, LLLY_EINVAL, "Invalid schema input format.");
        break;
    }

    free(enlarged_data);

    /* hack for NETCONF's edit-config's operation attribute. It is not defined in the schema, but since libyang
     * implements YANG metadata (annotations), we need its definition. Because the ietf-netconf schema is not the
     * internal part of libyang, we cannot add the annotation into the schema source, but we do it here to have
     * the anotation definitions available in the internal schema structure. There is another hack in schema
     * printers to do not print this internally added annotation. */
    if (mod && llly_strequal(mod->name, "ietf-netconf", 0)) {
        if (lllyp_add_ietf_netconf_annotations_config(mod)) {
            lllys_free(mod, NULL, 1, 1);
            return NULL;
        }
    }

    return mod;
}

API const struct lllys_module *
lllys_parse_mem(struct llly_ctx *ctx, const char *data, LLLYS_INFORMAT format)
{
    FUN_IN;

    return lllys_parse_mem_(ctx, data, format, NULL, 0, 1);
}

struct lllys_submodule *
lllys_sub_parse_mem(struct lllys_module *module, const char *data, LLLYS_INFORMAT format, struct unres_schema *unres)
{
    char *enlarged_data = NULL;
    struct lllys_submodule *submod = NULL;
    unsigned int len;

    assert(module);
    assert(data);

    if (format == LLLYS_IN_YANG) {
        /* enlarge data by 2 bytes for flex */
        len = strlen(data);
        enlarged_data = malloc((len + 2) * sizeof *enlarged_data);
        LLLY_CHECK_ERR_RETURN(!enlarged_data, LOGMEM(module->ctx), NULL);
        memcpy(enlarged_data, data, len);
        enlarged_data[len] = enlarged_data[len + 1] = '\0';
        data = enlarged_data;
    }

    /* get the main module */
    module = lllys_main_module(module);

    switch (format) {
    case LLLYS_IN_YIN:
        submod = yin_read_submodule(module, data, unres);
        break;
    case LLLYS_IN_YANG:
        submod = yang_read_submodule(module, data, 0, unres);
        break;
    default:
        assert(0);
        break;
    }

    free(enlarged_data);
    return submod;
}

API const struct lllys_module *
lllys_parse_path(struct llly_ctx *ctx, const char *path, LLLYS_INFORMAT format)
{
    FUN_IN;

    int fd;
    const struct lllys_module *ret;
    const char *rev, *dot, *filename;
    size_t len;

    if (!ctx || !path) {
        LOGARG;
        return NULL;
    }

    fd = open(path, O_RDONLY);
    if (fd == -1) {
        LOGERR(ctx, LLLY_ESYS, "Opening file \"%s\" failed (%s).", path, strerror(errno));
        return NULL;
    }

    ret = lllys_parse_fd(ctx, fd, format);
    close(fd);

    if (!ret) {
        /* error */
        return NULL;
    }

    /* check that name and revision match filename */
    filename = strrchr(path, '/');
    if (!filename) {
        filename = path;
    } else {
        filename++;
    }
    rev = strchr(filename, '@');
    dot = strrchr(filename, '.');

    /* name */
    len = strlen(ret->name);
    if (strncmp(filename, ret->name, len) ||
            ((rev && rev != &filename[len]) || (!rev && dot != &filename[len]))) {
        LOGWRN(ctx, "File name \"%s\" does not match module name \"%s\".", filename, ret->name);
    }
    if (rev) {
        len = dot - ++rev;
        if (!ret->rev_size || len != 10 || strncmp(ret->rev[0].date, rev, len)) {
            LOGWRN(ctx, "File name \"%s\" does not match module revision \"%s\".", filename,
                   ret->rev_size ? ret->rev[0].date : "none");
        }
    }

    if (!ret->filepath) {
        /* store URI */
        char rpath[PATH_MAX];
        if (realpath(path, rpath) != NULL) {
            ((struct lllys_module *)ret)->filepath = lllydict_insert(ctx, rpath, 0);
        } else {
            ((struct lllys_module *)ret)->filepath = lllydict_insert(ctx, path, 0);
        }
    }

    return ret;
}

API const struct lllys_module *
lllys_parse_fd(struct llly_ctx *ctx, int fd, LLLYS_INFORMAT format)
{
    FUN_IN;

    return lllys_parse_fd_(ctx, fd, format, NULL, 1);
}

static void
lllys_parse_set_filename(struct llly_ctx *ctx, const char **filename, int fd)
{
#ifdef __APPLE__
    char path[MAXPATHLEN];
#else
    int len;
    char path[PATH_MAX], proc_path[32];
#endif

#ifdef __APPLE__
    if (fcntl(fd, F_GETPATH, path) != -1) {
        *filename = lllydict_insert(ctx, path, 0);
    }
#else
    /* get URI if there is /proc */
    sprintf(proc_path, "/proc/self/fd/%d", fd);
    if ((len = readlink(proc_path, path, PATH_MAX - 1)) > 0) {
        *filename = lllydict_insert(ctx, path, len);
    }
#endif
}

const struct lllys_module *
lllys_parse_fd_(struct llly_ctx *ctx, int fd, LLLYS_INFORMAT format, const char *revision, int implement)
{
    const struct lllys_module *module;
    size_t length;
    char *addr;

    if (!ctx || fd < 0) {
        LOGARG;
        return NULL;
    }

    if (lllyp_mmap(ctx, fd, format == LLLYS_IN_YANG ? 1 : 0, &length, (void **)&addr)) {
        LOGERR(ctx, LLLY_ESYS, "Mapping file descriptor into memory failed (%s()).", __func__);
        return NULL;
    } else if (!addr) {
        LOGERR(ctx, LLLY_EINVAL, "Empty schema file.");
        return NULL;
    }

    module = lllys_parse_mem_(ctx, addr, format, revision, 1, implement);
    lllyp_munmap(addr, length);

    if (module && !module->filepath) {
        lllys_parse_set_filename(ctx, (const char **)&module->filepath, fd);
    }

    return module;
}

struct lllys_submodule *
lllys_sub_parse_fd(struct lllys_module *module, int fd, LLLYS_INFORMAT format, struct unres_schema *unres)
{
    struct lllys_submodule *submodule;
    size_t length;
    char *addr;

    assert(module);
    assert(fd >= 0);

    if (lllyp_mmap(module->ctx, fd, format == LLLYS_IN_YANG ? 1 : 0, &length, (void **)&addr)) {
        LOGERR(module->ctx, LLLY_ESYS, "Mapping file descriptor into memory failed (%s()).", __func__);
        return NULL;
    } else if (!addr) {
        LOGERR(module->ctx, LLLY_EINVAL, "Empty submodule schema file.");
        return NULL;
    }

    /* get the main module */
    module = lllys_main_module(module);

    switch (format) {
    case LLLYS_IN_YIN:
        submodule = yin_read_submodule(module, addr, unres);
        break;
    case LLLYS_IN_YANG:
        submodule = yang_read_submodule(module, addr, 0, unres);
        break;
    default:
        LOGINT(module->ctx);
        return NULL;
    }

    lllyp_munmap(addr, length);

    if (submodule && !submodule->filepath) {
        lllys_parse_set_filename(module->ctx, (const char **)&submodule->filepath, fd);
    }

    return submodule;

}

API int
lllys_search_localfile(const char * const *searchpaths, int cwd, const char *name, const char *revision, char **localfile, LLLYS_INFORMAT *format)
{
    FUN_IN;

    size_t len, flen, match_len = 0, dir_len;
    int i, implicit_cwd = 0, ret = EXIT_FAILURE;
    char *wd, *wn = NULL;
    DIR *dir = NULL;
    struct dirent *file;
    char *match_name = NULL;
    LLLYS_INFORMAT format_aux, match_format = 0;
    unsigned int u;
    struct llly_set *dirs;
    struct stat st;

    if (!localfile) {
        LOGARG;
        return EXIT_FAILURE;
    }

    /* start to fill the dir fifo with the context's search path (if set)
     * and the current working directory */
    dirs = llly_set_new();
    if (!dirs) {
        LOGMEM(NULL);
        return EXIT_FAILURE;
    }

    len = strlen(name);
    if (cwd) {
        wd = get_current_dir_name();
        if (!wd) {
            LOGMEM(NULL);
            goto cleanup;
        } else {
            /* add implicit current working directory (./) to be searched,
             * this directory is not searched recursively */
            if (llly_set_add(dirs, wd, 0) == -1) {
                goto cleanup;
            }
            implicit_cwd = 1;
        }
    }
    if (searchpaths) {
        for (i = 0; searchpaths[i]; i++) {
            /* check for duplicities with the implicit current working directory */
            if (implicit_cwd && !strcmp(dirs->set.g[0], searchpaths[i])) {
                implicit_cwd = 0;
                continue;
            }
            wd = strdup(searchpaths[i]);
            if (!wd) {
                LOGMEM(NULL);
                goto cleanup;
            } else if (llly_set_add(dirs, wd, 0) == -1) {
                goto cleanup;
            }
        }
    }
    wd = NULL;

    /* start searching */
    while (dirs->number) {
        free(wd);
        free(wn); wn = NULL;

        dirs->number--;
        wd = (char *)dirs->set.g[dirs->number];
        dirs->set.g[dirs->number] = NULL;
        LOGVRB("Searching for \"%s\" in %s.", name, wd);

        if (dir) {
            closedir(dir);
        }
        dir = opendir(wd);
        dir_len = strlen(wd);
        if (!dir) {
            LOGWRN(NULL, "Unable to open directory \"%s\" for searching (sub)modules (%s).", wd, strerror(errno));
        } else {
            while ((file = readdir(dir))) {
                if (!strcmp(".", file->d_name) || !strcmp("..", file->d_name)) {
                    /* skip . and .. */
                    continue;
                }
                free(wn);
                if (asprintf(&wn, "%s/%s", wd, file->d_name) == -1) {
                    LOGMEM(NULL);
                    goto cleanup;
                }
                if (stat(wn, &st) == -1) {
                    LOGWRN(NULL, "Unable to get information about \"%s\" file in \"%s\" when searching for (sub)modules (%s)",
                           file->d_name, wd, strerror(errno));
                    continue;
                }
                if (S_ISDIR(st.st_mode) && (dirs->number || !implicit_cwd)) {
                    /* we have another subdirectory in searchpath to explore,
                     * subdirectories are not taken into account in current working dir (dirs->set.g[0]) */
                    if (llly_set_add(dirs, wn, 0) == -1) {
                        goto cleanup;
                    }
                    /* continue with the next item in current directory */
                    wn = NULL;
                    continue;
                } else if (!S_ISREG(st.st_mode)) {
                    /* not a regular file (note that we see the target of symlinks instead of symlinks */
                    continue;
                }

                /* here we know that the item is a file which can contain a module */
                if (strncmp(name, file->d_name, len) ||
                        (file->d_name[len] != '.' && file->d_name[len] != '@')) {
                    /* different filename than the module we search for */
                    continue;
                }

                /* get type according to filename suffix */
                flen = strlen(file->d_name);
                if (!strcmp(&file->d_name[flen - 4], ".yin")) {
                    format_aux = LLLYS_IN_YIN;
                } else if (!strcmp(&file->d_name[flen - 5], ".yang")) {
                    format_aux = LLLYS_IN_YANG;
                } else {
                    /* not supportde suffix/file format */
                    continue;
                }

                if (revision) {
                    /* we look for the specific revision, try to get it from the filename */
                    if (file->d_name[len] == '@') {
                        /* check revision from the filename */
                        if (strncmp(revision, &file->d_name[len + 1], strlen(revision))) {
                            /* another revision */
                            continue;
                        } else {
                            /* exact revision */
                            free(match_name);
                            match_name = wn;
                            wn = NULL;
                            match_len = dir_len + 1 + len;
                            match_format = format_aux;
                            goto success;
                        }
                    } else {
                        /* continue trying to find exact revision match, use this only if not found */
                        free(match_name);
                        match_name = wn;
                        wn = NULL;
                        match_len = dir_len + 1 +len;
                        match_format = format_aux;
                        continue;
                    }
                } else {
                    /* remember the revision and try to find the newest one */
                    if (match_name) {
                        if (file->d_name[len] != '@' || lllyp_check_date(NULL, &file->d_name[len + 1])) {
                            continue;
                        } else if (match_name[match_len] == '@' &&
                                (strncmp(&match_name[match_len + 1], &file->d_name[len + 1], LLLY_REV_SIZE - 1) >= 0)) {
                            continue;
                        }
                        free(match_name);
                    }

                    match_name = wn;
                    wn = NULL;
                    match_len = dir_len + 1 + len;
                    match_format = format_aux;
                    continue;
                }
            }
        }
    }

success:
    (*localfile) = match_name;
    match_name = NULL;
    if (format) {
        (*format) = match_format;
    }
    ret = EXIT_SUCCESS;

cleanup:
    free(wn);
    free(wd);
    if (dir) {
        closedir(dir);
    }
    free(match_name);
    for (u = 0; u < dirs->number; u++) {
        free(dirs->set.g[u]);
    }
    llly_set_free(dirs);

    return ret;
}

int
lllys_ext_iter(struct lllys_ext_instance **ext, uint8_t ext_size, uint8_t start, LLLYEXT_SUBSTMT substmt)
{
    unsigned int u;

    for (u = start; u < ext_size; u++) {
        if (ext[u]->insubstmt == substmt) {
            return u;
        }
    }

    return -1;
}

/*
 * duplicate extension instance
 */
int
lllys_ext_dup(struct llly_ctx *ctx, struct lllys_module *mod, struct lllys_ext_instance **orig, uint8_t size, void *parent,
            LLLYEXT_PAR parent_type, struct lllys_ext_instance ***new, int shallow, struct unres_schema *unres)
{
    int i;
    uint8_t u = 0;
    struct lllys_ext_instance **result;
    struct unres_ext *info, *info_orig;
    size_t len;

    assert(new);

    if (!size) {
        if (orig) {
            LOGINT(ctx);
            return EXIT_FAILURE;
        }
        (*new) = NULL;
        return EXIT_SUCCESS;
    }

    (*new) = result = calloc(size, sizeof *result);
    LLLY_CHECK_ERR_RETURN(!result, LOGMEM(ctx), EXIT_FAILURE);
    for (u = 0; u < size; u++) {
        if (orig[u]) {
            /* resolved extension instance, just duplicate it */
            switch(orig[u]->ext_type) {
            case LLLYEXT_FLAG:
                result[u] = malloc(sizeof(struct lllys_ext_instance));
                LLLY_CHECK_ERR_GOTO(!result[u], LOGMEM(ctx), error);
                break;
            case LLLYEXT_COMPLEX:
                len = ((struct lllyext_plugin_complex*)orig[u]->def->plugin)->instance_size;
                result[u] = calloc(1, len);
                LLLY_CHECK_ERR_GOTO(!result[u], LOGMEM(ctx), error);

                ((struct lllys_ext_instance_complex*)result[u])->substmt = ((struct lllyext_plugin_complex*)orig[u]->def->plugin)->substmt;
                /* TODO duplicate data in extension instance content */
                memcpy((char *)result[u] + sizeof(**orig), (char *)orig[u] + sizeof(**orig), len - sizeof(**orig));
                break;
            }
            /* generic part */
            result[u]->def = orig[u]->def;
            result[u]->flags = LLLYEXT_OPT_CONTENT;
            result[u]->arg_value = lllydict_insert(ctx, orig[u]->arg_value, 0);
            result[u]->parent = parent;
            result[u]->parent_type = parent_type;
            result[u]->insubstmt = orig[u]->insubstmt;
            result[u]->insubstmt_index = orig[u]->insubstmt_index;
            result[u]->ext_type = orig[u]->ext_type;
            result[u]->priv = NULL;
            result[u]->nodetype = LLLYS_EXT;
            result[u]->module = mod;

            /* extensions */
            result[u]->ext_size = orig[u]->ext_size;
            if (lllys_ext_dup(ctx, mod, orig[u]->ext, orig[u]->ext_size, result[u],
                            LLLYEXT_PAR_EXTINST, &result[u]->ext, shallow, unres)) {
                goto error;
            }

            /* in case of shallow copy (duplication for deviation), duplicate only the link to private data
             * in a new copy, otherwise (grouping instantiation) do not duplicate the private data */
            if (shallow) {
                result[u]->priv = orig[u]->priv;
            }
        } else {
            /* original extension is not yet resolved, so duplicate it in unres */
            i = unres_schema_find(unres, -1, &orig, UNRES_EXT);
            if (i == -1) {
                /* extension not found in unres */
                LOGINT(ctx);
                goto error;
            }
            info_orig = unres->str_snode[i];
            info = malloc(sizeof *info);
            LLLY_CHECK_ERR_GOTO(!info, LOGMEM(ctx), error);
            info->datatype = info_orig->datatype;
            if (info->datatype == LLLYS_IN_YIN) {
                info->data.yin = lllyxml_dup_elem(ctx, info_orig->data.yin, NULL, 1, 0);
            } /* else TODO YANG */
            info->parent = parent;
            info->mod = mod;
            info->parent_type = parent_type;
            info->ext_index = u;
            if (unres_schema_add_node(info->mod, unres, new, UNRES_EXT, (struct lllys_node *)info) == -1) {
                goto error;
            }
        }
    }

    return EXIT_SUCCESS;

error:
    (*new) = NULL;
    lllys_extension_instances_free(ctx, result, u, NULL);
    return EXIT_FAILURE;
}

static struct lllys_restr *
lllys_restr_dup(struct lllys_module *mod, struct lllys_restr *old, int size, int shallow, struct unres_schema *unres)
{
    struct lllys_restr *result;
    int i;

    if (!size) {
        return NULL;
    }

    result = calloc(size, sizeof *result);
    LLLY_CHECK_ERR_RETURN(!result, LOGMEM(mod->ctx), NULL);

    for (i = 0; i < size; i++) {
        /* copying unresolved extensions is not supported */
        if (unres_schema_find(unres, -1, (void *)&old[i].ext, UNRES_EXT) == -1) {
            result[i].ext_size = old[i].ext_size;
            lllys_ext_dup(mod->ctx, mod, old[i].ext, old[i].ext_size, &result[i], LLLYEXT_PAR_RESTR, &result[i].ext, shallow, unres);
        }
        result[i].expr = lllydict_insert(mod->ctx, old[i].expr, 0);
        result[i].dsc = lllydict_insert(mod->ctx, old[i].dsc, 0);
        result[i].ref = lllydict_insert(mod->ctx, old[i].ref, 0);
        result[i].eapptag = lllydict_insert(mod->ctx, old[i].eapptag, 0);
        result[i].emsg = lllydict_insert(mod->ctx, old[i].emsg, 0);
    }

    return result;
}

void
lllys_restr_free(struct llly_ctx *ctx, struct lllys_restr *restr,
               void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    assert(ctx);
    if (!restr) {
        return;
    }

    lllys_extension_instances_free(ctx, restr->ext, restr->ext_size, private_destructor);
    lllydict_remove(ctx, restr->expr);
    lllydict_remove(ctx, restr->dsc);
    lllydict_remove(ctx, restr->ref);
    lllydict_remove(ctx, restr->eapptag);
    lllydict_remove(ctx, restr->emsg);
}

API void
lllys_iffeature_free(struct llly_ctx *ctx, struct lllys_iffeature *iffeature, uint8_t iffeature_size,
                   int shallow, void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    FUN_IN;

    uint8_t i;

    for (i = 0; i < iffeature_size; ++i) {
        lllys_extension_instances_free(ctx, iffeature[i].ext, iffeature[i].ext_size, private_destructor);
        if (!shallow) {
            free(iffeature[i].expr);
            free(iffeature[i].features);
        }
    }
    free(iffeature);
}

static int
type_dup(struct lllys_module *mod, struct lllys_node *parent, struct lllys_type *new, struct lllys_type *old,
         LLLY_DATA_TYPE base, int in_grp, int shallow, struct unres_schema *unres)
{
    int i;
    unsigned int u;

    switch (base) {
    case LLLY_TYPE_BINARY:
        if (old->info.binary.length) {
            new->info.binary.length = lllys_restr_dup(mod, old->info.binary.length, 1, shallow, unres);
        }
        break;

    case LLLY_TYPE_BITS:
        new->info.bits.count = old->info.bits.count;
        if (new->info.bits.count) {
            new->info.bits.bit = calloc(new->info.bits.count, sizeof *new->info.bits.bit);
            LLLY_CHECK_ERR_RETURN(!new->info.bits.bit, LOGMEM(mod->ctx), -1);

            for (u = 0; u < new->info.bits.count; u++) {
                new->info.bits.bit[u].name = lllydict_insert(mod->ctx, old->info.bits.bit[u].name, 0);
                new->info.bits.bit[u].dsc = lllydict_insert(mod->ctx, old->info.bits.bit[u].dsc, 0);
                new->info.bits.bit[u].ref = lllydict_insert(mod->ctx, old->info.bits.bit[u].ref, 0);
                new->info.bits.bit[u].flags = old->info.bits.bit[u].flags;
                new->info.bits.bit[u].pos = old->info.bits.bit[u].pos;
                new->info.bits.bit[u].ext_size = old->info.bits.bit[u].ext_size;
                if (lllys_ext_dup(mod->ctx, mod, old->info.bits.bit[u].ext, old->info.bits.bit[u].ext_size,
                                &new->info.bits.bit[u], LLLYEXT_PAR_TYPE_BIT,
                                &new->info.bits.bit[u].ext, shallow, unres)) {
                    return -1;
                }
            }
        }
        break;

    case LLLY_TYPE_DEC64:
        new->info.dec64.dig = old->info.dec64.dig;
        new->info.dec64.div = old->info.dec64.div;
        if (old->info.dec64.range) {
            new->info.dec64.range = lllys_restr_dup(mod, old->info.dec64.range, 1, shallow, unres);
        }
        break;

    case LLLY_TYPE_ENUM:
        new->info.enums.count = old->info.enums.count;
        if (new->info.enums.count) {
            new->info.enums.enm = calloc(new->info.enums.count, sizeof *new->info.enums.enm);
            LLLY_CHECK_ERR_RETURN(!new->info.enums.enm, LOGMEM(mod->ctx), -1);

            for (u = 0; u < new->info.enums.count; u++) {
                new->info.enums.enm[u].name = lllydict_insert(mod->ctx, old->info.enums.enm[u].name, 0);
                new->info.enums.enm[u].dsc = lllydict_insert(mod->ctx, old->info.enums.enm[u].dsc, 0);
                new->info.enums.enm[u].ref = lllydict_insert(mod->ctx, old->info.enums.enm[u].ref, 0);
                new->info.enums.enm[u].flags = old->info.enums.enm[u].flags;
                new->info.enums.enm[u].value = old->info.enums.enm[u].value;
                new->info.enums.enm[u].ext_size = old->info.enums.enm[u].ext_size;
                if (lllys_ext_dup(mod->ctx, mod, old->info.enums.enm[u].ext, old->info.enums.enm[u].ext_size,
                                &new->info.enums.enm[u], LLLYEXT_PAR_TYPE_ENUM,
                                &new->info.enums.enm[u].ext, shallow, unres)) {
                    return -1;
                }
            }
        }
        break;

    case LLLY_TYPE_IDENT:
        new->info.ident.count = old->info.ident.count;
        if (old->info.ident.count) {
            new->info.ident.ref = malloc(old->info.ident.count * sizeof *new->info.ident.ref);
            LLLY_CHECK_ERR_RETURN(!new->info.ident.ref, LOGMEM(mod->ctx), -1);
            memcpy(new->info.ident.ref, old->info.ident.ref, old->info.ident.count * sizeof *new->info.ident.ref);
        } else {
            /* there can be several unresolved base identities, duplicate them all */
            i = -1;
            do {
                i = unres_schema_find(unres, i, old, UNRES_TYPE_IDENTREF);
                if (i != -1) {
                    if (unres_schema_add_str(mod, unres, new, UNRES_TYPE_IDENTREF, unres->str_snode[i]) == -1) {
                        return -1;
                    }
                }
                --i;
            } while (i > -1);
        }
        break;

    case LLLY_TYPE_INST:
        new->info.inst.req = old->info.inst.req;
        break;

    case LLLY_TYPE_INT8:
    case LLLY_TYPE_INT16:
    case LLLY_TYPE_INT32:
    case LLLY_TYPE_INT64:
    case LLLY_TYPE_UINT8:
    case LLLY_TYPE_UINT16:
    case LLLY_TYPE_UINT32:
    case LLLY_TYPE_UINT64:
        if (old->info.num.range) {
            new->info.num.range = lllys_restr_dup(mod, old->info.num.range, 1, shallow, unres);
        }
        break;

    case LLLY_TYPE_LEAFREF:
        if (old->info.lref.path) {
            new->info.lref.path = lllydict_insert(mod->ctx, old->info.lref.path, 0);
            new->info.lref.req = old->info.lref.req;
            if (!in_grp && unres_schema_add_node(mod, unres, new, UNRES_TYPE_LEAFREF, parent) == -1) {
                return -1;
            }
        }
        break;

    case LLLY_TYPE_STRING:
        if (old->info.str.length) {
            new->info.str.length = lllys_restr_dup(mod, old->info.str.length, 1, shallow, unres);
        }
        if (old->info.str.pat_count) {
            new->info.str.patterns = lllys_restr_dup(mod, old->info.str.patterns, old->info.str.pat_count, shallow, unres);
            new->info.str.pat_count = old->info.str.pat_count;
#ifdef LLLY_ENABLED_CACHE
            if (!in_grp) {
                new->info.str.patterns_pcre = malloc(new->info.str.pat_count * 2 * sizeof *new->info.str.patterns_pcre);
                LLLY_CHECK_ERR_RETURN(!new->info.str.patterns_pcre, LOGMEM(mod->ctx), -1);
                for (u = 0; u < new->info.str.pat_count; u++) {
                    if (lllyp_precompile_pattern(mod->ctx, &new->info.str.patterns[u].expr[1],
                                              (pcre**)&new->info.str.patterns_pcre[2 * u],
                                              (pcre_extra**)&new->info.str.patterns_pcre[2 * u + 1])) {
                        free(new->info.str.patterns_pcre);
                        new->info.str.patterns_pcre = NULL;
                        return -1;
                    }
                }
            }
#endif
        }
        break;

    case LLLY_TYPE_UNION:
        new->info.uni.has_ptr_type = old->info.uni.has_ptr_type;
        new->info.uni.count = old->info.uni.count;
        if (new->info.uni.count) {
            new->info.uni.types = calloc(new->info.uni.count, sizeof *new->info.uni.types);
            LLLY_CHECK_ERR_RETURN(!new->info.uni.types, LOGMEM(mod->ctx), -1);

            for (u = 0; u < new->info.uni.count; u++) {
                if (lllys_type_dup(mod, parent, &(new->info.uni.types[u]), &(old->info.uni.types[u]), in_grp,
                        shallow, unres)) {
                    return -1;
                }
            }
        }
        break;

    default:
        /* nothing to do for LLLY_TYPE_BOOL, LLLY_TYPE_EMPTY */
        break;
    }

    return EXIT_SUCCESS;
}

struct yang_type *
lllys_yang_type_dup(struct lllys_module *module, struct lllys_node *parent, struct yang_type *old, struct lllys_type *type,
                  int in_grp, int shallow, struct unres_schema *unres)
{
    struct yang_type *new;

    new = calloc(1, sizeof *new);
    LLLY_CHECK_ERR_RETURN(!new, LOGMEM(module->ctx), NULL);
    new->flags = old->flags;
    new->base = old->base;
    new->name = lllydict_insert(module->ctx, old->name, 0);
    new->type = type;
    if (!new->name) {
        LOGMEM(module->ctx);
        goto error;
    }
    if (type_dup(module, parent, type, old->type, new->base, in_grp, shallow, unres)) {
        new->type->base = new->base;
        lllys_type_free(module->ctx, new->type, NULL);
        memset(&new->type->info, 0, sizeof new->type->info);
        goto error;
    }
    return new;

error:
    free(new);
    return NULL;
}

int
lllys_copy_union_leafrefs(struct lllys_module *mod, struct lllys_node *parent, struct lllys_type *type, struct lllys_type *prev_new,
                        struct unres_schema *unres)
{
    struct lllys_type new;
    unsigned int i, top_type;
    struct lllys_ext_instance **ext;
    uint8_t ext_size;
    void *reloc;

    if (!prev_new) {
        /* this is the "top-level" type, meaning it is a real type and no typedef directly above */
        top_type = 1;

        memset(&new, 0, sizeof new);

        new.base = type->base;
        new.parent = (struct lllys_tpdf *)parent;

        prev_new = &new;
    } else {
        /* this is not top-level type, just a type of a typedef */
        top_type = 0;
    }

    assert(type->der);
    if (type->der->module) {
        /* typedef, skip it, but keep the extensions */
        ext_size = type->ext_size;
        if (lllys_ext_dup(mod->ctx, mod, type->ext, type->ext_size, prev_new, LLLYEXT_PAR_TYPE, &ext, 0, unres)) {
            return -1;
        }
        if (prev_new->ext) {
            reloc = realloc(prev_new->ext, (prev_new->ext_size + ext_size) * sizeof *prev_new->ext);
            LLLY_CHECK_ERR_RETURN(!reloc, LOGMEM(mod->ctx), -1);
            prev_new->ext = reloc;

            memcpy(prev_new->ext + prev_new->ext_size, ext, ext_size * sizeof *ext);
            free(ext);

            prev_new->ext_size += ext_size;
        } else {
            prev_new->ext = ext;
            prev_new->ext_size = ext_size;
        }

        if (lllys_copy_union_leafrefs(mod, parent, &type->der->type, prev_new, unres)) {
            return -1;
        }
    } else {
        /* type, just make a deep copy */
        switch (type->base) {
        case LLLY_TYPE_UNION:
            prev_new->info.uni.has_ptr_type = type->info.uni.has_ptr_type;
            prev_new->info.uni.count = type->info.uni.count;
            /* this cannot be a typedef anymore */
            assert(prev_new->info.uni.count);

            prev_new->info.uni.types = calloc(prev_new->info.uni.count, sizeof *prev_new->info.uni.types);
            LLLY_CHECK_ERR_RETURN(!prev_new->info.uni.types, LOGMEM(mod->ctx), -1);

            for (i = 0; i < prev_new->info.uni.count; i++) {
                if (lllys_copy_union_leafrefs(mod, parent, &(type->info.uni.types[i]), &(prev_new->info.uni.types[i]), unres)) {
                    return -1;
                }
            }

            prev_new->der = type->der;
            break;
        default:
            if (lllys_type_dup(mod, parent, prev_new, type, 0, 0, unres)) {
                return -1;
            }
            break;
        }
    }

    if (top_type) {
        memcpy(type, prev_new, sizeof *type);
    }
    return EXIT_SUCCESS;
}

API const void *
lllys_ext_instance_substmt(const struct lllys_ext_instance *ext)
{
    FUN_IN;

    if (!ext) {
        return NULL;
    }

    switch (ext->insubstmt) {
    case LLLYEXT_SUBSTMT_SELF:
    case LLLYEXT_SUBSTMT_MODIFIER:
    case LLLYEXT_SUBSTMT_VERSION:
        return NULL;
    case LLLYEXT_SUBSTMT_ARGUMENT:
        if (ext->parent_type == LLLYEXT_PAR_EXT) {
            return ((struct lllys_ext_instance*)ext->parent)->arg_value;
        }
        break;
    case LLLYEXT_SUBSTMT_BASE:
        if (ext->parent_type == LLLYEXT_PAR_TYPE) {
            return ((struct lllys_type*)ext->parent)->info.ident.ref[ext->insubstmt_index];
        } else if (ext->parent_type == LLLYEXT_PAR_IDENT) {
            return ((struct lllys_ident*)ext->parent)->base[ext->insubstmt_index];
        }
        break;
    case LLLYEXT_SUBSTMT_BELONGSTO:
        if (ext->parent_type == LLLYEXT_PAR_MODULE && ((struct lllys_module*)ext->parent)->type) {
            return ((struct lllys_submodule*)ext->parent)->belongsto;
        }
        break;
    case LLLYEXT_SUBSTMT_CONFIG:
    case LLLYEXT_SUBSTMT_MANDATORY:
        if (ext->parent_type == LLLYEXT_PAR_NODE) {
            return &((struct lllys_node*)ext->parent)->flags;
        } else if (ext->parent_type == LLLYEXT_PAR_DEVIATE) {
            return &((struct lllys_deviate*)ext->parent)->flags;
        } else if (ext->parent_type == LLLYEXT_PAR_REFINE) {
            return &((struct lllys_refine*)ext->parent)->flags;
        }
        break;
    case LLLYEXT_SUBSTMT_CONTACT:
        if (ext->parent_type == LLLYEXT_PAR_MODULE) {
            return ((struct lllys_module*)ext->parent)->contact;
        }
        break;
    case LLLYEXT_SUBSTMT_DEFAULT:
        if (ext->parent_type == LLLYEXT_PAR_NODE) {
            switch (((struct lllys_node*)ext->parent)->nodetype) {
            case LLLYS_LEAF:
            case LLLYS_LEAFLIST:
                /* in case of leaf, the index is supposed to be 0, so it will return the
                 * correct pointer despite the leaf structure does not have dflt as array */
                return ((struct lllys_node_leaflist*)ext->parent)->dflt[ext->insubstmt_index];
            case LLLYS_CHOICE:
                return ((struct lllys_node_choice*)ext->parent)->dflt;
            default:
                /* internal error */
                break;
            }
        } else if (ext->parent_type == LLLYEXT_PAR_TPDF) {
            return ((struct lllys_tpdf*)ext->parent)->dflt;
        } else if (ext->parent_type == LLLYEXT_PAR_DEVIATE) {
            return ((struct lllys_deviate*)ext->parent)->dflt[ext->insubstmt_index];
        } else if (ext->parent_type == LLLYEXT_PAR_REFINE) {
            return &((struct lllys_refine*)ext->parent)->dflt[ext->insubstmt_index];
        }
        break;
    case LLLYEXT_SUBSTMT_DESCRIPTION:
        switch (ext->parent_type) {
        case LLLYEXT_PAR_NODE:
            return ((struct lllys_node*)ext->parent)->dsc;
        case LLLYEXT_PAR_MODULE:
            return ((struct lllys_module*)ext->parent)->dsc;
        case LLLYEXT_PAR_IMPORT:
            return ((struct lllys_import*)ext->parent)->dsc;
        case LLLYEXT_PAR_INCLUDE:
            return ((struct lllys_include*)ext->parent)->dsc;
        case LLLYEXT_PAR_EXT:
            return ((struct lllys_ext*)ext->parent)->dsc;
        case LLLYEXT_PAR_FEATURE:
            return ((struct lllys_feature*)ext->parent)->dsc;
        case LLLYEXT_PAR_TPDF:
            return ((struct lllys_tpdf*)ext->parent)->dsc;
        case LLLYEXT_PAR_TYPE_BIT:
            return ((struct lllys_type_bit*)ext->parent)->dsc;
        case LLLYEXT_PAR_TYPE_ENUM:
            return ((struct lllys_type_enum*)ext->parent)->dsc;
        case LLLYEXT_PAR_RESTR:
            return ((struct lllys_restr*)ext->parent)->dsc;
        case LLLYEXT_PAR_WHEN:
            return ((struct lllys_when*)ext->parent)->dsc;
        case LLLYEXT_PAR_IDENT:
            return ((struct lllys_ident*)ext->parent)->dsc;
        case LLLYEXT_PAR_DEVIATION:
            return ((struct lllys_deviation*)ext->parent)->dsc;
        case LLLYEXT_PAR_REVISION:
            return ((struct lllys_revision*)ext->parent)->dsc;
        case LLLYEXT_PAR_REFINE:
            return ((struct lllys_refine*)ext->parent)->dsc;
        default:
            break;
        }
        break;
    case LLLYEXT_SUBSTMT_ERRTAG:
        if (ext->parent_type == LLLYEXT_PAR_RESTR) {
            return ((struct lllys_restr*)ext->parent)->eapptag;
        }
        break;
    case LLLYEXT_SUBSTMT_ERRMSG:
        if (ext->parent_type == LLLYEXT_PAR_RESTR) {
            return ((struct lllys_restr*)ext->parent)->emsg;
        }
        break;
    case LLLYEXT_SUBSTMT_DIGITS:
        if (ext->parent_type == LLLYEXT_PAR_TYPE && ((struct lllys_type*)ext->parent)->base == LLLY_TYPE_DEC64) {
            return &((struct lllys_type*)ext->parent)->info.dec64.dig;
        }
        break;
    case LLLYEXT_SUBSTMT_KEY:
        if (ext->parent_type == LLLYEXT_PAR_NODE && ((struct lllys_node*)ext->parent)->nodetype == LLLYS_LIST) {
            return ((struct lllys_node_list*)ext->parent)->keys;
        }
        break;
    case LLLYEXT_SUBSTMT_MAX:
        if (ext->parent_type == LLLYEXT_PAR_NODE) {
            if (((struct lllys_node*)ext->parent)->nodetype == LLLYS_LIST) {
                return &((struct lllys_node_list*)ext->parent)->max;
            } else if (((struct lllys_node*)ext->parent)->nodetype == LLLYS_LEAFLIST) {
                return &((struct lllys_node_leaflist*)ext->parent)->max;
            }
        } else if (ext->parent_type == LLLYEXT_PAR_REFINE) {
            return &((struct lllys_refine*)ext->parent)->mod.list.max;
        }
        break;
    case LLLYEXT_SUBSTMT_MIN:
        if (ext->parent_type == LLLYEXT_PAR_NODE) {
            if (((struct lllys_node*)ext->parent)->nodetype == LLLYS_LIST) {
                return &((struct lllys_node_list*)ext->parent)->min;
            } else if (((struct lllys_node*)ext->parent)->nodetype == LLLYS_LEAFLIST) {
                return &((struct lllys_node_leaflist*)ext->parent)->min;
            }
        } else if (ext->parent_type == LLLYEXT_PAR_REFINE) {
            return &((struct lllys_refine*)ext->parent)->mod.list.min;
        }
        break;
    case LLLYEXT_SUBSTMT_NAMESPACE:
        if (ext->parent_type == LLLYEXT_PAR_MODULE && !((struct lllys_module*)ext->parent)->type) {
            return ((struct lllys_module*)ext->parent)->ns;
        }
        break;
    case LLLYEXT_SUBSTMT_ORDEREDBY:
        if (ext->parent_type == LLLYEXT_PAR_NODE &&
                (((struct lllys_node*)ext->parent)->nodetype & (LLLYS_LIST | LLLYS_LEAFLIST))) {
            return &((struct lllys_node_list*)ext->parent)->flags;
        }
        break;
    case LLLYEXT_SUBSTMT_ORGANIZATION:
        if (ext->parent_type == LLLYEXT_PAR_MODULE) {
            return ((struct lllys_module*)ext->parent)->org;
        }
        break;
    case LLLYEXT_SUBSTMT_PATH:
        if (ext->parent_type == LLLYEXT_PAR_TYPE && ((struct lllys_type*)ext->parent)->base == LLLY_TYPE_LEAFREF) {
            return ((struct lllys_type*)ext->parent)->info.lref.path;
        }
        break;
    case LLLYEXT_SUBSTMT_POSITION:
        if (ext->parent_type == LLLYEXT_PAR_TYPE_BIT) {
            return &((struct lllys_type_bit*)ext->parent)->pos;
        }
        break;
    case LLLYEXT_SUBSTMT_PREFIX:
        if (ext->parent_type == LLLYEXT_PAR_MODULE) {
            /* covers also lllys_submodule */
            return ((struct lllys_module*)ext->parent)->prefix;
        } else if (ext->parent_type == LLLYEXT_PAR_IMPORT) {
            return ((struct lllys_import*)ext->parent)->prefix;
        }
        break;
    case LLLYEXT_SUBSTMT_PRESENCE:
        if (ext->parent_type == LLLYEXT_PAR_NODE && ((struct lllys_node*)ext->parent)->nodetype == LLLYS_CONTAINER) {
            return ((struct lllys_node_container*)ext->parent)->presence;
        } else if (ext->parent_type == LLLYEXT_PAR_REFINE) {
            return ((struct lllys_refine*)ext->parent)->mod.presence;
        }
        break;
    case LLLYEXT_SUBSTMT_REFERENCE:
        switch (ext->parent_type) {
        case LLLYEXT_PAR_NODE:
            return ((struct lllys_node*)ext->parent)->ref;
        case LLLYEXT_PAR_MODULE:
            return ((struct lllys_module*)ext->parent)->ref;
        case LLLYEXT_PAR_IMPORT:
            return ((struct lllys_import*)ext->parent)->ref;
        case LLLYEXT_PAR_INCLUDE:
            return ((struct lllys_include*)ext->parent)->ref;
        case LLLYEXT_PAR_EXT:
            return ((struct lllys_ext*)ext->parent)->ref;
        case LLLYEXT_PAR_FEATURE:
            return ((struct lllys_feature*)ext->parent)->ref;
        case LLLYEXT_PAR_TPDF:
            return ((struct lllys_tpdf*)ext->parent)->ref;
        case LLLYEXT_PAR_TYPE_BIT:
            return ((struct lllys_type_bit*)ext->parent)->ref;
        case LLLYEXT_PAR_TYPE_ENUM:
            return ((struct lllys_type_enum*)ext->parent)->ref;
        case LLLYEXT_PAR_RESTR:
            return ((struct lllys_restr*)ext->parent)->ref;
        case LLLYEXT_PAR_WHEN:
            return ((struct lllys_when*)ext->parent)->ref;
        case LLLYEXT_PAR_IDENT:
            return ((struct lllys_ident*)ext->parent)->ref;
        case LLLYEXT_PAR_DEVIATION:
            return ((struct lllys_deviation*)ext->parent)->ref;
        case LLLYEXT_PAR_REVISION:
            return ((struct lllys_revision*)ext->parent)->ref;
        case LLLYEXT_PAR_REFINE:
            return ((struct lllys_refine*)ext->parent)->ref;
        default:
            break;
        }
        break;
    case LLLYEXT_SUBSTMT_REQINSTANCE:
        if (ext->parent_type == LLLYEXT_PAR_TYPE) {
            if (((struct lllys_type*)ext->parent)->base == LLLY_TYPE_LEAFREF) {
                return &((struct lllys_type*)ext->parent)->info.lref.req;
            } else if (((struct lllys_type*)ext->parent)->base == LLLY_TYPE_INST) {
                return &((struct lllys_type*)ext->parent)->info.inst.req;
            }
        }
        break;
    case LLLYEXT_SUBSTMT_REVISIONDATE:
        if (ext->parent_type == LLLYEXT_PAR_IMPORT) {
            return ((struct lllys_import*)ext->parent)->rev;
        } else if (ext->parent_type == LLLYEXT_PAR_INCLUDE) {
            return ((struct lllys_include*)ext->parent)->rev;
        }
        break;
    case LLLYEXT_SUBSTMT_STATUS:
        switch (ext->parent_type) {
        case LLLYEXT_PAR_NODE:
        case LLLYEXT_PAR_IDENT:
        case LLLYEXT_PAR_TPDF:
        case LLLYEXT_PAR_EXT:
        case LLLYEXT_PAR_FEATURE:
        case LLLYEXT_PAR_TYPE_ENUM:
        case LLLYEXT_PAR_TYPE_BIT:
            /* in all structures the flags member is at the same offset */
            return &((struct lllys_node*)ext->parent)->flags;
        default:
            break;
        }
        break;
    case LLLYEXT_SUBSTMT_UNIQUE:
        if (ext->parent_type == LLLYEXT_PAR_DEVIATE) {
            return &((struct lllys_deviate*)ext->parent)->unique[ext->insubstmt_index];
        } else if (ext->parent_type == LLLYEXT_PAR_NODE && ((struct lllys_node*)ext->parent)->nodetype == LLLYS_LIST) {
            return &((struct lllys_node_list*)ext->parent)->unique[ext->insubstmt_index];
        }
        break;
    case LLLYEXT_SUBSTMT_UNITS:
        if (ext->parent_type == LLLYEXT_PAR_NODE &&
                (((struct lllys_node*)ext->parent)->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
            /* units is at the same offset in both lllys_node_leaf and lllys_node_leaflist */
            return ((struct lllys_node_leaf*)ext->parent)->units;
        } else if (ext->parent_type == LLLYEXT_PAR_TPDF) {
            return ((struct lllys_tpdf*)ext->parent)->units;
        } else if (ext->parent_type == LLLYEXT_PAR_DEVIATE) {
            return ((struct lllys_deviate*)ext->parent)->units;
        }
        break;
    case LLLYEXT_SUBSTMT_VALUE:
        if (ext->parent_type == LLLYEXT_PAR_TYPE_ENUM) {
            return &((struct lllys_type_enum*)ext->parent)->value;
        }
        break;
    case LLLYEXT_SUBSTMT_YINELEM:
        if (ext->parent_type == LLLYEXT_PAR_EXT) {
            return &((struct lllys_ext*)ext->parent)->flags;
        }
        break;
    }
    LOGINT(ext->module->ctx);
    return NULL;
}

static int
lllys_type_dup(struct lllys_module *mod, struct lllys_node *parent, struct lllys_type *new, struct lllys_type *old,
            int in_grp, int shallow, struct unres_schema *unres)
{
    int i;

    new->base = old->base;
    new->der = old->der;
    new->parent = (struct lllys_tpdf *)parent;
    new->ext_size = old->ext_size;
    if (lllys_ext_dup(mod->ctx, mod, old->ext, old->ext_size, new, LLLYEXT_PAR_TYPE, &new->ext, shallow, unres)) {
        return -1;
    }

    i = unres_schema_find(unres, -1, old, UNRES_TYPE_DER);
    if (i != -1) {
        /* HACK (serious one) for unres */
        /* nothing else we can do but duplicate it immediately */
        if (((struct lllyxml_elem *)old->der)->flags & LLLY_YANG_STRUCTURE_FLAG) {
            new->der = (struct lllys_tpdf *)lllys_yang_type_dup(mod, parent, (struct yang_type *)old->der, new, in_grp,
                                                            shallow, unres);
        } else {
            new->der = (struct lllys_tpdf *)lllyxml_dup_elem(mod->ctx, (struct lllyxml_elem *)old->der, NULL, 1, 0);
        }
        /* all these unres additions can fail even though they did not before */
        if (!new->der || (unres_schema_add_node(mod, unres, new, UNRES_TYPE_DER, parent) == -1)) {
            return -1;
        }
        return EXIT_SUCCESS;
    }

    return type_dup(mod, parent, new, old, new->base, in_grp, shallow, unres);
}

void
lllys_type_free(struct llly_ctx *ctx, struct lllys_type *type,
              void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    unsigned int i;

    assert(ctx);
    if (!type) {
        return;
    }

    lllys_extension_instances_free(ctx, type->ext, type->ext_size, private_destructor);

    switch (type->base) {
    case LLLY_TYPE_BINARY:
        lllys_restr_free(ctx, type->info.binary.length, private_destructor);
        free(type->info.binary.length);
        break;
    case LLLY_TYPE_BITS:
        for (i = 0; i < type->info.bits.count; i++) {
            lllydict_remove(ctx, type->info.bits.bit[i].name);
            lllydict_remove(ctx, type->info.bits.bit[i].dsc);
            lllydict_remove(ctx, type->info.bits.bit[i].ref);
            lllys_iffeature_free(ctx, type->info.bits.bit[i].iffeature, type->info.bits.bit[i].iffeature_size, 0,
                               private_destructor);
            lllys_extension_instances_free(ctx, type->info.bits.bit[i].ext, type->info.bits.bit[i].ext_size,
                                         private_destructor);
        }
        free(type->info.bits.bit);
        break;

    case LLLY_TYPE_DEC64:
        lllys_restr_free(ctx, type->info.dec64.range, private_destructor);
        free(type->info.dec64.range);
        break;

    case LLLY_TYPE_ENUM:
        for (i = 0; i < type->info.enums.count; i++) {
            lllydict_remove(ctx, type->info.enums.enm[i].name);
            lllydict_remove(ctx, type->info.enums.enm[i].dsc);
            lllydict_remove(ctx, type->info.enums.enm[i].ref);
            lllys_iffeature_free(ctx, type->info.enums.enm[i].iffeature, type->info.enums.enm[i].iffeature_size, 0,
                               private_destructor);
            lllys_extension_instances_free(ctx, type->info.enums.enm[i].ext, type->info.enums.enm[i].ext_size,
                                         private_destructor);
        }
        free(type->info.enums.enm);
        break;

    case LLLY_TYPE_INT8:
    case LLLY_TYPE_INT16:
    case LLLY_TYPE_INT32:
    case LLLY_TYPE_INT64:
    case LLLY_TYPE_UINT8:
    case LLLY_TYPE_UINT16:
    case LLLY_TYPE_UINT32:
    case LLLY_TYPE_UINT64:
        lllys_restr_free(ctx, type->info.num.range, private_destructor);
        free(type->info.num.range);
        break;

    case LLLY_TYPE_LEAFREF:
        lllydict_remove(ctx, type->info.lref.path);
        break;

    case LLLY_TYPE_STRING:
        lllys_restr_free(ctx, type->info.str.length, private_destructor);
        free(type->info.str.length);
        for (i = 0; i < type->info.str.pat_count; i++) {
            lllys_restr_free(ctx, &type->info.str.patterns[i], private_destructor);
#ifdef LLLY_ENABLED_CACHE
            if (type->info.str.patterns_pcre) {
                pcre_free((pcre*)type->info.str.patterns_pcre[2 * i]);
                pcre_free_study((pcre_extra*)type->info.str.patterns_pcre[2 * i + 1]);
            }
#endif
        }
        free(type->info.str.patterns);
#ifdef LLLY_ENABLED_CACHE
        free(type->info.str.patterns_pcre);
#endif
        break;

    case LLLY_TYPE_UNION:
        for (i = 0; i < type->info.uni.count; i++) {
            lllys_type_free(ctx, &type->info.uni.types[i], private_destructor);
        }
        free(type->info.uni.types);
        break;

    case LLLY_TYPE_IDENT:
        free(type->info.ident.ref);
        break;

    default:
        /* nothing to do for LLLY_TYPE_INST, LLLY_TYPE_BOOL, LLLY_TYPE_EMPTY */
        break;
    }
}

static void
lllys_tpdf_free(struct llly_ctx *ctx, struct lllys_tpdf *tpdf,
              void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    assert(ctx);
    if (!tpdf) {
        return;
    }

    lllydict_remove(ctx, tpdf->name);
    lllydict_remove(ctx, tpdf->dsc);
    lllydict_remove(ctx, tpdf->ref);

    lllys_type_free(ctx, &tpdf->type, private_destructor);

    lllydict_remove(ctx, tpdf->units);
    lllydict_remove(ctx, tpdf->dflt);

    lllys_extension_instances_free(ctx, tpdf->ext, tpdf->ext_size, private_destructor);
}

static struct lllys_when *
lllys_when_dup(struct lllys_module *mod, struct lllys_when *old, int shallow, struct unres_schema *unres)
{
    struct lllys_when *new;

    if (!old) {
        return NULL;
    }

    new = calloc(1, sizeof *new);
    LLLY_CHECK_ERR_RETURN(!new, LOGMEM(mod->ctx), NULL);
    new->cond = lllydict_insert(mod->ctx, old->cond, 0);
    new->dsc = lllydict_insert(mod->ctx, old->dsc, 0);
    new->ref = lllydict_insert(mod->ctx, old->ref, 0);
    new->ext_size = old->ext_size;
    lllys_ext_dup(mod->ctx, mod, old->ext, old->ext_size, new, LLLYEXT_PAR_WHEN, &new->ext, shallow, unres);

    return new;
}

void
lllys_when_free(struct llly_ctx *ctx, struct lllys_when *w,
              void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    if (!w) {
        return;
    }

    lllys_extension_instances_free(ctx, w->ext, w->ext_size, private_destructor);
    lllydict_remove(ctx, w->cond);
    lllydict_remove(ctx, w->dsc);
    lllydict_remove(ctx, w->ref);

    free(w);
}

static void
lllys_augment_free(struct llly_ctx *ctx, struct lllys_node_augment *aug,
                 void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    struct lllys_node *next, *sub;

    /* children from a resolved augment are freed under the target node */
    if (!aug->target || (aug->flags & LLLYS_NOTAPPLIED)) {
        LLLY_TREE_FOR_SAFE(aug->child, next, sub) {
            lllys_node_free(sub, private_destructor, 0);
        }
    }

    lllydict_remove(ctx, aug->target_name);
    lllydict_remove(ctx, aug->dsc);
    lllydict_remove(ctx, aug->ref);

    lllys_iffeature_free(ctx, aug->iffeature, aug->iffeature_size, 0, private_destructor);
    lllys_extension_instances_free(ctx, aug->ext, aug->ext_size, private_destructor);

    lllys_when_free(ctx, aug->when, private_destructor);
}

static void
lllys_ident_free(struct llly_ctx *ctx, struct lllys_ident *ident,
               void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    assert(ctx);
    if (!ident) {
        return;
    }

    free(ident->base);
    llly_set_free(ident->der);
    lllydict_remove(ctx, ident->name);
    lllydict_remove(ctx, ident->dsc);
    lllydict_remove(ctx, ident->ref);
    lllys_iffeature_free(ctx, ident->iffeature, ident->iffeature_size, 0, private_destructor);
    lllys_extension_instances_free(ctx, ident->ext, ident->ext_size, private_destructor);

}

static void
lllys_grp_free(struct llly_ctx *ctx, struct lllys_node_grp *grp,
             void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    int i;

    /* handle only specific parts for LLLYS_GROUPING */
    for (i = 0; i < grp->tpdf_size; i++) {
        lllys_tpdf_free(ctx, &grp->tpdf[i], private_destructor);
    }
    free(grp->tpdf);
}

static void
lllys_rpc_action_free(struct llly_ctx *ctx, struct lllys_node_rpc_action *rpc_act,
             void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    int i;

    /* handle only specific parts for LLLYS_GROUPING */
    for (i = 0; i < rpc_act->tpdf_size; i++) {
        lllys_tpdf_free(ctx, &rpc_act->tpdf[i], private_destructor);
    }
    free(rpc_act->tpdf);
}

static void
lllys_inout_free(struct llly_ctx *ctx, struct lllys_node_inout *io,
               void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    int i;

    /* handle only specific parts for LLLYS_INPUT and LLLYS_OUTPUT */
    for (i = 0; i < io->tpdf_size; i++) {
        lllys_tpdf_free(ctx, &io->tpdf[i], private_destructor);
    }
    free(io->tpdf);

    for (i = 0; i < io->must_size; i++) {
        lllys_restr_free(ctx, &io->must[i], private_destructor);
    }
    free(io->must);
}

static void
lllys_notif_free(struct llly_ctx *ctx, struct lllys_node_notif *notif,
               void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    int i;

    for (i = 0; i < notif->must_size; i++) {
        lllys_restr_free(ctx, &notif->must[i], private_destructor);
    }
    free(notif->must);

    for (i = 0; i < notif->tpdf_size; i++) {
        lllys_tpdf_free(ctx, &notif->tpdf[i], private_destructor);
    }
    free(notif->tpdf);
}
static void
lllys_anydata_free(struct llly_ctx *ctx, struct lllys_node_anydata *anyxml,
                 void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    int i;

    for (i = 0; i < anyxml->must_size; i++) {
        lllys_restr_free(ctx, &anyxml->must[i], private_destructor);
    }
    free(anyxml->must);

    lllys_when_free(ctx, anyxml->when, private_destructor);
}

static void
lllys_leaf_free(struct llly_ctx *ctx, struct lllys_node_leaf *leaf,
              void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    int i;

    /* leafref backlinks */
    llly_set_free((struct llly_set *)leaf->backlinks);

    for (i = 0; i < leaf->must_size; i++) {
        lllys_restr_free(ctx, &leaf->must[i], private_destructor);
    }
    free(leaf->must);

    lllys_when_free(ctx, leaf->when, private_destructor);

    lllys_type_free(ctx, &leaf->type, private_destructor);
    lllydict_remove(ctx, leaf->units);
    lllydict_remove(ctx, leaf->dflt);
}

static void
lllys_leaflist_free(struct llly_ctx *ctx, struct lllys_node_leaflist *llist,
                  void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    int i;

    if (llist->backlinks) {
        /* leafref backlinks */
        llly_set_free(llist->backlinks);
    }

    for (i = 0; i < llist->must_size; i++) {
        lllys_restr_free(ctx, &llist->must[i], private_destructor);
    }
    free(llist->must);

    for (i = 0; i < llist->dflt_size; i++) {
        lllydict_remove(ctx, llist->dflt[i]);
    }
    free(llist->dflt);

    lllys_when_free(ctx, llist->when, private_destructor);

    lllys_type_free(ctx, &llist->type, private_destructor);
    lllydict_remove(ctx, llist->units);
}

static void
lllys_list_free(struct llly_ctx *ctx, struct lllys_node_list *list,
              void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    int i, j;

    /* handle only specific parts for LLLY_NODE_LIST */
    lllys_when_free(ctx, list->when, private_destructor);

    for (i = 0; i < list->must_size; i++) {
        lllys_restr_free(ctx, &list->must[i], private_destructor);
    }
    free(list->must);

    for (i = 0; i < list->tpdf_size; i++) {
        lllys_tpdf_free(ctx, &list->tpdf[i], private_destructor);
    }
    free(list->tpdf);

    free(list->keys);

    for (i = 0; i < list->unique_size; i++) {
        for (j = 0; j < list->unique[i].expr_size; j++) {
            lllydict_remove(ctx, list->unique[i].expr[j]);
        }
        free(list->unique[i].expr);
    }
    free(list->unique);

    lllydict_remove(ctx, list->keys_str);
}

static void
lllys_container_free(struct llly_ctx *ctx, struct lllys_node_container *cont,
                   void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    int i;

    /* handle only specific parts for LLLY_NODE_CONTAINER */
    lllydict_remove(ctx, cont->presence);

    for (i = 0; i < cont->tpdf_size; i++) {
        lllys_tpdf_free(ctx, &cont->tpdf[i], private_destructor);
    }
    free(cont->tpdf);

    for (i = 0; i < cont->must_size; i++) {
        lllys_restr_free(ctx, &cont->must[i], private_destructor);
    }
    free(cont->must);

    lllys_when_free(ctx, cont->when, private_destructor);
}

static void
lllys_feature_free(struct llly_ctx *ctx, struct lllys_feature *f,
                 void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    lllydict_remove(ctx, f->name);
    lllydict_remove(ctx, f->dsc);
    lllydict_remove(ctx, f->ref);
    lllys_iffeature_free(ctx, f->iffeature, f->iffeature_size, 0, private_destructor);
    llly_set_free(f->depfeatures);
    lllys_extension_instances_free(ctx, f->ext, f->ext_size, private_destructor);
}

static void
lllys_extension_free(struct llly_ctx *ctx, struct lllys_ext *e,
                   void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    lllydict_remove(ctx, e->name);
    lllydict_remove(ctx, e->dsc);
    lllydict_remove(ctx, e->ref);
    lllydict_remove(ctx, e->argument);
    lllys_extension_instances_free(ctx, e->ext, e->ext_size, private_destructor);
}

static void
lllys_deviation_free(struct lllys_module *module, struct lllys_deviation *dev,
                   void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    int i, j, k;
    struct llly_ctx *ctx;
    struct lllys_node *next, *elem;

    ctx = module->ctx;

    lllydict_remove(ctx, dev->target_name);
    lllydict_remove(ctx, dev->dsc);
    lllydict_remove(ctx, dev->ref);
    lllys_extension_instances_free(ctx, dev->ext, dev->ext_size, private_destructor);

    if (!dev->deviate) {
        return;
    }

    /* it could not be applied because it failed to be applied */
    if (dev->orig_node) {
        /* the module was freed, but we only need the context from orig_node, use ours */
        if (dev->deviate[0].mod == LLLY_DEVIATE_NO) {
            /* it's actually a node subtree, we need to update modules on all the nodes :-/ */
            LLLY_TREE_DFS_BEGIN(dev->orig_node, next, elem) {
                elem->module = module;

                LLLY_TREE_DFS_END(dev->orig_node, next, elem);
            }
            lllys_node_free(dev->orig_node, NULL, 0);
        } else {
            /* it's just a shallow copy, freeing one node */
            dev->orig_node->module = module;
            lllys_node_free(dev->orig_node, NULL, 1);
        }
    }

    for (i = 0; i < dev->deviate_size; i++) {
        lllys_extension_instances_free(ctx, dev->deviate[i].ext, dev->deviate[i].ext_size, private_destructor);

        for (j = 0; j < dev->deviate[i].dflt_size; j++) {
            lllydict_remove(ctx, dev->deviate[i].dflt[j]);
        }
        free(dev->deviate[i].dflt);

        lllydict_remove(ctx, dev->deviate[i].units);

        if (dev->deviate[i].mod == LLLY_DEVIATE_DEL) {
            for (j = 0; j < dev->deviate[i].must_size; j++) {
                lllys_restr_free(ctx, &dev->deviate[i].must[j], private_destructor);
            }
            free(dev->deviate[i].must);

            for (j = 0; j < dev->deviate[i].unique_size; j++) {
                for (k = 0; k < dev->deviate[i].unique[j].expr_size; k++) {
                    lllydict_remove(ctx, dev->deviate[i].unique[j].expr[k]);
                }
                free(dev->deviate[i].unique[j].expr);
            }
            free(dev->deviate[i].unique);
        }
    }
    free(dev->deviate);
}

static void
lllys_uses_free(struct llly_ctx *ctx, struct lllys_node_uses *uses,
              void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    int i, j;

    for (i = 0; i < uses->refine_size; i++) {
        lllydict_remove(ctx, uses->refine[i].target_name);
        lllydict_remove(ctx, uses->refine[i].dsc);
        lllydict_remove(ctx, uses->refine[i].ref);

        lllys_iffeature_free(ctx, uses->refine[i].iffeature, uses->refine[i].iffeature_size, 0, private_destructor);

        for (j = 0; j < uses->refine[i].must_size; j++) {
            lllys_restr_free(ctx, &uses->refine[i].must[j], private_destructor);
        }
        free(uses->refine[i].must);

        for (j = 0; j < uses->refine[i].dflt_size; j++) {
            lllydict_remove(ctx, uses->refine[i].dflt[j]);
        }
        free(uses->refine[i].dflt);

        lllys_extension_instances_free(ctx, uses->refine[i].ext, uses->refine[i].ext_size, private_destructor);

        if (uses->refine[i].target_type & LLLYS_CONTAINER) {
            lllydict_remove(ctx, uses->refine[i].mod.presence);
        }
    }
    free(uses->refine);

    for (i = 0; i < uses->augment_size; i++) {
        lllys_augment_free(ctx, &uses->augment[i], private_destructor);
    }
    free(uses->augment);

    lllys_when_free(ctx, uses->when, private_destructor);
}

void
lllys_node_free(struct lllys_node *node, void (*private_destructor)(const struct lllys_node *node, void *priv), int shallow)
{
    struct llly_ctx *ctx;
    struct lllys_node *sub, *next;

    if (!node) {
        return;
    }

    assert(node->module);
    assert(node->module->ctx);

    ctx = node->module->ctx;

    /* remove private object */
    if (node->priv && private_destructor) {
        private_destructor(node, node->priv);
    }

    /* common part */
    lllydict_remove(ctx, node->name);
    if (!(node->nodetype & (LLLYS_INPUT | LLLYS_OUTPUT))) {
        lllys_iffeature_free(ctx, node->iffeature, node->iffeature_size, shallow, private_destructor);
        lllydict_remove(ctx, node->dsc);
        lllydict_remove(ctx, node->ref);
    }

    if (!shallow && !(node->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
        LLLY_TREE_FOR_SAFE(node->child, next, sub) {
            lllys_node_free(sub, private_destructor, 0);
        }
    }

    lllys_extension_instances_free(ctx, node->ext, node->ext_size, private_destructor);

    /* specific part */
    switch (node->nodetype) {
    case LLLYS_CONTAINER:
        lllys_container_free(ctx, (struct lllys_node_container *)node, private_destructor);
        break;
    case LLLYS_CHOICE:
        lllys_when_free(ctx, ((struct lllys_node_choice *)node)->when, private_destructor);
        break;
    case LLLYS_LEAF:
        lllys_leaf_free(ctx, (struct lllys_node_leaf *)node, private_destructor);
        break;
    case LLLYS_LEAFLIST:
        lllys_leaflist_free(ctx, (struct lllys_node_leaflist *)node, private_destructor);
        break;
    case LLLYS_LIST:
        lllys_list_free(ctx, (struct lllys_node_list *)node, private_destructor);
        break;
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        lllys_anydata_free(ctx, (struct lllys_node_anydata *)node, private_destructor);
        break;
    case LLLYS_USES:
        lllys_uses_free(ctx, (struct lllys_node_uses *)node, private_destructor);
        break;
    case LLLYS_CASE:
        lllys_when_free(ctx, ((struct lllys_node_case *)node)->when, private_destructor);
        break;
    case LLLYS_AUGMENT:
        /* do nothing */
        break;
    case LLLYS_GROUPING:
        lllys_grp_free(ctx, (struct lllys_node_grp *)node, private_destructor);
        break;
    case LLLYS_RPC:
    case LLLYS_ACTION:
        lllys_rpc_action_free(ctx, (struct lllys_node_rpc_action *)node, private_destructor);
        break;
    case LLLYS_NOTIF:
        lllys_notif_free(ctx, (struct lllys_node_notif *)node, private_destructor);
        break;
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
        lllys_inout_free(ctx, (struct lllys_node_inout *)node, private_destructor);
        break;
    case LLLYS_EXT:
    case LLLYS_UNKNOWN:
        LOGINT(ctx);
        break;
    }

    /* again common part */
    lllys_node_unlink(node);
    free(node);
}

API struct lllys_module *
lllys_implemented_module(const struct lllys_module *mod)
{
    FUN_IN;

    struct llly_ctx *ctx;
    int i;

    if (!mod || mod->implemented) {
        /* invalid argument or the module itself is implemented */
        return (struct lllys_module *)mod;
    }

    ctx = mod->ctx;
    for (i = 0; i < ctx->models.used; i++) {
        if (!ctx->models.list[i]->implemented) {
            continue;
        }

        if (llly_strequal(mod->name, ctx->models.list[i]->name, 1)) {
            /* we have some revision of the module implemented */
            return ctx->models.list[i];
        }
    }

    /* we have no revision of the module implemented, return the module itself,
     * it is up to the caller to set the module implemented when needed */
    return (struct lllys_module *)mod;
}

/* free_int_mods - flag whether to free the internal modules as well */
static void
module_free_common(struct lllys_module *module, void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    struct llly_ctx *ctx;
    struct lllys_node *next, *iter;
    unsigned int i;

    assert(module->ctx);
    ctx = module->ctx;

    /* just free the import array, imported modules will stay in the context */
    for (i = 0; i < module->imp_size; i++) {
        lllydict_remove(ctx, module->imp[i].prefix);
        lllydict_remove(ctx, module->imp[i].dsc);
        lllydict_remove(ctx, module->imp[i].ref);
        lllys_extension_instances_free(ctx, module->imp[i].ext, module->imp[i].ext_size, private_destructor);
    }
    free(module->imp);

    /* submodules don't have data tree, the data nodes
     * are placed in the main module altogether */
    if (!module->type) {
        LLLY_TREE_FOR_SAFE(module->data, next, iter) {
            lllys_node_free(iter, private_destructor, 0);
        }
    }

    lllydict_remove(ctx, module->dsc);
    lllydict_remove(ctx, module->ref);
    lllydict_remove(ctx, module->org);
    lllydict_remove(ctx, module->contact);
    lllydict_remove(ctx, module->filepath);

    /* revisions */
    for (i = 0; i < module->rev_size; i++) {
        lllys_extension_instances_free(ctx, module->rev[i].ext, module->rev[i].ext_size, private_destructor);
        lllydict_remove(ctx, module->rev[i].dsc);
        lllydict_remove(ctx, module->rev[i].ref);
    }
    free(module->rev);

    /* identities */
    for (i = 0; i < module->ident_size; i++) {
        lllys_ident_free(ctx, &module->ident[i], private_destructor);
    }
    module->ident_size = 0;
    free(module->ident);

    /* typedefs */
    for (i = 0; i < module->tpdf_size; i++) {
        lllys_tpdf_free(ctx, &module->tpdf[i], private_destructor);
    }
    free(module->tpdf);

    /* extension instances */
    lllys_extension_instances_free(ctx, module->ext, module->ext_size, private_destructor);

    /* augment */
    for (i = 0; i < module->augment_size; i++) {
        lllys_augment_free(ctx, &module->augment[i], private_destructor);
    }
    free(module->augment);

    /* features */
    for (i = 0; i < module->features_size; i++) {
        lllys_feature_free(ctx, &module->features[i], private_destructor);
    }
    free(module->features);

    /* deviations */
    for (i = 0; i < module->deviation_size; i++) {
        lllys_deviation_free(module, &module->deviation[i], private_destructor);
    }
    free(module->deviation);

    /* extensions */
    for (i = 0; i < module->extensions_size; i++) {
        lllys_extension_free(ctx, &module->extensions[i], private_destructor);
    }
    free(module->extensions);

    lllydict_remove(ctx, module->name);
    lllydict_remove(ctx, module->prefix);
}

void
lllys_submodule_free(struct lllys_submodule *submodule, void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    int i;

    if (!submodule) {
        return;
    }

    /* common part with struct llly_module */
    module_free_common((struct lllys_module *)submodule, private_destructor);

    /* include */
    for (i = 0; i < submodule->inc_size; i++) {
        lllydict_remove(submodule->ctx, submodule->inc[i].dsc);
        lllydict_remove(submodule->ctx, submodule->inc[i].ref);
        lllys_extension_instances_free(submodule->ctx, submodule->inc[i].ext, submodule->inc[i].ext_size, private_destructor);
        /* complete submodule free is done only from main module since
         * submodules propagate their includes to the main module */
    }
    free(submodule->inc);

    free(submodule);
}

int
lllys_ingrouping(const struct lllys_node *node)
{
    const struct lllys_node *iter = node;
    assert(node);

    iter = node;
    while (iter && iter->nodetype != LLLYS_GROUPING) {
        if (iter->parent && (iter->parent->nodetype == LLLYS_AUGMENT) && iter->parent->parent) {
            /* for augments in uses, we do not care about the target */
            iter = iter->parent->parent;
        } else {
            iter = lllys_parent(iter);
        }
    }
    if (!iter) {
        return 0;
    } else {
        return 1;
    }
}

/*
 * final: 0 - do not change config flags; 1 - inherit config flags from the parent; 2 - remove config flags
 */
static struct lllys_node *
lllys_node_dup_recursion(struct lllys_module *module, struct lllys_node *parent, const struct lllys_node *node,
                       struct unres_schema *unres, int shallow, int finalize)
{
    struct lllys_node *retval = NULL, *iter, *p;
    struct llly_ctx *ctx = module->ctx;
    enum int_log_opts prev_ilo;
    int i, j, rc;
    unsigned int size, size1, size2;
    struct unres_list_uniq *unique_info;
    uint16_t flags;

    struct lllys_node_container *cont = NULL;
    struct lllys_node_container *cont_orig = (struct lllys_node_container *)node;
    struct lllys_node_choice *choice = NULL;
    struct lllys_node_choice *choice_orig = (struct lllys_node_choice *)node;
    struct lllys_node_leaf *leaf = NULL;
    struct lllys_node_leaf *leaf_orig = (struct lllys_node_leaf *)node;
    struct lllys_node_leaflist *llist = NULL;
    struct lllys_node_leaflist *llist_orig = (struct lllys_node_leaflist *)node;
    struct lllys_node_list *list = NULL;
    struct lllys_node_list *list_orig = (struct lllys_node_list *)node;
    struct lllys_node_anydata *any = NULL;
    struct lllys_node_anydata *any_orig = (struct lllys_node_anydata *)node;
    struct lllys_node_uses *uses = NULL;
    struct lllys_node_uses *uses_orig = (struct lllys_node_uses *)node;
    struct lllys_node_rpc_action *rpc = NULL;
    struct lllys_node_inout *io = NULL;
    struct lllys_node_notif *ntf = NULL;
    struct lllys_node_case *cs = NULL;
    struct lllys_node_case *cs_orig = (struct lllys_node_case *)node;

    /* we cannot just duplicate memory since the strings are stored in
     * dictionary and we need to update dictionary counters.
     */

    switch (node->nodetype) {
    case LLLYS_CONTAINER:
        cont = calloc(1, sizeof *cont);
        retval = (struct lllys_node *)cont;
        break;

    case LLLYS_CHOICE:
        choice = calloc(1, sizeof *choice);
        retval = (struct lllys_node *)choice;
        break;

    case LLLYS_LEAF:
        leaf = calloc(1, sizeof *leaf);
        retval = (struct lllys_node *)leaf;
        break;

    case LLLYS_LEAFLIST:
        llist = calloc(1, sizeof *llist);
        retval = (struct lllys_node *)llist;
        break;

    case LLLYS_LIST:
        list = calloc(1, sizeof *list);
        retval = (struct lllys_node *)list;
        break;

    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        any = calloc(1, sizeof *any);
        retval = (struct lllys_node *)any;
        break;

    case LLLYS_USES:
        uses = calloc(1, sizeof *uses);
        retval = (struct lllys_node *)uses;
        break;

    case LLLYS_CASE:
        cs = calloc(1, sizeof *cs);
        retval = (struct lllys_node *)cs;
        break;

    case LLLYS_RPC:
    case LLLYS_ACTION:
        rpc = calloc(1, sizeof *rpc);
        retval = (struct lllys_node *)rpc;
        break;

    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
        io = calloc(1, sizeof *io);
        retval = (struct lllys_node *)io;
        break;

    case LLLYS_NOTIF:
        ntf = calloc(1, sizeof *ntf);
        retval = (struct lllys_node *)ntf;
        break;

    default:
        LOGINT(ctx);
        goto error;
    }
    LLLY_CHECK_ERR_RETURN(!retval, LOGMEM(ctx), NULL);

    /*
     * duplicate generic part of the structure
     */
    retval->name = lllydict_insert(ctx, node->name, 0);
    retval->dsc = lllydict_insert(ctx, node->dsc, 0);
    retval->ref = lllydict_insert(ctx, node->ref, 0);
    retval->flags = node->flags;

    retval->module = module;
    retval->nodetype = node->nodetype;

    retval->prev = retval;

    /* copying unresolved extensions is not supported */
    if (unres_schema_find(unres, -1, (void *)&node->ext, UNRES_EXT) == -1) {
        retval->ext_size = node->ext_size;
        if (lllys_ext_dup(ctx, module, node->ext, node->ext_size, retval, LLLYEXT_PAR_NODE, &retval->ext, shallow, unres)) {
            goto error;
        }
    }

    if (node->iffeature_size) {
        retval->iffeature_size = node->iffeature_size;
        retval->iffeature = calloc(retval->iffeature_size, sizeof *retval->iffeature);
        LLLY_CHECK_ERR_GOTO(!retval->iffeature, LOGMEM(ctx), error);
    }

    if (!shallow) {
        for (i = 0; i < node->iffeature_size; ++i) {
            resolve_iffeature_getsizes(&node->iffeature[i], &size1, &size2);
            if (size1) {
                /* there is something to duplicate */

                /* duplicate compiled expression */
                size = (size1 / 4) + ((size1 % 4) ? 1 : 0);
                retval->iffeature[i].expr = malloc(size * sizeof *retval->iffeature[i].expr);
                LLLY_CHECK_ERR_GOTO(!retval->iffeature[i].expr, LOGMEM(ctx), error);
                memcpy(retval->iffeature[i].expr, node->iffeature[i].expr, size * sizeof *retval->iffeature[i].expr);

                /* list of feature pointer must be updated to point to the resulting tree */
                retval->iffeature[i].features = calloc(size2, sizeof *retval->iffeature[i].features);
                LLLY_CHECK_ERR_GOTO(!retval->iffeature[i].features, LOGMEM(ctx); free(retval->iffeature[i].expr), error);

                for (j = 0; (unsigned int)j < size2; j++) {
                    rc = unres_schema_dup(module, unres, &node->iffeature[i].features[j], UNRES_IFFEAT,
                                          &retval->iffeature[i].features[j]);
                    if (rc == EXIT_FAILURE) {
                        /* feature is resolved in origin, so copy it
                         * - duplication is used for instantiating groupings
                         * and if-feature inside grouping is supposed to be
                         * resolved inside the original grouping, so we want
                         * to keep pointers to features from the grouping
                         * context */
                        retval->iffeature[i].features[j] = node->iffeature[i].features[j];
                    } else if (rc == -1) {
                        goto error;
                    } /* else unres was duplicated */
                }
            }

            /* duplicate if-feature's extensions */
            retval->iffeature[i].ext_size = node->iffeature[i].ext_size;
            if (lllys_ext_dup(ctx, module, node->iffeature[i].ext, node->iffeature[i].ext_size,
                            &retval->iffeature[i], LLLYEXT_PAR_IFFEATURE, &retval->iffeature[i].ext, shallow, unres)) {
                goto error;
            }
        }

        /* inherit config flags */
        p = parent;
        do {
            for (iter = p; iter && (iter->nodetype == LLLYS_USES); iter = iter->parent);
        } while (iter && iter->nodetype == LLLYS_AUGMENT && (p = ((struct lllys_node_augment *)iter)->target));
        if (iter) {
            flags = iter->flags & LLLYS_CONFIG_MASK;
        } else {
            /* default */
            flags = LLLYS_CONFIG_W;
        }

        switch (finalize) {
        case 1:
            /* inherit config flags */
            if (retval->flags & LLLYS_CONFIG_SET) {
                /* skip nodes with an explicit config value */
                if ((flags & LLLYS_CONFIG_R) && (retval->flags & LLLYS_CONFIG_W)) {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, "true", "config");
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "State nodes cannot have configuration nodes as children.");
                    goto error;
                }
                break;
            }

            if (retval->nodetype != LLLYS_USES) {
                retval->flags = (retval->flags & ~LLLYS_CONFIG_MASK) | flags;
            }

            /* inherit status */
            if ((parent->flags & LLLYS_STATUS_MASK) > (retval->flags & LLLYS_STATUS_MASK)) {
                /* but do it only in case the parent has a stonger status */
                retval->flags &= ~LLLYS_STATUS_MASK;
                retval->flags |= (parent->flags & LLLYS_STATUS_MASK);
            }
            break;
        case 2:
            /* erase config flags */
            retval->flags &= ~LLLYS_CONFIG_MASK;
            retval->flags &= ~LLLYS_CONFIG_SET;
            break;
        }

        /* connect it to the parent */
        if (lllys_node_addchild(parent, retval->module, retval, 0)) {
            goto error;
        }

        /* go recursively */
        if (!(node->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
            LLLY_TREE_FOR(node->child, iter) {
                if (iter->nodetype & LLLYS_GROUPING) {
                    /* do not instantiate groupings */
                    continue;
                }
                if (!lllys_node_dup_recursion(module, retval, iter, unres, 0, finalize)) {
                    goto error;
                }
            }
        }

        if (finalize == 1) {
            /* check that configuration lists have keys
             * - we really want to check keys_size in original node, because the keys are
             * not yet resolved here, it is done below in nodetype specific part */
            if ((retval->nodetype == LLLYS_LIST) && (retval->flags & LLLYS_CONFIG_W)
                    && !((struct lllys_node_list *)node)->keys_size) {
                LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_LYS, retval, "key", "list");
                goto error;
            }
        }
    } else {
        memcpy(retval->iffeature, node->iffeature, retval->iffeature_size * sizeof *retval->iffeature);
    }

    /*
     * duplicate specific part of the structure
     */
    switch (node->nodetype) {
    case LLLYS_CONTAINER:
        if (cont_orig->when) {
            cont->when = lllys_when_dup(module, cont_orig->when, shallow, unres);
            LLLY_CHECK_GOTO(!cont->when, error);
        }
        cont->presence = lllydict_insert(ctx, cont_orig->presence, 0);

        if (cont_orig->must) {
            cont->must = lllys_restr_dup(module, cont_orig->must, cont_orig->must_size, shallow, unres);
            LLLY_CHECK_GOTO(!cont->must, error);
            cont->must_size = cont_orig->must_size;
        }

        /* typedefs are not needed in instantiated grouping, nor the deviation's shallow copy */

        break;
    case LLLYS_CHOICE:
        if (choice_orig->when) {
            choice->when = lllys_when_dup(module, choice_orig->when, shallow, unres);
            LLLY_CHECK_GOTO(!choice->when, error);
        }

        if (!shallow) {
            if (choice_orig->dflt) {
                rc = lllys_get_sibling(choice->child, lllys_node_module(retval)->name, 0, choice_orig->dflt->name, 0,
                                            LLLYS_ANYDATA | LLLYS_CASE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST,
                                            (const struct lllys_node **)&choice->dflt);
                if (rc) {
                    if (rc == EXIT_FAILURE) {
                        LOGINT(ctx);
                    }
                    goto error;
                }
            } else {
                /* useless to check return value, we don't know whether
                * there really wasn't any default defined or it just hasn't
                * been resolved, we just hope for the best :)
                */
                unres_schema_dup(module, unres, choice_orig, UNRES_CHOICE_DFLT, choice);
            }
        } else {
            choice->dflt = choice_orig->dflt;
        }
        break;

    case LLLYS_LEAF:
        if (lllys_type_dup(module, retval, &(leaf->type), &(leaf_orig->type), lllys_ingrouping(retval), shallow, unres)) {
            goto error;
        }
        leaf->units = lllydict_insert(module->ctx, leaf_orig->units, 0);

        if (leaf_orig->dflt) {
            /* transform into JSON format, may not be possible later */
            llly_ilo_change(NULL, ILO_IGNORE, &prev_ilo, NULL);
            leaf->dflt = transform_schema2json(lllys_main_module(leaf_orig->module), leaf_orig->dflt);
            llly_ilo_restore(NULL, prev_ilo, NULL, 0);
            if (!leaf->dflt) {
                /* invalid identityref format or it was already transformed, so ignore the error here */
                leaf->dflt = lllydict_insert(ctx, leaf_orig->dflt, 0);
            }
        }

        if (leaf_orig->must) {
            leaf->must = lllys_restr_dup(module, leaf_orig->must, leaf_orig->must_size, shallow, unres);
            LLLY_CHECK_GOTO(!leaf->must, error);
            leaf->must_size = leaf_orig->must_size;
        }

        if (leaf_orig->when) {
            leaf->when = lllys_when_dup(module, leaf_orig->when, shallow, unres);
            LLLY_CHECK_GOTO(!leaf->when, error);
        }
        break;

    case LLLYS_LEAFLIST:
        if (lllys_type_dup(module, retval, &(llist->type), &(llist_orig->type), lllys_ingrouping(retval), shallow, unres)) {
            goto error;
        }
        llist->units = lllydict_insert(module->ctx, llist_orig->units, 0);

        llist->min = llist_orig->min;
        llist->max = llist_orig->max;

        if (llist_orig->must) {
            llist->must = lllys_restr_dup(module, llist_orig->must, llist_orig->must_size, shallow, unres);
            LLLY_CHECK_GOTO(!llist->must, error);
            llist->must_size = llist_orig->must_size;
        }

        if (llist_orig->dflt) {
            llist->dflt = malloc(llist_orig->dflt_size * sizeof *llist->dflt);
            LLLY_CHECK_ERR_GOTO(!llist->dflt, LOGMEM(ctx), error);
            llist->dflt_size = llist_orig->dflt_size;

            for (i = 0; i < llist->dflt_size; i++) {
                llist->dflt[i] = lllydict_insert(ctx, llist_orig->dflt[i], 0);
            }
        }

        if (llist_orig->when) {
            llist->when = lllys_when_dup(module, llist_orig->when, shallow, unres);
        }
        break;

    case LLLYS_LIST:
        list->min = list_orig->min;
        list->max = list_orig->max;

        if (list_orig->must) {
            list->must = lllys_restr_dup(module, list_orig->must, list_orig->must_size, shallow, unres);
            LLLY_CHECK_GOTO(!list->must, error);
            list->must_size = list_orig->must_size;
        }

        /* typedefs are not needed in instantiated grouping, nor the deviation's shallow copy */

        if (list_orig->keys_size) {
            list->keys = calloc(list_orig->keys_size, sizeof *list->keys);
            LLLY_CHECK_ERR_GOTO(!list->keys, LOGMEM(ctx), error);
            list->keys_str = lllydict_insert(ctx, list_orig->keys_str, 0);
            list->keys_size = list_orig->keys_size;

            if (!shallow) {
                if (unres_schema_add_node(module, unres, list, UNRES_LIST_KEYS, NULL) == -1) {
                    goto error;
                }
            } else {
                memcpy(list->keys, list_orig->keys, list_orig->keys_size * sizeof *list->keys);
            }
        }

        if (list_orig->unique) {
            list->unique = malloc(list_orig->unique_size * sizeof *list->unique);
            LLLY_CHECK_ERR_GOTO(!list->unique, LOGMEM(ctx), error);
            list->unique_size = list_orig->unique_size;

            for (i = 0; i < list->unique_size; ++i) {
                list->unique[i].expr = malloc(list_orig->unique[i].expr_size * sizeof *list->unique[i].expr);
                LLLY_CHECK_ERR_GOTO(!list->unique[i].expr, LOGMEM(ctx), error);
                list->unique[i].expr_size = list_orig->unique[i].expr_size;
                for (j = 0; j < list->unique[i].expr_size; j++) {
                    list->unique[i].expr[j] = lllydict_insert(ctx, list_orig->unique[i].expr[j], 0);

                    /* if it stays in unres list, duplicate it also there */
                    unique_info = malloc(sizeof *unique_info);
                    LLLY_CHECK_ERR_GOTO(!unique_info, LOGMEM(ctx), error);
                    unique_info->list = (struct lllys_node *)list;
                    unique_info->expr = list->unique[i].expr[j];
                    unique_info->trg_type = &list->unique[i].trg_type;
                    unres_schema_dup(module, unres, &list_orig, UNRES_LIST_UNIQ, unique_info);
                }
            }
        }

        if (list_orig->when) {
            list->when = lllys_when_dup(module, list_orig->when, shallow, unres);
            LLLY_CHECK_GOTO(!list->when, error);
        }
        break;

    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        if (any_orig->must) {
            any->must = lllys_restr_dup(module, any_orig->must, any_orig->must_size, shallow, unres);
            LLLY_CHECK_GOTO(!any->must, error);
            any->must_size = any_orig->must_size;
        }

        if (any_orig->when) {
            any->when = lllys_when_dup(module, any_orig->when, shallow, unres);
            LLLY_CHECK_GOTO(!any->when, error);
        }
        break;

    case LLLYS_USES:
        uses->grp = uses_orig->grp;

        if (uses_orig->when) {
            uses->when = lllys_when_dup(module, uses_orig->when, shallow, unres);
            LLLY_CHECK_GOTO(!uses->when, error);
        }
        /* it is not needed to duplicate refine, nor augment. They are already applied to the uses children */
        break;

    case LLLYS_CASE:
        if (cs_orig->when) {
            cs->when = lllys_when_dup(module, cs_orig->when, shallow, unres);
            LLLY_CHECK_GOTO(!cs->when, error);
        }
        break;

    case LLLYS_ACTION:
    case LLLYS_RPC:
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
    case LLLYS_NOTIF:
        /* typedefs are not needed in instantiated grouping, nor the deviation's shallow copy */
        break;

    default:
        /* LLLY_NODE_AUGMENT */
        LOGINT(ctx);
        goto error;
    }

    return retval;

error:
    lllys_node_free(retval, NULL, 0);
    return NULL;
}

int
lllys_has_xpath(const struct lllys_node *node)
{
    assert(node);

    switch (node->nodetype) {
    case LLLYS_AUGMENT:
        if (((struct lllys_node_augment *)node)->when) {
            return 1;
        }
        break;
    case LLLYS_CASE:
        if (((struct lllys_node_case *)node)->when) {
            return 1;
        }
        break;
    case LLLYS_CHOICE:
        if (((struct lllys_node_choice *)node)->when) {
            return 1;
        }
        break;
    case LLLYS_ANYDATA:
        if (((struct lllys_node_anydata *)node)->when || ((struct lllys_node_anydata *)node)->must_size) {
            return 1;
        }
        break;
    case LLLYS_LEAF:
        if (((struct lllys_node_leaf *)node)->when || ((struct lllys_node_leaf *)node)->must_size) {
            return 1;
        }
        break;
    case LLLYS_LEAFLIST:
        if (((struct lllys_node_leaflist *)node)->when || ((struct lllys_node_leaflist *)node)->must_size) {
            return 1;
        }
        break;
    case LLLYS_LIST:
        if (((struct lllys_node_list *)node)->when || ((struct lllys_node_list *)node)->must_size) {
            return 1;
        }
        break;
    case LLLYS_CONTAINER:
        if (((struct lllys_node_container *)node)->when || ((struct lllys_node_container *)node)->must_size) {
            return 1;
        }
        break;
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
        if (((struct lllys_node_inout *)node)->must_size) {
            return 1;
        }
        break;
    case LLLYS_NOTIF:
        if (((struct lllys_node_notif *)node)->must_size) {
            return 1;
        }
        break;
    case LLLYS_USES:
        if (((struct lllys_node_uses *)node)->when) {
            return 1;
        }
        break;
    default:
        /* does not have XPath */
        break;
    }

    return 0;
}

int
lllys_type_is_local(const struct lllys_type *type)
{
    if (!type->der->module) {
        /* build-in type */
        return 1;
    }
    /* type->parent can be either a typedef or leaf/leaf-list, but module pointers are compatible */
    return (lllys_main_module(type->der->module) == lllys_main_module(((struct lllys_tpdf *)type->parent)->module));
}

/*
 * shallow -
 *         - do not inherit status from the parent
 */
struct lllys_node *
lllys_node_dup(struct lllys_module *module, struct lllys_node *parent, const struct lllys_node *node,
             struct unres_schema *unres, int shallow)
{
    struct lllys_node *p = NULL;
    int finalize = 0;
    struct lllys_node *result, *iter, *next;

    if (!shallow) {
        /* get know where in schema tree we are to know what should be done during instantiation of the grouping */
        for (p = parent;
             p && !(p->nodetype & (LLLYS_NOTIF | LLLYS_INPUT | LLLYS_OUTPUT | LLLYS_RPC | LLLYS_ACTION | LLLYS_GROUPING));
             p = lllys_parent(p));
        finalize = p ? ((p->nodetype == LLLYS_GROUPING) ? 0 : 2) : 1;
    }

    result = lllys_node_dup_recursion(module, parent, node, unres, shallow, finalize);
    if (finalize) {
        /* check xpath expressions in the instantiated tree */
        for (iter = next = result; iter; iter = next) {
            if (lllys_has_xpath(iter) && unres_schema_add_node(module, unres, iter, UNRES_XPATH, NULL) == -1) {
                /* invalid xpath */
                return NULL;
            }

            /* select next item */
            if (iter->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA | LLLYS_GROUPING)) {
                /* child exception for leafs, leaflists and anyxml without children, ignore groupings */
                next = NULL;
            } else {
                next = iter->child;
            }
            if (!next) {
                /* no children, try siblings */
                if (iter == result) {
                    /* we are done, no next element to process */
                    break;
                }
                next = iter->next;
            }
            while (!next) {
                /* parent is already processed, go to its sibling */
                iter = lllys_parent(iter);
                if (lllys_parent(iter) == lllys_parent(result)) {
                    /* we are done, no next element to process */
                    break;
                }
                next = iter->next;
            }
        }
    }

    return result;
}

/**
 * @brief Switch contents of two same schema nodes. One of the nodes
 * is expected to be ashallow copy of the other.
 *
 * @param[in] node1 Node whose contents will be switched with \p node2.
 * @param[in] node2 Node whose contents will be switched with \p node1.
 */
static void
lllys_node_switch(struct lllys_node *node1, struct lllys_node *node2)
{
    const size_t mem_size = 104;
    uint8_t mem[mem_size];
    size_t offset, size;

    assert((node1->module == node2->module) && llly_strequal(node1->name, node2->name, 1) && (node1->nodetype == node2->nodetype));

    /*
     * Initially, the nodes were really switched in the tree which
     * caused problems for some other nodes with pointers (augments, leafrefs, ...)
     * because their pointers were not being updated. Code kept in case there is
     * a use of it in future (it took some debugging to cover all the cases).

    * sibling next *
    if (node1->prev->next) {
        node1->prev->next = node2;
    }

    * sibling prev *
    if (node1->next) {
        node1->next->prev = node2;
    } else {
        for (child = node1->prev; child->prev->next; child = child->prev);
        child->prev = node2;
    }

    * next *
    node2->next = node1->next;
    node1->next = NULL;

    * prev *
    if (node1->prev != node1) {
        node2->prev = node1->prev;
    }
    node1->prev = node1;

    * parent child *
    if (node1->parent) {
        if (node1->parent->child == node1) {
            node1->parent->child = node2;
        }
    } else if (lllys_main_module(node1->module)->data == node1) {
        lllys_main_module(node1->module)->data = node2;
    }

    * parent *
    node2->parent = node1->parent;
    node1->parent = NULL;

    * child parent *
    LLLY_TREE_FOR(node1->child, child) {
        if (child->parent == node1) {
            child->parent = node2;
        }
    }

    * child *
    node2->child = node1->child;
    node1->child = NULL;
    */

    /* switch common node part */
    offset = 3 * sizeof(char *);
    size = sizeof(uint16_t) + 6 * sizeof(uint8_t) + sizeof(struct lllys_ext_instance **) + sizeof(struct lllys_iffeature *);
    memcpy(mem, ((uint8_t *)node1) + offset, size);
    memcpy(((uint8_t *)node1) + offset, ((uint8_t *)node2) + offset, size);
    memcpy(((uint8_t *)node2) + offset, mem, size);

    /* switch node-specific data */
    offset = sizeof(struct lllys_node);
    switch (node1->nodetype) {
    case LLLYS_CONTAINER:
        size = sizeof(struct lllys_node_container) - offset;
        break;
    case LLLYS_CHOICE:
        size = sizeof(struct lllys_node_choice) - offset;
        break;
    case LLLYS_LEAF:
        size = sizeof(struct lllys_node_leaf) - offset;
        break;
    case LLLYS_LEAFLIST:
        size = sizeof(struct lllys_node_leaflist) - offset;
        break;
    case LLLYS_LIST:
        size = sizeof(struct lllys_node_list) - offset;
        break;
    case LLLYS_ANYDATA:
    case LLLYS_ANYXML:
        size = sizeof(struct lllys_node_anydata) - offset;
        break;
    case LLLYS_CASE:
        size = sizeof(struct lllys_node_case) - offset;
        break;
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
        size = sizeof(struct lllys_node_inout) - offset;
        break;
    case LLLYS_NOTIF:
        size = sizeof(struct lllys_node_notif) - offset;
        break;
    case LLLYS_RPC:
    case LLLYS_ACTION:
        size = sizeof(struct lllys_node_rpc_action) - offset;
        break;
    default:
        assert(0);
        LOGINT(node1->module->ctx);
        return;
    }
    assert(size <= mem_size);
    memcpy(mem, ((uint8_t *)node1) + offset, size);
    memcpy(((uint8_t *)node1) + offset, ((uint8_t *)node2) + offset, size);
    memcpy(((uint8_t *)node2) + offset, mem, size);

    /* typedefs were not copied to the backup node, so always reuse them,
     * in leaves/leaf-lists we must correct the type parent pointer */
    switch (node1->nodetype) {
    case LLLYS_CONTAINER:
        ((struct lllys_node_container *)node1)->tpdf_size = ((struct lllys_node_container *)node2)->tpdf_size;
        ((struct lllys_node_container *)node1)->tpdf = ((struct lllys_node_container *)node2)->tpdf;
        ((struct lllys_node_container *)node2)->tpdf_size = 0;
        ((struct lllys_node_container *)node2)->tpdf = NULL;
        break;
    case LLLYS_LIST:
        ((struct lllys_node_list *)node1)->tpdf_size = ((struct lllys_node_list *)node2)->tpdf_size;
        ((struct lllys_node_list *)node1)->tpdf = ((struct lllys_node_list *)node2)->tpdf;
        ((struct lllys_node_list *)node2)->tpdf_size = 0;
        ((struct lllys_node_list *)node2)->tpdf = NULL;
        break;
    case LLLYS_RPC:
    case LLLYS_ACTION:
        ((struct lllys_node_rpc_action *)node1)->tpdf_size = ((struct lllys_node_rpc_action *)node2)->tpdf_size;
        ((struct lllys_node_rpc_action *)node1)->tpdf = ((struct lllys_node_rpc_action *)node2)->tpdf;
        ((struct lllys_node_rpc_action *)node2)->tpdf_size = 0;
        ((struct lllys_node_rpc_action *)node2)->tpdf = NULL;
        break;
    case LLLYS_NOTIF:
        ((struct lllys_node_notif *)node1)->tpdf_size = ((struct lllys_node_notif *)node2)->tpdf_size;
        ((struct lllys_node_notif *)node1)->tpdf = ((struct lllys_node_notif *)node2)->tpdf;
        ((struct lllys_node_notif *)node2)->tpdf_size = 0;
        ((struct lllys_node_notif *)node2)->tpdf = NULL;
        break;
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
        ((struct lllys_node_inout *)node1)->tpdf_size = ((struct lllys_node_inout *)node2)->tpdf_size;
        ((struct lllys_node_inout *)node1)->tpdf = ((struct lllys_node_inout *)node2)->tpdf;
        ((struct lllys_node_inout *)node2)->tpdf_size = 0;
        ((struct lllys_node_inout *)node2)->tpdf = NULL;
        break;
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
        ((struct lllys_node_leaf *)node1)->type.parent = (struct lllys_tpdf *)node1;
        ((struct lllys_node_leaf *)node2)->type.parent = (struct lllys_tpdf *)node2;
    default:
        break;
    }
}

void
lllys_free(struct lllys_module *module, void (*private_destructor)(const struct lllys_node *node, void *priv), int free_subs, int remove_from_ctx)
{
    struct llly_ctx *ctx;
    int i;

    if (!module) {
        return;
    }

    /* remove schema from the context */
    ctx = module->ctx;
    if (remove_from_ctx && ctx->models.used) {
        for (i = 0; i < ctx->models.used; i++) {
            if (ctx->models.list[i] == module) {
                /* move all the models to not change the order in the list */
                ctx->models.used--;
                memmove(&ctx->models.list[i], ctx->models.list[i + 1], (ctx->models.used - i) * sizeof *ctx->models.list);
                ctx->models.list[ctx->models.used] = NULL;
                /* we are done */
                break;
            }
        }
    }

    /* common part with struct llly_submodule */
    module_free_common(module, private_destructor);

    /* include */
    for (i = 0; i < module->inc_size; i++) {
        lllydict_remove(ctx, module->inc[i].dsc);
        lllydict_remove(ctx, module->inc[i].ref);
        lllys_extension_instances_free(ctx, module->inc[i].ext, module->inc[i].ext_size, private_destructor);
        /* complete submodule free is done only from main module since
         * submodules propagate their includes to the main module */
        if (free_subs) {
            lllys_submodule_free(module->inc[i].submodule, private_destructor);
        }
    }
    free(module->inc);

    /* specific items to free */
    lllydict_remove(ctx, module->ns);

    free(module);
}

static void
lllys_features_disable_recursive(struct lllys_feature *f)
{
    unsigned int i;
    struct lllys_feature *depf;

    /* disable the feature */
    f->flags &= ~LLLYS_FENABLED;

    /* by disabling feature we have to disable also all features that depends on this feature */
    if (f->depfeatures) {
        for (i = 0; i < f->depfeatures->number; i++) {
            depf = (struct lllys_feature *)f->depfeatures->set.g[i];
            if (depf->flags & LLLYS_FENABLED) {
                lllys_features_disable_recursive(depf);
            }
        }
    }
}

/*
 * op: 1 - enable, 0 - disable
 */
static int
lllys_features_change(const struct lllys_module *module, const char *name, int op)
{
    int all = 0;
    int i, j, k;
    int progress, faili, failj, failk;

    uint8_t fsize;
    struct lllys_feature *f;

    if (!module || !name || !strlen(name)) {
        LOGARG;
        return EXIT_FAILURE;
    }

    if (!strcmp(name, "*")) {
        /* enable all */
        all = 1;
    }

    progress = failk = 1;
    while (progress && failk) {
        for (i = -1, failk = progress = 0; i < module->inc_size; i++) {
            if (i == -1) {
                fsize = module->features_size;
                f = module->features;
            } else {
                fsize = module->inc[i].submodule->features_size;
                f = module->inc[i].submodule->features;
            }

            for (j = 0; j < fsize; j++) {
                if (all || !strcmp(f[j].name, name)) {
                    if ((op && (f[j].flags & LLLYS_FENABLED)) || (!op && !(f[j].flags & LLLYS_FENABLED))) {
                        if (all) {
                            /* skip already set features */
                            continue;
                        } else {
                            /* feature already set correctly */
                            return EXIT_SUCCESS;
                        }
                    }

                    if (op) {
                        /* check referenced features if they are enabled */
                        for (k = 0; k < f[j].iffeature_size; k++) {
                            if (!resolve_iffeature(&f[j].iffeature[k])) {
                                if (all) {
                                    faili = i;
                                    failj = j;
                                    failk = k + 1;
                                    break;
                                } else {
                                    LOGERR(module->ctx, LLLY_EINVAL, "Feature \"%s\" is disabled by its %d. if-feature condition.",
                                           f[j].name, k + 1);
                                    return EXIT_FAILURE;
                                }
                            }
                        }

                        if (k == f[j].iffeature_size) {
                            /* the last check passed, do the change */
                            f[j].flags |= LLLYS_FENABLED;
                            progress++;
                        }
                    } else {
                        lllys_features_disable_recursive(&f[j]);
                        progress++;
                    }
                    if (!all) {
                        /* stop in case changing a single feature */
                        return EXIT_SUCCESS;
                    }
                }
            }
        }
    }
    if (failk) {
        /* print info about the last failing feature */
        LOGERR(module->ctx, LLLY_EINVAL, "Feature \"%s\" is disabled by its %d. if-feature condition.",
               faili == -1 ? module->features[failj].name : module->inc[faili].submodule->features[failj].name, failk);
        return EXIT_FAILURE;
    }

    if (all) {
        return EXIT_SUCCESS;
    } else {
        /* the specified feature not found */
        return EXIT_FAILURE;
    }
}

API int
lllys_features_enable(const struct lllys_module *module, const char *feature)
{
    FUN_IN;

    return lllys_features_change(module, feature, 1);
}

API int
lllys_features_disable(const struct lllys_module *module, const char *feature)
{
    FUN_IN;

    return lllys_features_change(module, feature, 0);
}

API int
lllys_features_state(const struct lllys_module *module, const char *feature)
{
    FUN_IN;

    int i, j;

    if (!module || !feature) {
        return -1;
    }

    /* search for the specified feature */
    /* module itself */
    for (i = 0; i < module->features_size; i++) {
        if (!strcmp(feature, module->features[i].name)) {
            if (module->features[i].flags & LLLYS_FENABLED) {
                return 1;
            } else {
                return 0;
            }
        }
    }

    /* submodules */
    for (j = 0; j < module->inc_size; j++) {
        for (i = 0; i < module->inc[j].submodule->features_size; i++) {
            if (!strcmp(feature, module->inc[j].submodule->features[i].name)) {
                if (module->inc[j].submodule->features[i].flags & LLLYS_FENABLED) {
                    return 1;
                } else {
                    return 0;
                }
            }
        }
    }

    /* feature definition not found */
    return -1;
}

API const char **
lllys_features_list(const struct lllys_module *module, uint8_t **states)
{
    FUN_IN;

    const char **result = NULL;
    int i, j;
    unsigned int count;

    if (!module) {
        return NULL;
    }

    count = module->features_size;
    for (i = 0; i < module->inc_size; i++) {
        count += module->inc[i].submodule->features_size;
    }
    result = malloc((count + 1) * sizeof *result);
    LLLY_CHECK_ERR_RETURN(!result, LOGMEM(module->ctx), NULL);

    if (states) {
        *states = malloc((count + 1) * sizeof **states);
        LLLY_CHECK_ERR_RETURN(!(*states), LOGMEM(module->ctx); free(result), NULL);
    }
    count = 0;

    /* module itself */
    for (i = 0; i < module->features_size; i++) {
        result[count] = module->features[i].name;
        if (states) {
            if (module->features[i].flags & LLLYS_FENABLED) {
                (*states)[count] = 1;
            } else {
                (*states)[count] = 0;
            }
        }
        count++;
    }

    /* submodules */
    for (j = 0; j < module->inc_size; j++) {
        for (i = 0; i < module->inc[j].submodule->features_size; i++) {
            result[count] = module->inc[j].submodule->features[i].name;
            if (states) {
                if (module->inc[j].submodule->features[i].flags & LLLYS_FENABLED) {
                    (*states)[count] = 1;
                } else {
                    (*states)[count] = 0;
                }
            }
            count++;
        }
    }

    /* terminating NULL byte */
    result[count] = NULL;

    return result;
}

API struct lllys_module *
lllys_node_module(const struct lllys_node *node)
{
    FUN_IN;

    if (!node) {
        return NULL;
    }

    return node->module->type ? ((struct lllys_submodule *)node->module)->belongsto : node->module;
}

API struct lllys_module *
lllys_main_module(const struct lllys_module *module)
{
    FUN_IN;

    if (!module) {
        return NULL;
    }

    return (module->type ? ((struct lllys_submodule *)module)->belongsto : (struct lllys_module *)module);
}

API struct lllys_node *
lllys_parent(const struct lllys_node *node)
{
    FUN_IN;

    struct lllys_node *parent;

    if (!node) {
        return NULL;
    }

    if (node->nodetype == LLLYS_EXT) {
        if (((struct lllys_ext_instance_complex*)node)->parent_type != LLLYEXT_PAR_NODE) {
            return NULL;
        }
        parent = (struct lllys_node*)((struct lllys_ext_instance_complex*)node)->parent;
    } else if (!node->parent) {
        return NULL;
    } else {
        parent = node->parent;
    }

    if (parent->nodetype == LLLYS_AUGMENT) {
        return ((struct lllys_node_augment *)parent)->target;
    } else {
        return parent;
    }
}

struct lllys_node **
lllys_child(const struct lllys_node *node, LLLYS_NODE nodetype)
{
    void *pp;
    assert(node);

    if (node->nodetype == LLLYS_EXT) {
        pp = lllys_ext_complex_get_substmt(lllys_snode2stmt(nodetype), (struct lllys_ext_instance_complex*)node, NULL);
        if (!pp) {
            return NULL;
        }
        return (struct lllys_node **)pp;
    } else if (node->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA)) {
        return NULL;
    } else {
        return (struct lllys_node **)&node->child;
    }
}

API void *
lllys_set_private(const struct lllys_node *node, void *priv)
{
    FUN_IN;

    void *prev;

    if (!node) {
        LOGARG;
        return NULL;
    }

    prev = node->priv;
    ((struct lllys_node *)node)->priv = priv;

    return prev;
}

int
lllys_leaf_add_leafref_target(struct lllys_node_leaf *leafref_target, struct lllys_node *leafref)
{
    struct lllys_node_leaf *iter;
    struct llly_ctx *ctx = leafref_target->module->ctx;

    if (!(leafref_target->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
        LOGINT(ctx);
        return -1;
    }

    /* check for config flag */
    if (((struct lllys_node_leaf*)leafref)->type.info.lref.req != -1 &&
            (leafref->flags & LLLYS_CONFIG_W) && (leafref_target->flags & LLLYS_CONFIG_R)) {
        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_LYS, leafref,
               "The leafref %s is config but refers to a non-config %s.",
               strnodetype(leafref->nodetype), strnodetype(leafref_target->nodetype));
        return -1;
    }
    /* check for cycles */
    for (iter = leafref_target; iter && iter->type.base == LLLY_TYPE_LEAFREF; iter = iter->type.info.lref.target) {
        if ((void *)iter == (void *)leafref) {
            /* cycle detected */
            LOGVAL(ctx, LLLYE_CIRC_LEAFREFS, LLLY_VLOG_LYS, leafref);
            return -1;
        }
    }

    /* create fake child - the llly_set structure to hold the list of
     * leafrefs referencing the leaf(-list) */
    if (!leafref_target->backlinks) {
        leafref_target->backlinks = (void *)llly_set_new();
        if (!leafref_target->backlinks) {
            LOGMEM(ctx);
            return -1;
        }
    }
    llly_set_add(leafref_target->backlinks, leafref, 0);

    return 0;
}

/* not needed currently */
#if 0

static const char *
lllys_data_path_reverse(const struct lllys_node *node, char * const buf, uint32_t buf_len)
{
    struct lllys_module *prev_mod;
    uint32_t str_len, mod_len, buf_idx;

    if (!(node->nodetype & (LLLYS_CONTAINER | LLLYS_LIST | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA))) {
        LOGINT;
        return NULL;
    }

    buf_idx = buf_len - 1;
    buf[buf_idx] = '\0';

    while (node) {
        if (lllys_parent(node)) {
            prev_mod = lllys_node_module(lllys_parent(node));
        } else {
            prev_mod = NULL;
        }

        if (node->nodetype & (LLLYS_CONTAINER | LLLYS_LIST | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA)) {
            str_len = strlen(node->name);

            if (prev_mod != node->module) {
                mod_len = strlen(node->module->name);
            } else {
                mod_len = 0;
            }

            if (buf_idx < 1 + (mod_len ? mod_len + 1 : 0) + str_len) {
                LOGINT;
                return NULL;
            }

            buf_idx -= 1 + (mod_len ? mod_len + 1 : 0) + str_len;

            buf[buf_idx] = '/';
            if (mod_len) {
                memcpy(buf + buf_idx + 1, node->module->name, mod_len);
                buf[buf_idx + 1 + mod_len] = ':';
            }
            memcpy(buf + buf_idx + 1 + (mod_len ? mod_len + 1 : 0), node->name, str_len);
        }

        node = lllys_parent(node);
    }

    return buf + buf_idx;
}

#endif

API struct llly_set *
lllys_xpath_atomize(const struct lllys_node *ctx_node, enum lllyxp_node_type ctx_node_type, const char *expr, int options)
{
    FUN_IN;

    struct lllyxp_set set;
    const struct lllys_node *parent;
    struct llly_set *ret_set;
    uint32_t i;

    if (!ctx_node || !expr) {
        LOGARG;
        return NULL;
    }

    /* adjust the root */
    if ((ctx_node_type == LLLYXP_NODE_ROOT) || (ctx_node_type == LLLYXP_NODE_ROOT_CONFIG)) {
        do {
            ctx_node = lllys_getnext(NULL, NULL, lllys_node_module(ctx_node), LLLYS_GETNEXT_NOSTATECHECK);
        } while ((ctx_node_type == LLLYXP_NODE_ROOT_CONFIG) && (ctx_node->flags & LLLYS_CONFIG_R));
    }

    memset(&set, 0, sizeof set);

    for (parent = ctx_node; parent && (parent->nodetype != LLLYS_OUTPUT); parent = lllys_parent(parent));
    if (parent) {
        options &= ~(LLLYXP_MUST | LLLYXP_WHEN);
        options |= LLLYXP_SNODE_OUTPUT;
    } else if (options & LLLYXP_MUST) {
        options &= ~LLLYXP_MUST;
        options |= LLLYXP_SNODE_MUST;
    } else if (options & LLLYXP_WHEN) {
        options &= ~LLLYXP_WHEN;
        options |= LLLYXP_SNODE_WHEN;
    } else {
        options |= LLLYXP_SNODE;
    }

    if (lllyxp_atomize(expr, ctx_node, ctx_node_type, &set, options, NULL)) {
        free(set.val.snodes);
        LOGVAL(ctx_node->module->ctx, LLLYE_SPEC, LLLY_VLOG_LYS, ctx_node, "Resolving XPath expression \"%s\" failed.", expr);
        return NULL;
    }

    ret_set = llly_set_new();

    for (i = 0; i < set.used; ++i) {
        switch (set.val.snodes[i].type) {
        case LLLYXP_NODE_ELEM:
            if (llly_set_add(ret_set, set.val.snodes[i].snode, LLLY_SET_OPT_USEASLIST) == -1) {
                llly_set_free(ret_set);
                free(set.val.snodes);
                return NULL;
            }
            break;
        default:
            /* ignore roots, text and attr should not ever appear */
            break;
        }
    }

    free(set.val.snodes);
    return ret_set;
}

API struct llly_set *
lllys_node_xpath_atomize(const struct lllys_node *node, int options)
{
    FUN_IN;

    const struct lllys_node *next, *elem, *parent, *tmp;
    struct lllyxp_set set;
    struct llly_set *ret_set;
    uint16_t i;

    if (!node) {
        LOGARG;
        return NULL;
    }

    for (parent = node; parent && !(parent->nodetype & (LLLYS_NOTIF | LLLYS_INPUT | LLLYS_OUTPUT)); parent = lllys_parent(parent));
    if (!parent) {
        /* not in input, output, or notification */
        return NULL;
    }

    ret_set = llly_set_new();
    if (!ret_set) {
        return NULL;
    }

    LLLY_TREE_DFS_BEGIN(node, next, elem) {
        if ((options & LLLYXP_NO_LOCAL) && !(elem->flags & (LLLYS_XPCONF_DEP | LLLYS_XPSTATE_DEP))) {
            /* elem has no dependencies from other subtrees and local nodes get discarded */
            goto next_iter;
        }

        if (lllyxp_node_atomize(elem, &set, 0)) {
            llly_set_free(ret_set);
            free(set.val.snodes);
            return NULL;
        }

        for (i = 0; i < set.used; ++i) {
            switch (set.val.snodes[i].type) {
            case LLLYXP_NODE_ELEM:
                if (options & LLLYXP_NO_LOCAL) {
                    for (tmp = set.val.snodes[i].snode; tmp && (tmp != parent); tmp = lllys_parent(tmp));
                    if (tmp) {
                        /* in local subtree, discard */
                        break;
                    }
                }
                if (llly_set_add(ret_set, set.val.snodes[i].snode, 0) == -1) {
                    llly_set_free(ret_set);
                    free(set.val.snodes);
                    return NULL;
                }
                break;
            default:
                /* ignore roots, text and attr should not ever appear */
                break;
            }
        }

        free(set.val.snodes);
        if (!(options & LLLYXP_RECURSIVE)) {
            break;
        }
next_iter:
        LLLY_TREE_DFS_END(node, next, elem);
    }

    return ret_set;
}

/* logs */
int
apply_aug(struct lllys_node_augment *augment, struct unres_schema *unres)
{
    struct lllys_node *child, *parent;
    struct lllys_module *mod;
    struct lllys_type *type;
    int clear_config;
    unsigned int u;
    uint8_t *v;
    struct lllys_ext_instance *ext;

    assert(augment->target && (augment->flags & LLLYS_NOTAPPLIED));

    if (!augment->child) {
        /* nothing to apply */
        goto success;
    }

    /* inherit config information from actual parent */
    for (parent = augment->target; parent && !(parent->nodetype & (LLLYS_NOTIF | LLLYS_INPUT | LLLYS_OUTPUT | LLLYS_RPC)); parent = lllys_parent(parent));
    clear_config = (parent) ? 1 : 0;
    LLLY_TREE_FOR(augment->child, child) {
        if (inherit_config_flag(child, augment->target->flags & LLLYS_CONFIG_MASK, clear_config)) {
            return -1;
        }
    }

    /* inherit extensions if any */
    for (u = 0; u < augment->target->ext_size; u++) {
        ext = augment->target->ext[u]; /* shortcut */
        if (ext && ext->def->plugin && (ext->def->plugin->flags & LLLYEXT_OPT_INHERIT)) {
            v = malloc(sizeof *v);
            LLLY_CHECK_ERR_RETURN(!v, LOGMEM(augment->module->ctx), -1);
            *v = u;
            if (unres_schema_add_node(lllys_main_module(augment->module), unres, &augment->target->ext,
                    UNRES_EXT_FINALIZE, (struct lllys_node *)v) == -1) {
                /* something really bad happened since the extension finalization is not actually
                 * being resolved while adding into unres, so something more serious with the unres
                 * list itself must happened */
                return -1;
            }
        }
    }

    /* check that all leafrefs point to implemented modules */
    LLLY_TREE_DFS_BEGIN((struct lllys_node *)augment, parent, child) {
        if (child->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) {
            type = &((struct lllys_node_leaf *)child)->type;
            if (type->base == LLLY_TYPE_LEAFREF) {
                /* must be resolved or in unres */
                if (!type->info.lref.target) {
                    if (unres_schema_find(unres, -1, type, UNRES_TYPE_LEAFREF) > -1) {
                        if (unres_schema_add_node(lllys_node_module(child), unres, type, UNRES_TYPE_LEAFREF, child) == -1) {
                            return -1;
                        }
                    }
                } else {
                    mod = lllys_node_module((struct lllys_node *)type->info.lref.target);
                    if (!mod->implemented) {
                        mod->implemented = 1;
                        if (unres_schema_add_node(mod, unres, NULL, UNRES_MOD_IMPLEMENT, NULL) == -1) {
                            return -1;
                        }
                    }
                }
            }
        }

        LLLY_TREE_DFS_END((struct lllys_node *)augment, parent, child);
    }

    /* reconnect augmenting data into the target - add them to the target child list */
    if (augment->target->child) {
        child = augment->target->child->prev;
        child->next = augment->child;
        augment->target->child->prev = augment->child->prev;
        augment->child->prev = child;
    } else {
        augment->target->child = augment->child;
    }

success:
    /* remove the flag about not applicability */
    augment->flags &= ~LLLYS_NOTAPPLIED;
    return EXIT_SUCCESS;
}

static void
remove_aug(struct lllys_node_augment *augment)
{
    struct lllys_node *last, *elem;

    if ((augment->flags & LLLYS_NOTAPPLIED) || !augment->target) {
        /* skip already not applied augment */
        return;
    }

    elem = augment->child;
    if (elem) {
        LLLY_TREE_FOR(elem, last) {
            if (!last->next || (last->next->parent != (struct lllys_node *)augment)) {
                break;
            }
        }
        /* elem is first augment child, last is the last child */

        /* parent child ptr */
        if (augment->target->child == elem) {
            augment->target->child = last->next;
        }

        /* parent child next ptr */
        if (elem->prev->next) {
            elem->prev->next = last->next;
        }

        /* parent child prev ptr */
        if (last->next) {
            last->next->prev = elem->prev;
        } else if (augment->target->child) {
            augment->target->child->prev = elem->prev;
        }

        /* update augment children themselves */
        elem->prev = last;
        last->next = NULL;
    }

    /* augment->target still keeps the resolved target, but for lllys_augment_free()
     * we have to keep information that this augment is not applied to free its data */
    augment->flags |= LLLYS_NOTAPPLIED;
}

/*
 * @param[in] module - the module where the deviation is defined
 */
static void
lllys_switch_deviation(struct lllys_deviation *dev, const struct lllys_module *module, struct unres_schema *unres)
{
    int ret, reapply = 0;
    char *parent_path;
    struct lllys_node *target = NULL, *parent;
    struct lllys_node_inout *inout;
    struct llly_set *set;

    if (!dev->deviate) {
        return;
    }

    if (dev->deviate[0].mod == LLLY_DEVIATE_NO) {
        if (dev->orig_node) {
            /* removing not-supported deviation ... */
            if (strrchr(dev->target_name, '/') != dev->target_name) {
                /* ... from a parent */

                /* reconnect to its previous position */
                parent = dev->orig_node->parent;
                if (parent && (parent->nodetype == LLLYS_AUGMENT)) {
                    dev->orig_node->parent = NULL;
                    /* the original node was actually from augment, we have to get know if the augment is
                     * applied (its module is enabled and implemented). If yes, the node will be connected
                     * to the augment and the linkage with the target will be fixed if needed, otherwise
                     * it will be connected only to the augment */
                    if (!(parent->flags & LLLYS_NOTAPPLIED)) {
                        /* start with removing augment if applied before adding nodes, we have to make sure
                         * that everything will be connect correctly */
                        remove_aug((struct lllys_node_augment *)parent);
                        reapply = 1;
                    }
                    /* connect the deviated node back into the augment */
                    lllys_node_addchild(parent, NULL, dev->orig_node, 0);
                    if (reapply) {
                        /* augment is supposed to be applied, so fix pointers in target and the status of the original node */
                        assert(lllys_node_module(parent)->implemented);
                        parent->flags |= LLLYS_NOTAPPLIED; /* allow apply_aug() */
                        apply_aug((struct lllys_node_augment *)parent, unres);
                    }
                } else if (parent && (parent->nodetype == LLLYS_USES)) {
                    /* uses child */
                    lllys_node_addchild(parent, NULL, dev->orig_node, 0);
                } else {
                    /* non-augment, non-toplevel */
                    parent_path = strndup(dev->target_name, strrchr(dev->target_name, '/') - dev->target_name);
                    ret = resolve_schema_nodeid(parent_path, NULL, module, &set, 0, 1);
                    free(parent_path);
                    if (ret == -1) {
                        LOGINT(module->ctx);
                        llly_set_free(set);
                        return;
                    }
                    target = set->set.s[0];
                    llly_set_free(set);

                    lllys_node_addchild(target, NULL, dev->orig_node, 0);
                }
            } else {
                /* ... from top-level data */
                lllys_node_addchild(NULL, lllys_node_module(dev->orig_node), dev->orig_node, 0);
            }

            dev->orig_node = NULL;
        } else {
            /* adding not-supported deviation */
            ret = resolve_schema_nodeid(dev->target_name, NULL, module, &set, 0, 1);
            if (ret == -1) {
                LOGINT(module->ctx);
                llly_set_free(set);
                return;
            }
            target = set->set.s[0];
            llly_set_free(set);

            /* unlink and store the original node */
            parent = target->parent;
            lllys_node_unlink(target);
            if (parent) {
                if (parent->nodetype & (LLLYS_AUGMENT | LLLYS_USES)) {
                    /* hack for augment, because when the original will be sometime reconnected back, we actually need
                     * to reconnect it to both - the augment and its target (which is deduced from the deviations target
                     * path), so we need to remember the augment as an addition */
                    /* we also need to remember the parent uses so that we connect it back to it when switching deviation state */
                    target->parent = parent;
                } else if (parent->nodetype & (LLLYS_RPC | LLLYS_ACTION)) {
                    /* re-create implicit node */
                    inout = calloc(1, sizeof *inout);
                    LLLY_CHECK_ERR_RETURN(!inout, LOGMEM(module->ctx), );

                    inout->nodetype = target->nodetype;
                    inout->name = lllydict_insert(module->ctx, (inout->nodetype == LLLYS_INPUT) ? "input" : "output", 0);
                    inout->module = target->module;
                    inout->flags = LLLYS_IMPLICIT;

                    /* insert it manually */
                    assert(parent->child && !parent->child->next
                    && (parent->child->nodetype == (inout->nodetype == LLLYS_INPUT ? LLLYS_OUTPUT : LLLYS_INPUT)));
                    parent->child->next = (struct lllys_node *)inout;
                    inout->prev = parent->child;
                    parent->child->prev = (struct lllys_node *)inout;
                    inout->parent = parent;
                }
            }
            dev->orig_node = target;
        }
    } else {
        ret = resolve_schema_nodeid(dev->target_name, NULL, module, &set, 0, 1);
        if (ret == -1) {
            LOGINT(module->ctx);
            llly_set_free(set);
            return;
        }
        target = set->set.s[0];
        llly_set_free(set);

        /* contents are switched */
        lllys_node_switch(target, dev->orig_node);
    }
}

/* temporarily removes or applies deviations, updates module deviation flag accordingly */
void
lllys_enable_deviations(struct lllys_module *module)
{
    uint32_t i = 0, j;
    const struct lllys_module *mod;
    const char *ptr;
    struct unres_schema *unres;

    if (module->deviated) {
        unres = calloc(1, sizeof *unres);
        LLLY_CHECK_ERR_RETURN(!unres, LOGMEM(module->ctx), );

        while ((mod = llly_ctx_get_module_iter(module->ctx, &i))) {
            if (mod == module) {
                continue;
            }

            for (j = 0; j < mod->deviation_size; ++j) {
                ptr = strstr(mod->deviation[j].target_name, module->name);
                if (ptr && ptr[strlen(module->name)] == ':') {
                    lllys_switch_deviation(&mod->deviation[j], mod, unres);
                }
            }
        }

        assert(module->deviated == 2);
        module->deviated = 1;

        for (j = 0; j < module->inc_size; j++) {
            if (module->inc[j].submodule->deviated) {
                module->inc[j].submodule->deviated = module->deviated;
            }
        }

        if (unres->count) {
            resolve_unres_schema(module, unres);
        }
        unres_schema_free(module, &unres, 1);
    }
}

void
lllys_disable_deviations(struct lllys_module *module)
{
    uint32_t i, j;
    const struct lllys_module *mod;
    const char *ptr;
    struct unres_schema *unres;

    if (module->deviated) {
        unres = calloc(1, sizeof *unres);
        LLLY_CHECK_ERR_RETURN(!unres, LOGMEM(module->ctx), );

        i = module->ctx->models.used;
        while (i--) {
            mod = module->ctx->models.list[i];

            if (mod == module) {
                continue;
            }

            j = mod->deviation_size;
            while (j--) {
                ptr = strstr(mod->deviation[j].target_name, module->name);
                if (ptr && ptr[strlen(module->name)] == ':') {
                    lllys_switch_deviation(&mod->deviation[j], mod, unres);
                }
            }
        }

        assert(module->deviated == 1);
        module->deviated = 2;

        for (j = 0; j < module->inc_size; j++) {
            if (module->inc[j].submodule->deviated) {
                module->inc[j].submodule->deviated = module->deviated;
            }
        }

        if (unres->count) {
            resolve_unres_schema(module, unres);
        }
        unres_schema_free(module, &unres, 1);
    }
}

static void
apply_dev(struct lllys_deviation *dev, const struct lllys_module *module, struct unres_schema *unres)
{
    lllys_switch_deviation(dev, module, unres);

    assert(dev->orig_node);
    lllys_node_module(dev->orig_node)->deviated = 1; /* main module */
    dev->orig_node->module->deviated = 1;          /* possible submodule */
}

static void
remove_dev(struct lllys_deviation *dev, const struct lllys_module *module, struct unres_schema *unres)
{
    uint32_t idx = 0, j;
    const struct lllys_module *mod;
    struct lllys_module *target_mod, *target_submod;
    const char *ptr;

    if (dev->orig_node) {
        target_mod = lllys_node_module(dev->orig_node);
        target_submod = dev->orig_node->module;
    } else {
        LOGINT(module->ctx);
        return;
    }
    lllys_switch_deviation(dev, module, unres);

    /* clear the deviation flag if possible */
    while ((mod = llly_ctx_get_module_iter(module->ctx, &idx))) {
        if ((mod == module) || (mod == target_mod)) {
            continue;
        }

        for (j = 0; j < mod->deviation_size; ++j) {
            ptr = strstr(mod->deviation[j].target_name, target_mod->name);
            if (ptr && (ptr[strlen(target_mod->name)] == ':')) {
                /* some other module deviation targets the inspected module, flag remains */
                break;
            }
        }

        if (j < mod->deviation_size) {
            break;
        }
    }

    if (!mod) {
        target_mod->deviated = 0;    /* main module */
        target_submod->deviated = 0; /* possible submodule */
    }
}

void
lllys_sub_module_apply_devs_augs(struct lllys_module *module)
{
    uint8_t u, v;
    struct unres_schema *unres;

    assert(module->implemented);

    unres = calloc(1, sizeof *unres);
    LLLY_CHECK_ERR_RETURN(!unres, LOGMEM(module->ctx), );

    /* apply deviations */
    for (u = 0; u < module->deviation_size; ++u) {
        apply_dev(&module->deviation[u], module, unres);
    }
    /* apply augments */
    for (u = 0; u < module->augment_size; ++u) {
        apply_aug(&module->augment[u], unres);
    }

    /* apply deviations and augments defined in submodules */
    for (v = 0; v < module->inc_size; ++v) {
        for (u = 0; u < module->inc[v].submodule->deviation_size; ++u) {
            apply_dev(&module->inc[v].submodule->deviation[u], module, unres);
        }

        for (u = 0; u < module->inc[v].submodule->augment_size; ++u) {
            apply_aug(&module->inc[v].submodule->augment[u], unres);
        }
    }

    if (unres->count) {
        resolve_unres_schema(module, unres);
    }
    /* nothing else left to do even if something is not resolved */
    unres_schema_free(module, &unres, 1);
}

void
lllys_sub_module_remove_devs_augs(struct lllys_module *module)
{
    uint8_t u, v, w;
    struct unres_schema *unres;

    unres = calloc(1, sizeof *unres);
    LLLY_CHECK_ERR_RETURN(!unres, LOGMEM(module->ctx), );

    /* remove applied deviations */
    for (u = 0; u < module->deviation_size; ++u) {
        /* the deviation could not be applied because it failed to be applied in the first place*/
        if (module->deviation[u].orig_node) {
            remove_dev(&module->deviation[u], module, unres);
        }

        /* Free the deviation's must array(s). These are shallow copies of the arrays
           on the target node(s), so a deep free is not needed. */
        for (v = 0; v < module->deviation[u].deviate_size; ++v) {
            if (module->deviation[u].deviate[v].mod == LLLY_DEVIATE_ADD) {
                free(module->deviation[u].deviate[v].must);
            }
        }
    }
    /* remove applied augments */
    for (u = 0; u < module->augment_size; ++u) {
        remove_aug(&module->augment[u]);
    }

    /* remove deviation and augments defined in submodules */
    for (v = 0; v < module->inc_size && module->inc[v].submodule; ++v) {
        for (u = 0; u < module->inc[v].submodule->deviation_size; ++u) {
            if (module->inc[v].submodule->deviation[u].orig_node) {
                remove_dev(&module->inc[v].submodule->deviation[u], module, unres);
            }

            /* Free the deviation's must array(s). These are shallow copies of the arrays
               on the target node(s), so a deep free is not needed. */
            for (w = 0; w < module->inc[v].submodule->deviation[u].deviate_size; ++w) {
                if (module->inc[v].submodule->deviation[u].deviate[w].mod == LLLY_DEVIATE_ADD) {
                    free(module->inc[v].submodule->deviation[u].deviate[w].must);
                }
            }
        }

        for (u = 0; u < module->inc[v].submodule->augment_size; ++u) {
            remove_aug(&module->inc[v].submodule->augment[u]);
        }
    }

    if (unres->count) {
        resolve_unres_schema(module, unres);
    }
    /* nothing else left to do even if something is not resolved */
    unres_schema_free(module, &unres, 1);
}

int
lllys_make_implemented_r(struct lllys_module *module, struct unres_schema *unres)
{
    struct llly_ctx *ctx;
    struct lllys_node *root, *next, *node;
    struct lllys_module *target_module;
    uint16_t i, j, k;

    assert(module->implemented);
    ctx = module->ctx;

    for (i = 0; i < ctx->models.used; ++i) {
        if (module == ctx->models.list[i]) {
            continue;
        }

        if (!strcmp(module->name, ctx->models.list[i]->name) && ctx->models.list[i]->implemented) {
            LOGERR(ctx, LLLY_EINVAL, "Module \"%s\" in another revision already implemented.", module->name);
            return EXIT_FAILURE;
        }
    }

    for (i = 0; i < module->augment_size; i++) {

        /* make target module implemented if was not */
        assert(module->augment[i].target);
        target_module = lllys_node_module(module->augment[i].target);
        if (!target_module->implemented) {
            target_module->implemented = 1;
            if (unres_schema_add_node(target_module, unres, NULL, UNRES_MOD_IMPLEMENT, NULL) == -1) {
                return -1;
            }
        }

        /* apply augment */
        if ((module->augment[i].flags & LLLYS_NOTAPPLIED) && apply_aug(&module->augment[i], unres)) {
            return -1;
        }
    }

    /* identities */
    for (i = 0; i < module->ident_size; i++) {
        for (j = 0; j < module->ident[i].base_size; j++) {
            resolve_identity_backlink_update(&module->ident[i], module->ident[i].base[j]);
        }
    }

    /* process augments in submodules */
    for (i = 0; i < module->inc_size && module->inc[i].submodule; ++i) {
        module->inc[i].submodule->implemented = 1;

        for (j = 0; j < module->inc[i].submodule->augment_size; j++) {

            /* make target module implemented if it was not */
            assert(module->inc[i].submodule->augment[j].target);
            target_module = lllys_node_module(module->inc[i].submodule->augment[j].target);
            if (!target_module->implemented) {
                target_module->implemented = 1;
                if (unres_schema_add_node(target_module, unres, NULL, UNRES_MOD_IMPLEMENT, NULL) == -1) {
                    return -1;
                }
            }

            /* apply augment */
            if ((module->inc[i].submodule->augment[j].flags & LLLYS_NOTAPPLIED) && apply_aug(&module->inc[i].submodule->augment[j], unres)) {
                return -1;
            }
        }

        /* identities */
        for (j = 0; j < module->inc[i].submodule->ident_size; j++) {
            for (k = 0; k < module->inc[i].submodule->ident[j].base_size; k++) {
                resolve_identity_backlink_update(&module->inc[i].submodule->ident[j],
                                                 module->inc[i].submodule->ident[j].base[k]);
            }
        }
    }

    LLLY_TREE_FOR(module->data, root) {
        /* handle leafrefs and recursively change the implemented flags in the leafref targets */
        LLLY_TREE_DFS_BEGIN(root, next, node) {
            if (node->nodetype == LLLYS_GROUPING) {
                goto nextsibling;
            }
            if (node->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) {
                if (((struct lllys_node_leaf *)node)->type.base == LLLY_TYPE_LEAFREF) {
                    if (unres_schema_add_node(module, unres, &((struct lllys_node_leaf *)node)->type,
                                              UNRES_TYPE_LEAFREF, node) == -1) {
                        return -1;
                    }
                }
            }

            /* modified LLLY_TREE_DFS_END */
            next = node->child;
            /* child exception for leafs, leaflists and anyxml without children */
            if (node->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA)) {
                next = NULL;
            }
            if (!next) {
nextsibling:
                /* no children */
                if (node == root) {
                    /* we are done, root has no children */
                    break;
                }
                /* try siblings */
                next = node->next;
            }
            while (!next) {
                /* parent is already processed, go to its sibling */
                node = lllys_parent(node);
                /* no siblings, go back through parents */
                if (lllys_parent(node) == lllys_parent(root)) {
                    /* we are done, no next element to process */
                    break;
                }
                next = node->next;
            }
        }
    }

    return EXIT_SUCCESS;
}

API int
lllys_set_implemented(const struct lllys_module *module)
{
    FUN_IN;

    struct unres_schema *unres;
    int disabled = 0;

    if (!module) {
        LOGARG;
        return EXIT_FAILURE;
    }

    module = lllys_main_module(module);

    if (module->disabled) {
        disabled = 1;
        lllys_set_enabled(module);
    }

    if (module->implemented) {
        return EXIT_SUCCESS;
    }

    unres = calloc(1, sizeof *unres);
    if (!unres) {
        LOGMEM(module->ctx);
        if (disabled) {
            /* set it back disabled */
            lllys_set_disabled(module);
        }
        return EXIT_FAILURE;
    }
    /* recursively make the module implemented */
    ((struct lllys_module *)module)->implemented = 1;
    if (lllys_make_implemented_r((struct lllys_module *)module, unres)) {
        goto error;
    }

    /* try again resolve augments in other modules possibly augmenting this one,
     * since we have just enabled it
     */
    /* resolve rest of unres items */
    if (unres->count && resolve_unres_schema((struct lllys_module *)module, unres)) {
        goto error;
    }
    unres_schema_free(NULL, &unres, 0);

    LOGVRB("Module \"%s%s%s\" now implemented.", module->name, (module->rev_size ? "@" : ""),
           (module->rev_size ? module->rev[0].date : ""));
    return EXIT_SUCCESS;

error:
    if (disabled) {
        /* set it back disabled */
        lllys_set_disabled(module);
    }

    ((struct lllys_module *)module)->implemented = 0;
    unres_schema_free((struct lllys_module *)module, &unres, 1);
    return EXIT_FAILURE;
}

void
lllys_submodule_module_data_free(struct lllys_submodule *submodule)
{
    struct lllys_node *next, *elem;

    /* remove parsed data */
    LLLY_TREE_FOR_SAFE(submodule->belongsto->data, next, elem) {
        if (elem->module == (struct lllys_module *)submodule) {
            lllys_node_free(elem, NULL, 0);
        }
    }
}

API char *
lllys_path(const struct lllys_node *node, int options)
{
    FUN_IN;

    char *buf = NULL;

    if (!node) {
        LOGARG;
        return NULL;
    }

    if (llly_vlog_build_path(LLLY_VLOG_LYS, node, &buf, (options & LLLYS_PATH_FIRST_PREFIX) ? 0 : 1, 0)) {
        return NULL;
    }

    return buf;
}

API char *
lllys_data_path(const struct lllys_node *node)
{
    FUN_IN;

    char *result = NULL, buf[1024];
    const char *separator, *name;
    int i, used;
    struct llly_set *set;
    const struct lllys_module *prev_mod;

    if (!node) {
        LOGARG;
        return NULL;
    }

    buf[0] = '\0';
    set = llly_set_new();
    LLLY_CHECK_ERR_GOTO(!set, LOGMEM(node->module->ctx), cleanup);

    while (node) {
        llly_set_add(set, (void *)node, 0);
        do {
            node = lllys_parent(node);
        } while (node && (node->nodetype & (LLLYS_USES | LLLYS_CHOICE | LLLYS_CASE | LLLYS_INPUT | LLLYS_OUTPUT)));
    }

    prev_mod = NULL;
    used = 0;
    for (i = set->number - 1; i > -1; --i) {
        node = set->set.s[i];
        if (node->nodetype == LLLYS_EXT) {
            if (strcmp(((struct lllys_ext_instance *)node)->def->name, "yang-data")) {
                continue;
            }
            name = ((struct lllys_ext_instance *)node)->arg_value;
            separator = ":#";
        } else {
            name = node->name;
            separator = ":";
        }
        used += sprintf(buf + used, "/%s%s%s", (lllys_node_module(node) == prev_mod ? "" : lllys_node_module(node)->name),
                        (lllys_node_module(node) == prev_mod ? "" : separator), name);
        prev_mod = lllys_node_module(node);
    }

    result = strdup(buf);
    LLLY_CHECK_ERR_GOTO(!result, LOGMEM(node->module->ctx), cleanup);

cleanup:
    llly_set_free(set);
    return result;
}

struct lllys_node_augment *
lllys_getnext_target_aug(struct lllys_node_augment *last, const struct lllys_module *mod, const struct lllys_node *aug_target)
{
    struct lllys_node *child;
    struct lllys_node_augment *aug;
    int i, j, last_found;

    assert(mod && aug_target);

    if (!last) {
        last_found = 1;
    } else {
        last_found = 0;
    }

    /* search module augments */
    for (i = 0; i < mod->augment_size; ++i) {
        if (!mod->augment[i].target) {
            /* still unresolved, skip */
            continue;
        }

        if (mod->augment[i].target == aug_target) {
            if (last_found) {
                /* next match after last */
                return &mod->augment[i];
            }

            if (&mod->augment[i] == last) {
                last_found = 1;
            }
        }
    }

    /* search submodule augments */
    for (i = 0; i < mod->inc_size; ++i) {
        for (j = 0; j < mod->inc[i].submodule->augment_size; ++j) {
            if (!mod->inc[i].submodule->augment[j].target) {
                continue;
            }

            if (mod->inc[i].submodule->augment[j].target == aug_target) {
                if (last_found) {
                    /* next match after last */
                    return &mod->inc[i].submodule->augment[j];
                }

                if (&mod->inc[i].submodule->augment[j] == last) {
                    last_found = 1;
                }
            }
        }
    }

    /* we also need to check possible augments to choices */
    LLLY_TREE_FOR(aug_target->child, child) {
        if (child->nodetype == LLLYS_CHOICE) {
            aug = lllys_getnext_target_aug(last, mod, child);
            if (aug) {
                return aug;
            }
        }
    }

    return NULL;
}

API struct llly_set *
lllys_find_path(const struct lllys_module *cur_module, const struct lllys_node *cur_node, const char *path)
{
    FUN_IN;

    struct llly_set *ret;
    int rc;

    if ((!cur_module && !cur_node) || !path) {
        return NULL;
    }

    rc = resolve_schema_nodeid(path, cur_node, cur_module, &ret, 1, 1);
    if (rc == -1) {
        return NULL;
    }

    return ret;
}

static void
lllys_extcomplex_free_str(struct llly_ctx *ctx, struct lllys_ext_instance_complex *ext, LLLY_STMT stmt)
{
    struct lllyext_substmt *info;
    const char **str, ***a;
    int c;

    str = lllys_ext_complex_get_substmt(stmt, ext, &info);
    if (!str || !(*str)) {
        return;
    }
    if (info->cardinality >= LLLY_STMT_CARD_SOME) {
        /* we have array */
        a = (const char ***)str;
        for (str = (*(const char ***)str), c = 0; str[c]; c++) {
            lllydict_remove(ctx, str[c]);
        }
        free(a[0]);
        if (stmt == LLLY_STMT_BELONGSTO) {
            for (str = a[1], c = 0; str[c]; c++) {
                lllydict_remove(ctx, str[c]);
            }
            free(a[1]);
        } else if (stmt == LLLY_STMT_ARGUMENT) {
            free(a[1]);
        }
    } else {
        lllydict_remove(ctx, str[0]);
        if (stmt == LLLY_STMT_BELONGSTO) {
            lllydict_remove(ctx, str[1]);
        }
    }
}

void
lllys_extension_instances_free(struct llly_ctx *ctx, struct lllys_ext_instance **e, unsigned int size,
                             void (*private_destructor)(const struct lllys_node *node, void *priv))
{
    unsigned int i, j, k;
    struct lllyext_substmt *substmt;
    void **pp, **start;
    struct lllys_node *siter, *snext;

#define EXTCOMPLEX_FREE_STRUCT(STMT, TYPE, FUNC, FREE, ARGS...)                               \
    pp = lllys_ext_complex_get_substmt(STMT, (struct lllys_ext_instance_complex *)e[i], NULL);    \
    if (!pp || !(*pp)) { break; }                                                             \
    if (substmt[j].cardinality >= LLLY_STMT_CARD_SOME) { /* process array */                    \
        for (start = pp = *pp; *pp; pp++) {                                                   \
            FUNC(ctx, (TYPE *)(*pp), ##ARGS, private_destructor);                             \
            if (FREE) { free(*pp); }                                                          \
        }                                                                                     \
        free(start);                                                                          \
    } else { /* single item */                                                                \
        FUNC(ctx, (TYPE *)(*pp), ##ARGS, private_destructor);                                 \
        if (FREE) { free(*pp); }                                                              \
    }

    if (!size || !e) {
        return;
    }

    for (i = 0; i < size; i++) {
        if (!e[i]) {
            continue;
        }

        if (e[i]->flags & (LLLYEXT_OPT_INHERIT)) {
            /* no free, this is just a shadow copy of the original extension instance */
        } else {
            if (e[i]->flags & (LLLYEXT_OPT_YANG)) {
                free(e[i]->def);     /* remove name of instance extension */
                e[i]->def = NULL;
                yang_free_ext_data((struct yang_ext_substmt *)e[i]->parent); /* remove backup part of yang file */
            }
            /* remove private object */
            if (e[i]->priv && private_destructor) {
                private_destructor((struct lllys_node*)e[i], e[i]->priv);
            }
            lllys_extension_instances_free(ctx, e[i]->ext, e[i]->ext_size, private_destructor);
            lllydict_remove(ctx, e[i]->arg_value);
        }

        if (e[i]->def && e[i]->def->plugin && e[i]->def->plugin->type == LLLYEXT_COMPLEX
                && ((e[i]->flags & LLLYEXT_OPT_CONTENT) == 0)) {
            substmt = ((struct lllys_ext_instance_complex *)e[i])->substmt;
            for (j = 0; substmt[j].stmt; j++) {
                switch(substmt[j].stmt) {
                case LLLY_STMT_DESCRIPTION:
                case LLLY_STMT_REFERENCE:
                case LLLY_STMT_UNITS:
                case LLLY_STMT_ARGUMENT:
                case LLLY_STMT_DEFAULT:
                case LLLY_STMT_ERRTAG:
                case LLLY_STMT_ERRMSG:
                case LLLY_STMT_PREFIX:
                case LLLY_STMT_NAMESPACE:
                case LLLY_STMT_PRESENCE:
                case LLLY_STMT_REVISIONDATE:
                case LLLY_STMT_KEY:
                case LLLY_STMT_BASE:
                case LLLY_STMT_BELONGSTO:
                case LLLY_STMT_CONTACT:
                case LLLY_STMT_ORGANIZATION:
                case LLLY_STMT_PATH:
                    lllys_extcomplex_free_str(ctx, (struct lllys_ext_instance_complex *)e[i], substmt[j].stmt);
                    break;
                case LLLY_STMT_TYPE:
                    EXTCOMPLEX_FREE_STRUCT(LLLY_STMT_TYPE, struct lllys_type, lllys_type_free, 1);
                    break;
                case LLLY_STMT_TYPEDEF:
                    EXTCOMPLEX_FREE_STRUCT(LLLY_STMT_TYPEDEF, struct lllys_tpdf, lllys_tpdf_free, 1);
                    break;
                case LLLY_STMT_IFFEATURE:
                    EXTCOMPLEX_FREE_STRUCT(LLLY_STMT_IFFEATURE, struct lllys_iffeature, lllys_iffeature_free, 0, 1, 0);
                    break;
                case LLLY_STMT_MAX:
                case LLLY_STMT_MIN:
                case LLLY_STMT_POSITION:
                case LLLY_STMT_VALUE:
                    pp = (void**)&((struct lllys_ext_instance_complex *)e[i])->content[substmt[j].offset];
                    if (substmt[j].cardinality >= LLLY_STMT_CARD_SOME && *pp) {
                        for(k = 0; ((uint32_t**)(*pp))[k]; k++) {
                            free(((uint32_t**)(*pp))[k]);
                        }
                    }
                    free(*pp);
                    break;
                case LLLY_STMT_DIGITS:
                    if (substmt[j].cardinality >= LLLY_STMT_CARD_SOME) {
                        /* free the array */
                        pp = (void**)&((struct lllys_ext_instance_complex *)e[i])->content[substmt[j].offset];
                        free(*pp);
                    }
                    break;
                case LLLY_STMT_MODULE:
                    /* modules are part of the context, so they will be freed there */
                    if (substmt[j].cardinality >= LLLY_STMT_CARD_SOME) {
                        /* free the array */
                        pp = (void**)&((struct lllys_ext_instance_complex *)e[i])->content[substmt[j].offset];
                        free(*pp);
                    }
                    break;
                case LLLY_STMT_ACTION:
                case LLLY_STMT_ANYDATA:
                case LLLY_STMT_ANYXML:
                case LLLY_STMT_CASE:
                case LLLY_STMT_CHOICE:
                case LLLY_STMT_CONTAINER:
                case LLLY_STMT_GROUPING:
                case LLLY_STMT_INPUT:
                case LLLY_STMT_LEAF:
                case LLLY_STMT_LEAFLIST:
                case LLLY_STMT_LIST:
                case LLLY_STMT_NOTIFICATION:
                case LLLY_STMT_OUTPUT:
                case LLLY_STMT_RPC:
                case LLLY_STMT_USES:
                    pp = (void**)&((struct lllys_ext_instance_complex *)e[i])->content[substmt[j].offset];
                    LLLY_TREE_FOR_SAFE((struct lllys_node *)(*pp), snext, siter) {
                        lllys_node_free(siter, NULL, 0);
                    }
                    *pp = NULL;
                    break;
                case LLLY_STMT_UNIQUE:
                    pp = lllys_ext_complex_get_substmt(LLLY_STMT_UNIQUE, (struct lllys_ext_instance_complex *)e[i], NULL);
                    if (!pp || !(*pp)) {
                        break;
                    }
                    if (substmt[j].cardinality >= LLLY_STMT_CARD_SOME) { /* process array */
                        for (start = pp = *pp; *pp; pp++) {
                            for (k = 0; k < (*(struct lllys_unique**)pp)->expr_size; k++) {
                                lllydict_remove(ctx, (*(struct lllys_unique**)pp)->expr[k]);
                            }
                            free((*(struct lllys_unique**)pp)->expr);
                            free(*pp);
                        }
                        free(start);
                    } else { /* single item */
                        for (k = 0; k < (*(struct lllys_unique**)pp)->expr_size; k++) {
                            lllydict_remove(ctx, (*(struct lllys_unique**)pp)->expr[k]);
                        }
                        free((*(struct lllys_unique**)pp)->expr);
                        free(*pp);
                    }
                    break;
                case LLLY_STMT_LENGTH:
                case LLLY_STMT_MUST:
                case LLLY_STMT_PATTERN:
                case LLLY_STMT_RANGE:
                    EXTCOMPLEX_FREE_STRUCT(substmt[j].stmt, struct lllys_restr, lllys_restr_free, 1);
                    break;
                case LLLY_STMT_WHEN:
                    EXTCOMPLEX_FREE_STRUCT(LLLY_STMT_WHEN, struct lllys_when, lllys_when_free, 0);
                    break;
                case LLLY_STMT_REVISION:
                    pp = lllys_ext_complex_get_substmt(LLLY_STMT_REVISION, (struct lllys_ext_instance_complex *)e[i], NULL);
                    if (!pp || !(*pp)) {
                        break;
                    }
                    if (substmt[j].cardinality >= LLLY_STMT_CARD_SOME) { /* process array */
                        for (start = pp = *pp; *pp; pp++) {
                            lllydict_remove(ctx, (*(struct lllys_revision**)pp)->dsc);
                            lllydict_remove(ctx, (*(struct lllys_revision**)pp)->ref);
                            lllys_extension_instances_free(ctx, (*(struct lllys_revision**)pp)->ext,
                                                         (*(struct lllys_revision**)pp)->ext_size, private_destructor);
                            free(*pp);
                        }
                        free(start);
                    } else { /* single item */
                        lllydict_remove(ctx, (*(struct lllys_revision**)pp)->dsc);
                        lllydict_remove(ctx, (*(struct lllys_revision**)pp)->ref);
                        lllys_extension_instances_free(ctx, (*(struct lllys_revision**)pp)->ext,
                                                     (*(struct lllys_revision**)pp)->ext_size, private_destructor);
                        free(*pp);
                    }
                    break;
                default:
                    /* nothing to free */
                    break;
                }
            }
        }

        free(e[i]);
    }
    free(e);

#undef EXTCOMPLEX_FREE_STRUCT
}
