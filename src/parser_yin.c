/**
 * @file yin.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief YIN parser for libyang
 *
 * Copyright (c) 2015 - 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/types.h>

#include "libyang.h"
#include "common.h"
#include "context.h"
#include "hash_table.h"
#include "xpath.h"
#include "parser.h"
#include "resolve.h"
#include "tree_internal.h"
#include "xml_internal.h"

#define GETVAL(ctx, value, node, arg)                                                 \
    value = lllyxml_get_attr(node, arg, NULL);                                     \
    if (!value) {                                                                \
        LOGVAL(ctx, LLLYE_MISSARG, LLLY_VLOG_NONE, NULL, arg, node->name);                \
        goto error;                                                              \
    }

#define YIN_CHECK_ARRAY_OVERFLOW_CODE(ctx, counter, storage, name, parent, code)      \
    if ((counter) == LLLY_ARRAY_MAX(storage)) {                                    \
        LOGERR(ctx, LLLY_EINT, "Reached limit (%"PRIu64") for storing %s in %s statement.", \
               LLLY_ARRAY_MAX(storage), name, parent);                             \
        code;                                                                    \
    }

#define YIN_CHECK_ARRAY_OVERFLOW_RETURN(ctx, counter, storage, name, parent, retval)  \
    YIN_CHECK_ARRAY_OVERFLOW_CODE(ctx, counter, storage, name, parent, return retval)

#define YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, counter, storage, name, parent, target)    \
    YIN_CHECK_ARRAY_OVERFLOW_CODE(ctx, counter, storage, name, parent, goto target)

#define OPT_IDENT       0x01
#define OPT_CFG_PARSE   0x02
#define OPT_CFG_INHERIT 0x04
#define OPT_CFG_IGNORE  0x08
#define OPT_MODULE      0x10
static int read_yin_common(struct lllys_module *, struct lllys_node *, void *, LLLYEXT_PAR, struct lllyxml_elem *, int,
                           struct unres_schema *);

static struct lllys_node *read_yin_choice(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
                                        int options, struct unres_schema *unres);
static struct lllys_node *read_yin_case(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
                                      int options, struct unres_schema *unres);
static struct lllys_node *read_yin_anydata(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
                                         LLLYS_NODE type, int valid_config, struct unres_schema *unres);
static struct lllys_node *read_yin_container(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
                                           int options, struct unres_schema *unres);
static struct lllys_node *read_yin_leaf(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
                                      int valid_config, struct unres_schema *unres);
static struct lllys_node *read_yin_leaflist(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
                                          int valid_config, struct unres_schema *unres);
static struct lllys_node *read_yin_list(struct lllys_module *module,struct lllys_node *parent, struct lllyxml_elem *yin,
                                      int options, struct unres_schema *unres);
static struct lllys_node *read_yin_uses(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
                                      int options, struct unres_schema *unres);
static struct lllys_node *read_yin_grouping(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
                                          int options, struct unres_schema *unres);
static struct lllys_node *read_yin_rpc_action(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
                                            int options, struct unres_schema *unres);
static struct lllys_node *read_yin_notif(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
                                       int options, struct unres_schema *unres);
static struct lllys_when *read_yin_when(struct lllys_module *module, struct lllyxml_elem *yin, struct unres_schema *unres);

/*
 * yin - the provided XML subtree is unlinked
 * ext - pointer to the storage in the parent structure to be able to update its location after realloc
 */
int
lllyp_yin_fill_ext(void *parent, LLLYEXT_PAR parent_type, LLLYEXT_SUBSTMT substmt, uint8_t substmt_index,
             struct lllys_module *module, struct lllyxml_elem *yin, struct lllys_ext_instance ***ext,
             uint8_t ext_index, struct unres_schema *unres)
{
    struct unres_ext *info;

    info = malloc(sizeof *info);
    LLLY_CHECK_ERR_RETURN(!info, LOGMEM(module->ctx), EXIT_FAILURE);
    lllyxml_unlink(module->ctx, yin);
    info->data.yin = yin;
    info->datatype = LLLYS_IN_YIN;
    info->parent = parent;
    info->mod = module;
    info->parent_type = parent_type;
    info->substmt = substmt;
    info->substmt_index = substmt_index;
    info->ext_index = ext_index;

    if (unres_schema_add_node(module, unres, ext, UNRES_EXT, (struct lllys_node *)info) == -1) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/* logs directly */
static const char *
read_yin_subnode(struct llly_ctx *ctx, struct lllyxml_elem *node, const char *name)
{
    int len;

    /* there should be <text> child */
    if (!node->child || !node->child->name || strcmp(node->child->name, name)) {
        LOGERR(ctx, LLLY_EVALID, "Expected \"%s\" element in \"%s\" element.", name, node->name);
        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, name, node->name);
        return NULL;
    } else if (node->child->content) {
        len = strlen(node->child->content);
        return lllydict_insert(ctx, node->child->content, len);
    } else {
        return lllydict_insert(ctx, "", 0);
    }
}

int
lllyp_yin_parse_subnode_ext(struct lllys_module *mod, void *elem, LLLYEXT_PAR elem_type,
                     struct lllyxml_elem *yin, LLLYEXT_SUBSTMT type, uint8_t i, struct unres_schema *unres)
{
    void *reallocated;
    struct lllyxml_elem *next, *child;
    int r;
    struct lllys_ext_instance ***ext;
    uint8_t *ext_size;
    const char *statement;

    switch (elem_type) {
    case LLLYEXT_PAR_MODULE:
        ext_size = &((struct lllys_module *)elem)->ext_size;
        ext = &((struct lllys_module *)elem)->ext;
        statement = ((struct lllys_module *)elem)->type ? "submodule" : "module";
        break;
    case LLLYEXT_PAR_IMPORT:
        ext_size = &((struct lllys_import *)elem)->ext_size;
        ext = &((struct lllys_import *)elem)->ext;
        statement = "import";
        break;
    case LLLYEXT_PAR_INCLUDE:
        ext_size = &((struct lllys_include *)elem)->ext_size;
        ext = &((struct lllys_include *)elem)->ext;
        statement = "include";
        break;
    case LLLYEXT_PAR_REVISION:
        ext_size = &((struct lllys_revision *)elem)->ext_size;
        ext = &((struct lllys_revision *)elem)->ext;
        statement = "revision";
        break;
    case LLLYEXT_PAR_NODE:
        ext_size = &((struct lllys_node *)elem)->ext_size;
        ext = &((struct lllys_node *)elem)->ext;
        statement = strnodetype(((struct lllys_node *)elem)->nodetype);
        break;
    case LLLYEXT_PAR_IDENT:
        ext_size = &((struct lllys_ident *)elem)->ext_size;
        ext = &((struct lllys_ident *)elem)->ext;
        statement = "identity";
        break;
    case LLLYEXT_PAR_TYPE:
        ext_size = &((struct lllys_type *)elem)->ext_size;
        ext = &((struct lllys_type *)elem)->ext;
        statement = "type";
        break;
    case LLLYEXT_PAR_TYPE_BIT:
        ext_size = &((struct lllys_type_bit *)elem)->ext_size;
        ext = &((struct lllys_type_bit *)elem)->ext;
        statement = "bit";
        break;
    case LLLYEXT_PAR_TYPE_ENUM:
        ext_size = &((struct lllys_type_enum *)elem)->ext_size;
        ext = &((struct lllys_type_enum *)elem)->ext;
        statement = "enum";
        break;
    case LLLYEXT_PAR_TPDF:
        ext_size = &((struct lllys_tpdf *)elem)->ext_size;
        ext = &((struct lllys_tpdf *)elem)->ext;
        statement = " typedef";
        break;
    case LLLYEXT_PAR_EXT:
        ext_size = &((struct lllys_ext *)elem)->ext_size;
        ext = &((struct lllys_ext *)elem)->ext;
        statement = "extension";
        break;
    case LLLYEXT_PAR_EXTINST:
        ext_size = &((struct lllys_ext_instance *)elem)->ext_size;
        ext = &((struct lllys_ext_instance *)elem)->ext;
        statement = "extension instance";
        break;
    case LLLYEXT_PAR_FEATURE:
        ext_size = &((struct lllys_feature *)elem)->ext_size;
        ext = &((struct lllys_feature *)elem)->ext;
        statement = "feature";
        break;
    case LLLYEXT_PAR_REFINE:
        ext_size = &((struct lllys_refine *)elem)->ext_size;
        ext = &((struct lllys_refine *)elem)->ext;
        statement = "refine";
        break;
    case LLLYEXT_PAR_RESTR:
        ext_size = &((struct lllys_restr *)elem)->ext_size;
        ext = &((struct lllys_restr *)elem)->ext;
        statement = "YANG restriction";
        break;
    case LLLYEXT_PAR_WHEN:
        ext_size = &((struct lllys_when *)elem)->ext_size;
        ext = &((struct lllys_when *)elem)->ext;
        statement = "when";
        break;
    case LLLYEXT_PAR_DEVIATE:
        ext_size = &((struct lllys_deviate *)elem)->ext_size;
        ext = &((struct lllys_deviate *)elem)->ext;
        statement = "deviate";
        break;
    case LLLYEXT_PAR_DEVIATION:
        ext_size = &((struct lllys_deviation *)elem)->ext_size;
        ext = &((struct lllys_deviation *)elem)->ext;
        statement = "deviation";
        break;
    default:
        LOGERR(mod->ctx, LLLY_EINT, "parent type %d", elem_type);
        return EXIT_FAILURE;
    }

    if (type == LLLYEXT_SUBSTMT_SELF) {
        /* parse for the statement self, not for the substatement */
        child = yin;
        next = NULL;
        goto parseext;
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, child) {
        if (!child->ns) {
            LOGVAL(mod->ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Extension instance \"%s\" is missing namespace.", child->name);
            return EXIT_FAILURE;
        }
        if (!strcmp(child->ns->value, LLLY_NSYIN)) {
            /* skip the regular YIN nodes */
            continue;
        }

        /* parse it as extension */
parseext:

        YIN_CHECK_ARRAY_OVERFLOW_RETURN(mod->ctx, *ext_size, *ext_size, "extension", statement, EXIT_FAILURE);
        /* first, allocate a space for the extension instance in the parent elem */
        reallocated = realloc(*ext, (1 + (*ext_size)) * sizeof **ext);
        LLLY_CHECK_ERR_RETURN(!reallocated, LOGMEM(mod->ctx), EXIT_FAILURE);
        (*ext) = reallocated;

        /* init memory */
        (*ext)[(*ext_size)] = NULL;

        /* parse YIN data */
        r = lllyp_yin_fill_ext(elem, elem_type, type, i, mod, child, &(*ext), (*ext_size), unres);
        (*ext_size)++;
        if (r) {
            return EXIT_FAILURE;
        }

        /* done - do not free the child, it is unlinked in lllyp_yin_fill_ext */
    }

    return EXIT_SUCCESS;
}

/* logs directly */
static int
fill_yin_iffeature(struct lllys_node *parent, int parent_is_feature, struct lllyxml_elem *yin, struct lllys_iffeature *iffeat,
                   struct unres_schema *unres)
{
    int r, c_ext = 0;
    const char *value;
    struct lllyxml_elem *node, *next;
    struct llly_ctx *ctx = parent->module->ctx;

    GETVAL(ctx, value, yin, "name");

    if ((lllys_node_module(parent)->version != 2) && ((value[0] == '(') || strchr(value, ' '))) {
        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "if-feature");
error:
        return EXIT_FAILURE;
    }

    if (!(value = transform_iffeat_schema2json(parent->module, value))) {
        return EXIT_FAILURE;
    }

    r = resolve_iffeature_compile(iffeat, value, parent, parent_is_feature, unres);
    lllydict_remove(ctx, value);
    if (r) {
        return EXIT_FAILURE;
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, node) {
        if (!node->ns) {
            /* garbage */
            lllyxml_free(ctx, node);
        } else if (strcmp(node->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_RETURN(ctx, c_ext, iffeat->ext_size, "extensions", "if-feature", EXIT_FAILURE);
            c_ext++;
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name, "if-feature");
            return EXIT_FAILURE;
        }
    }
    if (c_ext) {
        iffeat->ext = calloc(c_ext, sizeof *iffeat->ext);
        LLLY_CHECK_ERR_RETURN(!iffeat->ext, LOGMEM(ctx), EXIT_FAILURE);
        LLLY_TREE_FOR_SAFE(yin->child, next, node) {
            /* extensions */
            r = lllyp_yin_fill_ext(iffeat, LLLYEXT_PAR_IDENT, 0, 0, parent->module, node,
                                 &iffeat->ext, iffeat->ext_size, unres);
            iffeat->ext_size++;
            if (r) {
                return EXIT_FAILURE;
            }
        }
    }

    return EXIT_SUCCESS;
}

/* logs directly */
static int
fill_yin_identity(struct lllys_module *module, struct lllyxml_elem *yin, struct lllys_ident *ident, struct unres_schema *unres)
{
    struct lllyxml_elem *node, *next;
    struct llly_ctx *ctx = module->ctx;
    const char *value;
    int rc;
    int c_ftrs = 0, c_base = 0, c_ext = 0;
    void *reallocated;

    GETVAL(ctx, value, yin, "name");
    ident->name = value;

    if (read_yin_common(module, NULL, ident, LLLYEXT_PAR_IDENT, yin, OPT_IDENT | OPT_MODULE, unres)) {
        goto error;
    }

    if (dup_identities_check(ident->name, module)) {
        goto error;
    }

    LLLY_TREE_FOR(yin->child, node) {
        if (strcmp(node->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, ident->ext_size, "extensions", "identity", error);
            c_ext++;
        } else if (!strcmp(node->name, "base")) {
            if (c_base && (module->version < 2)) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, "base", "identity");
                goto error;
            }
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_base, ident->base_size, "bases", "identity", error);
            if (lllyp_yin_parse_subnode_ext(module, ident, LLLYEXT_PAR_IDENT, node, LLLYEXT_SUBSTMT_BASE, c_base, unres)) {
                goto error;
            }
            c_base++;

        } else if ((module->version >= 2) && !strcmp(node->name, "if-feature")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, ident->iffeature_size, "if-features", "identity", error);
            c_ftrs++;

        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name, "identity");
            goto error;
        }
    }

    if (c_base) {
        ident->base_size = 0;
        ident->base = calloc(c_base, sizeof *ident->base);
        LLLY_CHECK_ERR_GOTO(!ident->base, LOGMEM(ctx), error);
    }
    if (c_ftrs) {
        ident->iffeature = calloc(c_ftrs, sizeof *ident->iffeature);
        LLLY_CHECK_ERR_GOTO(!ident->iffeature, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(ident->ext, (c_ext + ident->ext_size) * sizeof *ident->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        ident->ext = reallocated;

        /* init memory */
        memset(&ident->ext[ident->ext_size], 0, c_ext * sizeof *ident->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, node) {
        if (strcmp(node->ns->value, LLLY_NSYIN)) {
            /* extension */
            rc = lllyp_yin_fill_ext(ident, LLLYEXT_PAR_IDENT, 0, 0, module, node, &ident->ext, ident->ext_size, unres);
            ident->ext_size++;
            if (rc) {
                goto error;
            }
        } else if (!strcmp(node->name, "base")) {
            GETVAL(ctx, value, node, "name");
            value = transform_schema2json(module, value);
            if (!value) {
                goto error;
            }

            if (unres_schema_add_str(module, unres, ident, UNRES_IDENT, value) == -1) {
                lllydict_remove(ctx, value);
                goto error;
            }
            lllydict_remove(ctx, value);
        } else if (!strcmp(node->name, "if-feature")) {
            rc = fill_yin_iffeature((struct lllys_node *)ident, 0, node, &ident->iffeature[ident->iffeature_size], unres);
            ident->iffeature_size++;
            if (rc) {
                goto error;
            }
        }
    }

    return EXIT_SUCCESS;

error:
    return EXIT_FAILURE;
}

/* logs directly */
static int
read_restr_substmt(struct lllys_module *module, struct lllys_restr *restr, struct lllyxml_elem *yin,
                   struct unres_schema *unres)
{
    struct lllyxml_elem *child, *next;
    const char *value;
    struct llly_ctx *ctx = module->ctx;

    LLLY_TREE_FOR_SAFE(yin->child, next, child) {
        if (!child->ns) {
            /* garbage */
            continue;
        } else if (strcmp(child->ns->value, LLLY_NSYIN)) {
            /* extension */
            if (lllyp_yin_parse_subnode_ext(module, restr, LLLYEXT_PAR_RESTR, child, LLLYEXT_SUBSTMT_SELF, 0, unres)) {
                return EXIT_FAILURE;
            }
        } else if (!strcmp(child->name, "description")) {
            if (restr->dsc) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                return EXIT_FAILURE;
            }
            if (lllyp_yin_parse_subnode_ext(module, restr, LLLYEXT_PAR_RESTR, child, LLLYEXT_SUBSTMT_DESCRIPTION, 0, unres)) {
                return EXIT_FAILURE;
            }
            restr->dsc = read_yin_subnode(ctx, child, "text");
            if (!restr->dsc) {
                return EXIT_FAILURE;
            }
        } else if (!strcmp(child->name, "reference")) {
            if (restr->ref) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                return EXIT_FAILURE;
            }
            if (lllyp_yin_parse_subnode_ext(module, restr, LLLYEXT_PAR_RESTR, child, LLLYEXT_SUBSTMT_REFERENCE, 0, unres)) {
                return EXIT_FAILURE;
            }
            restr->ref = read_yin_subnode(ctx, child, "text");
            if (!restr->ref) {
                return EXIT_FAILURE;
            }
        } else if (!strcmp(child->name, "error-app-tag")) {
            if (restr->eapptag) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                return EXIT_FAILURE;
            }
            if (lllyp_yin_parse_subnode_ext(module, restr, LLLYEXT_PAR_RESTR, child, LLLYEXT_SUBSTMT_ERRTAG, 0, unres)) {
                return EXIT_FAILURE;
            }
            GETVAL(ctx, value, child, "value");
            restr->eapptag = lllydict_insert(ctx, value, 0);
        } else if (!strcmp(child->name, "error-message")) {
            if (restr->emsg) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                return EXIT_FAILURE;
            }
            if (lllyp_yin_parse_subnode_ext(module, restr, LLLYEXT_PAR_RESTR, child, LLLYEXT_SUBSTMT_ERRMSG, 0, unres)) {
                return EXIT_FAILURE;
            }
            restr->emsg = read_yin_subnode(ctx, child, "value");
            if (!restr->emsg) {
                return EXIT_FAILURE;
            }
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;

error:
    return EXIT_FAILURE;
}

/* logs directly, returns EXIT_SUCCESS, EXIT_FAILURE, -1 */
int
fill_yin_type(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin, struct lllys_type *type,
              int parenttype, struct unres_schema *unres)
{
    const char *value, *name, *module_name = NULL;
    struct lllys_node *siter;
    struct lllyxml_elem *next, *next2, *node, *child, exts;
    struct lllys_restr **restrs, *restr;
    struct lllys_type_bit bit, *bits_sc = NULL;
    struct lllys_type_enum *enms_sc = NULL; /* shortcut */
    struct lllys_type *dertype;
    struct llly_ctx *ctx = module->ctx;
    int rc, val_set, c_ftrs, c_ext = 0;
    unsigned int i, j;
    int ret = -1;
    int64_t v, v_;
    int64_t p, p_;
    size_t len;
    int in_grp = 0;
    char *buf, modifier;

    /* init */
    memset(&exts, 0, sizeof exts);

    GETVAL(ctx, value, yin, "name");
    value = transform_schema2json(module, value);
    if (!value) {
        goto error;
    }

    i = parse_identifier(value);
    if (i < 1) {
        LOGVAL(ctx, LLLYE_INCHAR, LLLY_VLOG_NONE, NULL, value[-i], &value[-i]);
        lllydict_remove(ctx, value);
        goto error;
    }
    /* module name */
    name = value;
    if (value[i]) {
        module_name = lllydict_insert(ctx, value, i);
        name += i;
        if ((name[0] != ':') || (parse_identifier(name + 1) < 1)) {
            LOGVAL(ctx, LLLYE_INCHAR, LLLY_VLOG_NONE, NULL, name[0], name);
            lllydict_remove(ctx, module_name);
            lllydict_remove(ctx, value);
            goto error;
        }
        /* name is in dictionary, but moved */
        ++name;
    }

    rc = resolve_superior_type(name, module_name, module, parent, &type->der);
    if (rc == -1) {
        LOGVAL(ctx, LLLYE_INMOD, LLLY_VLOG_NONE, NULL, module_name);
        lllydict_remove(ctx, module_name);
        lllydict_remove(ctx, value);
        goto error;

    /* the type could not be resolved or it was resolved to an unresolved typedef */
    } else if (rc == EXIT_FAILURE) {
        LOGVAL(ctx, LLLYE_NORESOLV, LLLY_VLOG_NONE, NULL, "type", name);
        lllydict_remove(ctx, module_name);
        lllydict_remove(ctx, value);
        ret = EXIT_FAILURE;
        goto error;
    }
    lllydict_remove(ctx, module_name);
    lllydict_remove(ctx, value);

    if (type->value_flags & LLLY_VALUE_UNRESGRP) {
        /* resolved type in grouping, decrease the grouping's nacm number to indicate that one less
         * unresolved item left inside the grouping, LLLYTYPE_GRP used as a flag for types inside a grouping. */
        for (siter = parent; siter && (siter->nodetype != LLLYS_GROUPING); siter = lllys_parent(siter));
        if (siter) {
            assert(((struct lllys_node_grp *)siter)->unres_count);
            ((struct lllys_node_grp *)siter)->unres_count--;
        } else {
            LOGINT(ctx);
            goto error;
        }
        type->value_flags &= ~LLLY_VALUE_UNRESGRP;
    }
    type->base = type->der->type.base;

    /* check status */
    if (lllyp_check_status(type->parent->flags, type->parent->module, type->parent->name,
                         type->der->flags, type->der->module, type->der->name,  parent)) {
        return -1;
    }

    /* parse extension instances */
    LLLY_TREE_FOR_SAFE(yin->child, next, node) {
        if (!node->ns) {
            /* garbage */
            lllyxml_free(ctx, node);
            continue;
        } else if (!strcmp(node->ns->value, LLLY_NSYIN)) {
            /* YANG (YIN) statements - process later */
            continue;
        }

        YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, type->ext_size, "extensions", "type", error);

        lllyxml_unlink_elem(ctx, node, 2);
        lllyxml_add_child(ctx, &exts, node);
        c_ext++;
    }
    if (c_ext) {
        type->ext = calloc(c_ext, sizeof *type->ext);
        LLLY_CHECK_ERR_GOTO(!type->ext, LOGMEM(ctx), error);

        LLLY_TREE_FOR_SAFE(exts.child, next, node) {
            rc = lllyp_yin_fill_ext(type, LLLYEXT_PAR_TYPE, 0, 0, module, node, &type->ext, type->ext_size, unres);
            type->ext_size++;
            if (rc) {
                goto error;
            }
        }
    }

    switch (type->base) {
    case LLLY_TYPE_BITS:
        /* RFC 6020 9.7.4 - bit */

        /* get bit specifications, at least one must be present */
        LLLY_TREE_FOR_SAFE(yin->child, next, node) {
            if (!strcmp(node->name, "bit")) {
                YIN_CHECK_ARRAY_OVERFLOW_CODE(ctx, type->info.bits.count, type->info.bits.count, "bits", "type",
                                              type->info.bits.count = 0; goto error);
                type->info.bits.count++;
            } else {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name);
                type->info.bits.count = 0;
                goto error;
            }
        }
        dertype = &type->der->type;
        if (!dertype->der) {
            if (!type->info.bits.count) {
                /* type is derived directly from buit-in bits type and bit statement is required */
                LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "bit", "type");
                goto error;
            }
        } else {
            for (; !dertype->info.enums.count; dertype = &dertype->der->type);
            if (module->version < 2 && type->info.bits.count) {
                /* type is not directly derived from buit-in bits type and bit statement is prohibited,
                 * since YANG 1.1 the bit statements can be used to restrict the base bits type */
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, "bit");
                type->info.bits.count = 0;
                goto error;
            }
        }

        type->info.bits.bit = calloc(type->info.bits.count, sizeof *type->info.bits.bit);
        LLLY_CHECK_ERR_GOTO(!type->info.bits.bit, LOGMEM(ctx), error);

        p = 0;
        i = 0;
        LLLY_TREE_FOR(yin->child, next) {
            c_ftrs = 0;

            GETVAL(ctx, value, next, "name");
            if (lllyp_check_identifier(ctx, value, LLLY_IDENT_SIMPLE, NULL, NULL)) {
                goto error;
            }

            type->info.bits.bit[i].name = lllydict_insert(ctx, value, strlen(value));
            if (read_yin_common(module, NULL, &type->info.bits.bit[i], LLLYEXT_PAR_TYPE_BIT, next, 0, unres)) {
                type->info.bits.count = i + 1;
                goto error;
            }

            if (!dertype->der) { /* directly derived type from bits built-in type */
                /* check the name uniqueness */
                for (j = 0; j < i; j++) {
                    if (!strcmp(type->info.bits.bit[j].name, type->info.bits.bit[i].name)) {
                        LOGVAL(ctx, LLLYE_BITS_DUPNAME, LLLY_VLOG_NONE, NULL, type->info.bits.bit[i].name);
                        type->info.bits.count = i + 1;
                        goto error;
                    }
                }
            } else {
                /* restricted bits type - the name MUST be used in the base type */
                bits_sc = dertype->info.bits.bit;
                for (j = 0; j < dertype->info.bits.count; j++) {
                    if (llly_strequal(bits_sc[j].name, value, 1)) {
                        break;
                    }
                }
                if (j == dertype->info.bits.count) {
                    LOGVAL(ctx, LLLYE_BITS_INNAME, LLLY_VLOG_NONE, NULL, value);
                    type->info.bits.count = i + 1;
                    goto error;
                }
            }


            p_ = -1;
            LLLY_TREE_FOR_SAFE(next->child, next2, node) {
                if (!node->ns) {
                    /* garbage */
                    continue;
                } else if (strcmp(node->ns->value, LLLY_NSYIN)) {
                    /* extension */
                    if (lllyp_yin_parse_subnode_ext(module, &type->info.bits.bit[i], LLLYEXT_PAR_TYPE_BIT, node,
                                                  LLLYEXT_SUBSTMT_SELF, 0, unres)) {
                        goto error;
                    }
                } else if (!strcmp(node->name, "position")) {
                    if (p_ != -1) {
                        LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, next->name);
                        type->info.bits.count = i + 1;
                        goto error;
                    }

                    GETVAL(ctx, value, node, "value");
                    p_ = strtoll(value, NULL, 10);

                    /* range check */
                    if (p_ < 0 || p_ > UINT32_MAX) {
                        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "bit/position");
                        type->info.bits.count = i + 1;
                        goto error;
                    }
                    type->info.bits.bit[i].pos = (uint32_t)p_;

                    if (!dertype->der) { /* directly derived type from bits built-in type */
                        /* keep the highest enum value for automatic increment */
                        if (type->info.bits.bit[i].pos >= p) {
                            p = type->info.bits.bit[i].pos;
                            p++;
                        } else {
                            /* check that the value is unique */
                            for (j = 0; j < i; j++) {
                                if (type->info.bits.bit[j].pos == type->info.bits.bit[i].pos) {
                                    LOGVAL(ctx, LLLYE_BITS_DUPVAL, LLLY_VLOG_NONE, NULL,
                                           type->info.bits.bit[i].pos, type->info.bits.bit[i].name,
                                           type->info.bits.bit[j].name);
                                    type->info.bits.count = i + 1;
                                    goto error;
                                }
                            }
                        }
                    }

                    if (lllyp_yin_parse_subnode_ext(module, &type->info.bits.bit[i], LLLYEXT_PAR_TYPE_BIT, node,
                                             LLLYEXT_SUBSTMT_POSITION, 0, unres)) {
                        goto error;
                    }

                    for (j = 0; j < type->info.bits.bit[i].ext_size; ++j) {
                        /* set flag, which represent LLLYEXT_OPT_VALID */
                        if (type->info.bits.bit[i].ext[j]->flags & LLLYEXT_OPT_VALID) {
                            type->parent->flags |= LLLYS_VALID_EXT;
                            break;
                        }
                    }

                } else if ((module->version >= 2) && !strcmp(node->name, "if-feature")) {
                    YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, type->info.bits.bit[i].iffeature_size, "if-features", "bit", error);
                    c_ftrs++;
                } else {
                    LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name);
                    goto error;
                }
            }

            if (!dertype->der) { /* directly derived type from bits built-in type */
                if (p_ == -1) {
                    /* assign value automatically */
                    if (p > UINT32_MAX) {
                        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, "4294967295", "bit/position");
                        type->info.bits.count = i + 1;
                        goto error;
                    }
                    type->info.bits.bit[i].pos = (uint32_t)p;
                    type->info.bits.bit[i].flags |= LLLYS_AUTOASSIGNED;
                    p++;
                }
            } else { /* restricted bits type */
                if (p_ == -1) {
                    /* automatically assign position from base type */
                    type->info.bits.bit[i].pos = bits_sc[j].pos;
                    type->info.bits.bit[i].flags |= LLLYS_AUTOASSIGNED;
                } else {
                    /* check that the assigned position corresponds to the original
                     * position of the bit in the base type */
                    if (p_ != bits_sc[j].pos) {
                        /* p_ - assigned position in restricted bits
                         * bits_sc[j].pos - position assigned to the corresponding bit (detected above) in base type */
                        LOGVAL(ctx, LLLYE_BITS_INVAL, LLLY_VLOG_NONE, NULL, type->info.bits.bit[i].pos,
                               type->info.bits.bit[i].name, bits_sc[j].pos);
                        type->info.bits.count = i + 1;
                        goto error;
                    }
                }
            }

            /* if-features */
            if (c_ftrs) {
                bits_sc = &type->info.bits.bit[i];
                bits_sc->iffeature = calloc(c_ftrs, sizeof *bits_sc->iffeature);
                if (!bits_sc->iffeature) {
                    LOGMEM(ctx);
                    type->info.bits.count = i + 1;
                    goto error;
                }

                LLLY_TREE_FOR(next->child, node) {
                    if (!strcmp(node->name, "if-feature")) {
                        rc = fill_yin_iffeature((struct lllys_node *)type->parent, 0, node,
                                                &bits_sc->iffeature[bits_sc->iffeature_size], unres);
                        bits_sc->iffeature_size++;
                        if (rc) {
                            type->info.bits.count = i + 1;
                            goto error;
                        }
                    }
                }
            }

            /* keep them ordered by position */
            j = i;
            while (j && type->info.bits.bit[j - 1].pos > type->info.bits.bit[j].pos) {
                /* switch them */
                memcpy(&bit, &type->info.bits.bit[j], sizeof bit);
                memcpy(&type->info.bits.bit[j], &type->info.bits.bit[j - 1], sizeof bit);
                memcpy(&type->info.bits.bit[j - 1], &bit, sizeof bit);
                j--;
            }

            ++i;
        }
        break;

    case LLLY_TYPE_DEC64:
        /* RFC 6020 9.2.4 - range and 9.3.4 - fraction-digits */
        LLLY_TREE_FOR(yin->child, node) {

            if (!strcmp(node->name, "range")) {
                if (type->info.dec64.range) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, yin->name);
                    goto error;
                }

                GETVAL(ctx, value, node, "value");
                type->info.dec64.range = calloc(1, sizeof *type->info.dec64.range);
                LLLY_CHECK_ERR_GOTO(!type->info.dec64.range, LOGMEM(ctx), error);
                type->info.dec64.range->expr = lllydict_insert(ctx, value, 0);

                /* get possible substatements */
                if (read_restr_substmt(module, type->info.dec64.range, node, unres)) {
                    goto error;
                }
                for (j = 0; j < type->info.dec64.range->ext_size; ++j) {
                    /* set flag, which represent LLLYEXT_OPT_VALID */
                    if (type->info.dec64.range->ext[j]->flags & LLLYEXT_OPT_VALID) {
                        type->parent->flags |= LLLYS_VALID_EXT;
                        break;
                    }
                }
            } else if (!strcmp(node->name, "fraction-digits")) {
                if (type->info.dec64.dig) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, yin->name);
                    goto error;
                }
                GETVAL(ctx, value, node, "value");
                v = strtol(value, NULL, 10);

                /* range check */
                if (v < 1 || v > 18) {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, node->name);
                    goto error;
                }
                type->info.dec64.dig = (uint8_t)v;
                type->info.dec64.div = 10;
                for (i = 1; i < v; i++) {
                    type->info.dec64.div *= 10;
                }

                /* extensions */
                if (lllyp_yin_parse_subnode_ext(module, type, LLLYEXT_PAR_TYPE, node, LLLYEXT_SUBSTMT_DIGITS, 0, unres)) {
                    goto error;
                }
            } else {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name);
                goto error;
            }
        }

        /* mandatory sub-statement(s) check */
        if (!type->info.dec64.dig && !type->der->type.der) {
            /* decimal64 type directly derived from built-in type requires fraction-digits */
            LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "fraction-digits", "type");
            goto error;
        }
        if (type->info.dec64.dig && type->der->type.der) {
            /* type is not directly derived from buit-in type and fraction-digits statement is prohibited */
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, "fraction-digits");
            goto error;
        }

        /* copy fraction-digits specification from parent type for easier internal use */
        if (type->der->type.der) {
            type->info.dec64.dig = type->der->type.info.dec64.dig;
            type->info.dec64.div = type->der->type.info.dec64.div;
        }

        if (type->info.dec64.range && lllyp_check_length_range(ctx, type->info.dec64.range->expr, type)) {
            LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "range");
            goto error;
        }
        break;

    case LLLY_TYPE_ENUM:
        /* RFC 6020 9.6 - enum */

        /* get enum specifications, at least one must be present */
        LLLY_TREE_FOR_SAFE(yin->child, next, node) {

            if (!strcmp(node->name, "enum")) {
                YIN_CHECK_ARRAY_OVERFLOW_CODE(ctx, type->info.enums.count, type->info.enums.count, "enums", "type",
                                              type->info.enums.count = 0; goto error);
                type->info.enums.count++;
            } else {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name);
                type->info.enums.count = 0;
                goto error;
            }
        }
        dertype = &type->der->type;
        if (!dertype->der) {
            if (!type->info.enums.count) {
                /* type is derived directly from buit-in enumeartion type and enum statement is required */
                LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "enum", "type");
                goto error;
            }
        } else {
            for (; !dertype->info.enums.count; dertype = &dertype->der->type);
            if (module->version < 2 && type->info.enums.count) {
                /* type is not directly derived from built-in enumeration type and enum statement is prohibited
                 * in YANG 1.0, since YANG 1.1 enum statements can be used to restrict the base enumeration type */
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, "enum");
                type->info.enums.count = 0;
                goto error;
            }
        }

        type->info.enums.enm = calloc(type->info.enums.count, sizeof *type->info.enums.enm);
        LLLY_CHECK_ERR_GOTO(!type->info.enums.enm, LOGMEM(ctx), error);

        v = 0;
        i = 0;
        LLLY_TREE_FOR(yin->child, next) {
            c_ftrs = 0;

            GETVAL(ctx, value, next, "name");
            if (!value[0]) {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "enum name");
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Enum name must not be empty.");
                goto error;
            }
            type->info.enums.enm[i].name = lllydict_insert(ctx, value, strlen(value));
            if (read_yin_common(module, NULL, &type->info.enums.enm[i], LLLYEXT_PAR_TYPE_ENUM, next, 0, unres)) {
                type->info.enums.count = i + 1;
                goto error;
            }

            /* the assigned name MUST NOT have any leading or trailing whitespace characters */
            value = type->info.enums.enm[i].name;
            if (isspace(value[0]) || isspace(value[strlen(value) - 1])) {
                LOGVAL(ctx, LLLYE_ENUM_WS, LLLY_VLOG_NONE, NULL, value);
                type->info.enums.count = i + 1;
                goto error;
            }

            if (!dertype->der) { /* directly derived type from enumeration built-in type */
                /* check the name uniqueness */
                for (j = 0; j < i; j++) {
                    if (llly_strequal(type->info.enums.enm[j].name, value, 1)) {
                        LOGVAL(ctx, LLLYE_ENUM_DUPNAME, LLLY_VLOG_NONE, NULL, value);
                        type->info.enums.count = i + 1;
                        goto error;
                    }
                }
            } else {
                /* restricted enumeration type - the name MUST be used in the base type */
                enms_sc = dertype->info.enums.enm;
                for (j = 0; j < dertype->info.enums.count; j++) {
                    if (llly_strequal(enms_sc[j].name, value, 1)) {
                        break;
                    }
                }
                if (j == dertype->info.enums.count) {
                    LOGVAL(ctx, LLLYE_ENUM_INNAME, LLLY_VLOG_NONE, NULL, value);
                    type->info.enums.count = i + 1;
                    goto error;
                }
            }

            val_set = 0;
            LLLY_TREE_FOR_SAFE(next->child, next2, node) {
                if (!node->ns) {
                    /* garbage */
                    continue;
                } else if (strcmp(node->ns->value, LLLY_NSYIN)) {
                    /* extensions */
                    if (lllyp_yin_parse_subnode_ext(module, &type->info.enums.enm[i], LLLYEXT_PAR_TYPE_ENUM, node,
                                             LLLYEXT_SUBSTMT_SELF, 0, unres)) {
                        goto error;
                    }
                } else if (!strcmp(node->name, "value")) {
                    if (val_set) {
                        LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, next->name);
                        type->info.enums.count = i + 1;
                        goto error;
                    }

                    GETVAL(ctx, value, node, "value");
                    v_ = strtoll(value, NULL, 10);

                    /* range check */
                    if (v_ < INT32_MIN || v_ > INT32_MAX) {
                        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "enum/value");
                        type->info.enums.count = i + 1;
                        goto error;
                    }
                    type->info.enums.enm[i].value = v_;

                    if (!dertype->der) { /* directly derived type from enumeration built-in type */
                        if (!i) {
                            /* change value, which is assigned automatically, if first enum has value. */
                            v = type->info.enums.enm[i].value;
                            v++;
                        } else {
                            /* keep the highest enum value for automatic increment */
                            if (type->info.enums.enm[i].value >= v) {
                                v = type->info.enums.enm[i].value;
                                v++;
                            } else {
                                /* check that the value is unique */
                                for (j = 0; j < i; j++) {
                                    if (type->info.enums.enm[j].value == type->info.enums.enm[i].value) {
                                        LOGVAL(ctx, LLLYE_ENUM_DUPVAL, LLLY_VLOG_NONE, NULL,
                                               type->info.enums.enm[i].value, type->info.enums.enm[i].name,
                                               type->info.enums.enm[j].name);
                                        type->info.enums.count = i + 1;
                                        goto error;
                                    }
                                }
                            }
                        }
                    }
                    val_set = 1;

                    if (lllyp_yin_parse_subnode_ext(module, &type->info.enums.enm[i], LLLYEXT_PAR_TYPE_ENUM, node,
                                             LLLYEXT_SUBSTMT_VALUE, 0, unres)) {
                        goto error;
                    }

                    for (j = 0; j < type->info.enums.enm[i].ext_size; ++j) {
                        /* set flag, which represent LLLYEXT_OPT_VALID */
                        if (type->info.enums.enm[i].ext[j]->flags & LLLYEXT_OPT_VALID) {
                            type->parent->flags |= LLLYS_VALID_EXT;
                            break;
                        }
                    }
                } else if ((module->version >= 2) && !strcmp(node->name, "if-feature")) {
                    YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, type->info.enums.enm[i].iffeature_size, "if-features", "enum", error);
                    c_ftrs++;

                } else {
                    LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name);
                    goto error;
                }
            }

            if (!dertype->der) { /* directly derived type from enumeration */
                if (!val_set) {
                    /* assign value automatically */
                    if (v > INT32_MAX) {
                        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, "2147483648", "enum/value");
                        type->info.enums.count = i + 1;
                        goto error;
                    }
                    type->info.enums.enm[i].value = v;
                    type->info.enums.enm[i].flags |= LLLYS_AUTOASSIGNED;
                    v++;
                }
            } else { /* restricted enum type */
                if (!val_set) {
                    /* automatically assign value from base type */
                    type->info.enums.enm[i].value = enms_sc[j].value;
                    type->info.enums.enm[i].flags |= LLLYS_AUTOASSIGNED;
                } else {
                    /* check that the assigned value corresponds to the original
                     * value of the enum in the base type */
                    if (v_ != enms_sc[j].value) {
                        /* v_ - assigned value in restricted enum
                         * enms_sc[j].value - value assigned to the corresponding enum (detected above) in base type */
                        LOGVAL(ctx, LLLYE_ENUM_INVAL, LLLY_VLOG_NONE, NULL,
                               type->info.enums.enm[i].value, type->info.enums.enm[i].name, enms_sc[j].value);
                        type->info.enums.count = i + 1;
                        goto error;
                    }
                }
            }

            /* if-features */
            if (c_ftrs) {
                enms_sc = &type->info.enums.enm[i];
                enms_sc->iffeature = calloc(c_ftrs, sizeof *enms_sc->iffeature);
                if (!enms_sc->iffeature) {
                    LOGMEM(ctx);
                    type->info.enums.count = i + 1;
                    goto error;
                }

                LLLY_TREE_FOR(next->child, node) {
                    if (!strcmp(node->name, "if-feature")) {
                        rc = fill_yin_iffeature((struct lllys_node *)type->parent, 0, node,
                                                &enms_sc->iffeature[enms_sc->iffeature_size], unres);
                        enms_sc->iffeature_size++;
                        if (rc) {
                            type->info.enums.count = i + 1;
                            goto error;
                        }
                    }
                }
            }

            ++i;
        }
        break;

    case LLLY_TYPE_IDENT:
        /* RFC 6020 9.10 - base */

        /* get base specification, at least one must be present */
        LLLY_TREE_FOR_SAFE(yin->child, next, node) {

            if (strcmp(node->name, "base")) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name);
                goto error;
            }

            GETVAL(ctx, value, yin->child, "name");
            /* store in the JSON format */
            value = transform_schema2json(module, value);
            if (!value) {
                goto error;
            }
            rc = unres_schema_add_str(module, unres, type, UNRES_TYPE_IDENTREF, value);
            lllydict_remove(ctx, value);
            if (rc == -1) {
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(module, type, LLLYEXT_PAR_TYPE, node, LLLYEXT_SUBSTMT_BASE, 0, unres)) {
                goto error;
            }
        }

        if (!yin->child) {
            if (type->der->type.der) {
                /* this is just a derived type with no base required */
                break;
            }
            LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "base", "type");
            goto error;
        } else {
            if (type->der->type.der) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, "base");
                goto error;
            }
        }
        if (yin->child->next) {
            LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, yin->child->next->name, yin->name);
            goto error;
        }
        break;

    case LLLY_TYPE_INST:
        /* RFC 6020 9.13.2 - require-instance */
        LLLY_TREE_FOR(yin->child, node) {

            if (!strcmp(node->name, "require-instance")) {
                if (type->info.inst.req) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, yin->name);
                    goto error;
                }
                GETVAL(ctx, value, node, "value");
                if (!strcmp(value, "true")) {
                    type->info.inst.req = 1;
                } else if (!strcmp(value, "false")) {
                    type->info.inst.req = -1;
                } else {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, node->name);
                    goto error;
                }

                /* extensions */
                if (lllyp_yin_parse_subnode_ext(module, type, LLLYEXT_PAR_TYPE, node, LLLYEXT_SUBSTMT_REQINSTANCE, 0, unres)) {
                    goto error;
                }
            } else {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name);
                goto error;
            }
        }

        break;

    case LLLY_TYPE_BINARY:
        /* RFC 6020 9.8.1, 9.4.4 - length, number of octets it contains */
    case LLLY_TYPE_INT8:
    case LLLY_TYPE_INT16:
    case LLLY_TYPE_INT32:
    case LLLY_TYPE_INT64:
    case LLLY_TYPE_UINT8:
    case LLLY_TYPE_UINT16:
    case LLLY_TYPE_UINT32:
    case LLLY_TYPE_UINT64:
        /* RFC 6020 9.2.4 - range */

        /* length and range are actually the same restriction, so process
         * them by this common code, we just need to differ the name and
         * structure where the information will be stored
         */
        if (type->base == LLLY_TYPE_BINARY) {
            restrs = &type->info.binary.length;
            name = "length";
        } else {
            restrs = &type->info.num.range;
            name = "range";
        }

        LLLY_TREE_FOR(yin->child, node) {

            if (!strcmp(node->name, name)) {
                if (*restrs) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, yin->name);
                    goto error;
                }

                GETVAL(ctx, value, node, "value");
                if (lllyp_check_length_range(ctx, value, type)) {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, name);
                    goto error;
                }
                *restrs = calloc(1, sizeof **restrs);
                LLLY_CHECK_ERR_GOTO(!(*restrs), LOGMEM(ctx), error);
                (*restrs)->expr = lllydict_insert(ctx, value, 0);

                /* get possible substatements */
                if (read_restr_substmt(module, *restrs, node, unres)) {
                    goto error;
                }

                for (j = 0; j < (*restrs)->ext_size; ++j) {
                    /* set flag, which represent LLLYEXT_OPT_VALID */
                    if ((*restrs)->ext[j]->flags & LLLYEXT_OPT_VALID) {
                        type->parent->flags |= LLLYS_VALID_EXT;
                        break;
                    }
                }
            } else {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name);
                goto error;
            }
        }
        break;

    case LLLY_TYPE_LEAFREF:
        /* flag resolving for later use */
        if (!parenttype && lllys_ingrouping(parent)) {
            /* just a flag - do not resolve */
            parenttype = 1;
        }

        /* RFC 6020 9.9.2 - path */
        LLLY_TREE_FOR(yin->child, node) {
            if (!strcmp(node->name, "path") && !type->der->type.der) {
                /* keep path for later */
            } else if (module->version >= 2 && !strcmp(node->name, "require-instance") && !type->der->type.der) {
                if (type->info.lref.req) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, yin->name);
                    goto error;
                }
                GETVAL(ctx, value, node, "value");
                if (!strcmp(value, "true")) {
                    type->info.lref.req = 1;
                } else if (!strcmp(value, "false")) {
                    type->info.lref.req = -1;
                } else {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, node->name);
                    goto error;
                }

                /* extensions */
                if (lllyp_yin_parse_subnode_ext(module, type, LLLYEXT_PAR_TYPE, node, LLLYEXT_SUBSTMT_REQINSTANCE, 0, unres)) {
                    goto error;
                }
            } else {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name);
                goto error;
            }
        }

        /* now that require-instance is properly set, try to find and resolve path */
        LLLY_TREE_FOR(yin->child, node) {
            if (!strcmp(node->name, "path") && !type->der->type.der) {
                if (type->info.lref.path) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, yin->name);
                    goto error;
                }

                GETVAL(ctx, value, node, "value");
                /* store in the JSON format */
                type->info.lref.path = transform_schema2json(module, value);
                if (!type->info.lref.path) {
                    goto error;
                }

                /* try to resolve leafref path only when this is instantiated
                 * leaf, so it is not:
                 * - typedef's type,
                 * - in  grouping definition,
                 * - just instantiated in a grouping definition,
                 * because in those cases the nodes referenced in path might not be present
                 * and it is not a bug.  */
                if (!parenttype && unres_schema_add_node(module, unres, type, UNRES_TYPE_LEAFREF, parent) == -1) {
                    goto error;
                }

                /* extensions */
                if (lllyp_yin_parse_subnode_ext(module, type, LLLYEXT_PAR_TYPE, node, LLLYEXT_SUBSTMT_PATH, 0, unres)) {
                    goto error;
                }

                break;
            }
        }

        if (!type->info.lref.path) {
            if (!type->der->type.der) {
                LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "path", "type");
                goto error;
            } else {
                /* copy leafref definition into the derived type */
                type->info.lref.path = lllydict_insert(ctx, type->der->type.info.lref.path, 0);
                type->info.lref.req = type->der->type.info.lref.req;
                /* and resolve the path at the place we are (if not in grouping/typedef) */
                if (!parenttype && unres_schema_add_node(module, unres, type, UNRES_TYPE_LEAFREF, parent) == -1) {
                    goto error;
                }
            }
        }

        break;

    case LLLY_TYPE_STRING:
        /* RFC 6020 9.4.4 - length */
        /* RFC 6020 9.4.6 - pattern */
        i = 0;
        LLLY_TREE_FOR_SAFE(yin->child, next, node) {

            if (!strcmp(node->name, "length")) {
                if (type->info.str.length) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, yin->name);
                    goto error;
                }

                GETVAL(ctx, value, node, "value");
                if (lllyp_check_length_range(ctx, value, type)) {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "length");
                    goto error;
                }
                type->info.str.length = calloc(1, sizeof *type->info.str.length);
                LLLY_CHECK_ERR_GOTO(!type->info.str.length, LOGMEM(ctx), error);
                type->info.str.length->expr = lllydict_insert(ctx, value, 0);

                /* get possible sub-statements */
                if (read_restr_substmt(module, type->info.str.length, node, unres)) {
                    goto error;
                }

                for (j = 0; j < type->info.str.length->ext_size; ++j) {
                    /* set flag, which represent LLLYEXT_OPT_VALID */
                    if (type->info.str.length->ext[j]->flags & LLLYEXT_OPT_VALID) {
                        type->parent->flags |= LLLYS_VALID_EXT;
                        break;
                    }
                }
                lllyxml_free(ctx, node);
            } else if (!strcmp(node->name, "pattern")) {
                YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, i, type->info.str.pat_count, "patterns", "type", error);
                i++;
            } else {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name);
                goto error;
            }
        }
        /* store patterns in array */
        if (i) {
            if (!parenttype && parent && lllys_ingrouping(parent)) {
                in_grp = 1;
            }
            type->info.str.patterns = calloc(i, sizeof *type->info.str.patterns);
            LLLY_CHECK_ERR_GOTO(!type->info.str.patterns, LOGMEM(ctx), error);
#ifdef LLLY_ENABLED_CACHE
            if (!in_grp) {
                /* do not compile patterns in groupings */
                type->info.str.patterns_pcre = calloc(2 * i, sizeof *type->info.str.patterns_pcre);
                LLLY_CHECK_ERR_GOTO(!type->info.str.patterns_pcre, LOGMEM(ctx), error);
            }
#endif
            LLLY_TREE_FOR(yin->child, node) {
                GETVAL(ctx, value, node, "value");

                if (in_grp) {
                    /* in grouping, just check the pattern syntax */
                    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) && lllyp_check_pattern(ctx, value, NULL)) {
                        goto error;
                    }
                }
#ifdef LLLY_ENABLED_CACHE
                else {
                    /* outside grouping, check syntax and precompile pattern for later use by libpcre */
                    if (lllyp_precompile_pattern(ctx, value,
                            (pcre **)&type->info.str.patterns_pcre[type->info.str.pat_count * 2],
                            (pcre_extra **)&type->info.str.patterns_pcre[type->info.str.pat_count * 2 + 1])) {
                        goto error;
                    }
                }
#endif
                restr = &type->info.str.patterns[type->info.str.pat_count]; /* shortcut */
                type->info.str.pat_count++;

                modifier = 0x06; /* ACK */
                name = NULL;
                if (module->version >= 2) {
                    LLLY_TREE_FOR_SAFE(node->child, next2, child) {
                        if (child->ns && !strcmp(child->ns->value, LLLY_NSYIN) && !strcmp(child->name, "modifier")) {
                            if (name) {
                                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, "modifier", node->name);
                                goto error;
                            }

                            GETVAL(ctx, name, child, "value");
                            if (!strcmp(name, "invert-match")) {
                                modifier = 0x15; /* NACK */
                            } else {
                                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, name, "modifier");
                                goto error;
                            }
                            /* get extensions of the modifier */
                            if (lllyp_yin_parse_subnode_ext(module, restr, LLLYEXT_PAR_RESTR, child,
                                                          LLLYEXT_SUBSTMT_MODIFIER, 0, unres)) {
                                goto error;
                            }

                            lllyxml_free(ctx, child);
                        }
                    }
                }

                len = strlen(value);
                buf = malloc((len + 2) * sizeof *buf); /* modifier byte + value + terminating NULL byte */
                LLLY_CHECK_ERR_GOTO(!buf, LOGMEM(ctx), error);
                buf[0] = modifier;
                strcpy(&buf[1], value);

                restr->expr = lllydict_insert_zc(ctx, buf);

                /* get possible sub-statements */
                if (read_restr_substmt(module, restr, node, unres)) {
                    goto error;
                }

                for (j = 0; j < restr->ext_size; ++j) {
                    /* set flag, which represent LLLYEXT_OPT_VALID */
                    if (restr->ext[j]->flags & LLLYEXT_OPT_VALID) {
                        type->parent->flags |= LLLYS_VALID_EXT;
                        break;
                    }
                }
            }
        }
        break;

    case LLLY_TYPE_UNION:
        /* RFC 6020 7.4 - type */
        /* count number of types in union */
        i = 0;
        LLLY_TREE_FOR_SAFE(yin->child, next, node) {

            if (!strcmp(node->name, "type")) {
                if (type->der->type.der) {
                    /* type can be a substatement only in "union" type, not in derived types */
                    LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, "type", "derived type");
                    goto error;
                }
                YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, i, type->info.uni.count, "types", "type", error);
                i++;
            } else {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name);
                goto error;
            }
        }

        if (!i && !type->der->type.der) {
            LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "type", "(union) type");
            goto error;
        }

        /* inherit instid presence information */
        if ((type->der->type.base == LLLY_TYPE_UNION) && type->der->type.info.uni.has_ptr_type) {
            type->info.uni.has_ptr_type = 1;
        }

        /* allocate array for union's types ... */
        if (i) {
            type->info.uni.types = calloc(i, sizeof *type->info.uni.types);
            LLLY_CHECK_ERR_GOTO(!type->info.uni.types, LOGMEM(ctx), error);
        }

        /* ... and fill the structures */
        LLLY_TREE_FOR(yin->child, node) {
            type->info.uni.types[type->info.uni.count].parent = type->parent;
            rc = fill_yin_type(module, parent, node, &type->info.uni.types[type->info.uni.count], parenttype, unres);
            if (!rc) {
                type->info.uni.count++;

                if (module->version < 2) {
                    /* union's type cannot be empty or leafref */
                    if (type->info.uni.types[type->info.uni.count - 1].base == LLLY_TYPE_EMPTY) {
                        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, "empty", node->name);
                        rc = -1;
                    } else if (type->info.uni.types[type->info.uni.count - 1].base == LLLY_TYPE_LEAFREF) {
                        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, "leafref", node->name);
                        rc = -1;
                    }
                }

                if ((type->info.uni.types[type->info.uni.count - 1].base == LLLY_TYPE_INST)
                        || (type->info.uni.types[type->info.uni.count - 1].base == LLLY_TYPE_LEAFREF)
                        || ((type->info.uni.types[type->info.uni.count - 1].base == LLLY_TYPE_UNION)
                        && type->info.uni.types[type->info.uni.count - 1].info.uni.has_ptr_type)) {
                    type->info.uni.has_ptr_type = 1;
                }
            }
            if (rc) {
                /* even if we got EXIT_FAILURE, throw it all away, too much trouble doing something else */
                for (i = 0; i < type->info.uni.count; ++i) {
                    lllys_type_free(ctx, &type->info.uni.types[i], NULL);
                }
                free(type->info.uni.types);
                type->info.uni.types = NULL;
                type->info.uni.count = 0;
                type->info.uni.has_ptr_type = 0;
                type->der = NULL;
                type->base = LLLY_TYPE_DER;

                if (rc == EXIT_FAILURE) {
                    ret = EXIT_FAILURE;
                }
                goto error;
            }
        }
        break;

    case LLLY_TYPE_BOOL:
    case LLLY_TYPE_EMPTY:
        /* no sub-statement allowed */
        if (yin->child) {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, yin->child->name);
            goto error;
        }
        break;

    default:
        LOGINT(ctx);
        goto error;
    }

    for(j = 0; j < type->ext_size; ++j) {
        /* set flag, which represent LLLYEXT_OPT_VALID */
        if (type->ext[j]->flags & LLLYEXT_OPT_VALID) {
            type->parent->flags |= LLLYS_VALID_EXT;
            break;
        }
    }

    /* if derived type has extension, which need validate data */
    dertype = &type->der->type;
    while (dertype->der) {
        if (dertype->parent->flags & LLLYS_VALID_EXT) {
            type->parent->flags |= LLLYS_VALID_EXT;
        }
        dertype = &dertype->der->type;
    }

    return EXIT_SUCCESS;

error:
    lllyxml_free_withsiblings(ctx, exts.child);
    return ret;
}

/* logs directly */
static int
fill_yin_typedef(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin, struct lllys_tpdf *tpdf,
                 struct unres_schema *unres)
{
    const char *value;
    struct lllyxml_elem *node, *next;
    struct llly_ctx *ctx = module->ctx;
    int rc, has_type = 0, c_ext = 0, i;
    void *reallocated;

    GETVAL(ctx, value, yin, "name");
    if (lllyp_check_identifier(ctx, value, LLLY_IDENT_TYPE, module, parent)) {
        goto error;
    }
    tpdf->name = lllydict_insert(ctx, value, strlen(value));

    /* generic part - status, description, reference */
    if (read_yin_common(module, NULL, tpdf, LLLYEXT_PAR_TPDF, yin, OPT_MODULE, unres)) {
        goto error;
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, node) {
        if (strcmp(node->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, tpdf->ext_size, "extensions", "typedef", error);
            c_ext++;
            continue;
        } else if (!strcmp(node->name, "type")) {
            if (has_type) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, yin->name);
                goto error;
            }
            /* HACK for unres */
            tpdf->type.der = (struct lllys_tpdf *)node;
            tpdf->type.parent = tpdf;
            if (unres_schema_add_node(module, unres, &tpdf->type, UNRES_TYPE_DER_TPDF, parent) == -1) {
                goto error;
            }
            has_type = 1;

            /* skip lllyxml_free() at the end of the loop, node was freed or at least unlinked in unres processing */
            continue;
        } else if (!strcmp(node->name, "default")) {
            if (tpdf->dflt) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, yin->name);
                goto error;
            }
            GETVAL(ctx, value, node, "value");
            tpdf->dflt = lllydict_insert(ctx, value, strlen(value));

            if (lllyp_yin_parse_subnode_ext(module, tpdf, LLLYEXT_PAR_TPDF, node, LLLYEXT_SUBSTMT_DEFAULT, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "units")) {
            if (tpdf->units) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, yin->name);
                goto error;
            }
            GETVAL(ctx, value, node, "name");
            tpdf->units = lllydict_insert(ctx, value, strlen(value));

            if (lllyp_yin_parse_subnode_ext(module, tpdf, LLLYEXT_PAR_TPDF, node, LLLYEXT_SUBSTMT_UNITS, 0, unres)) {
                goto error;
            }
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, value);
            goto error;
        }

        lllyxml_free(ctx, node);
    }

    /* check mandatory value */
    if (!has_type) {
        LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "type", yin->name);
        goto error;
    }

    /* check default value (if not defined, there still could be some restrictions
     * that need to be checked against a default value from a derived type) */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) &&
            unres_schema_add_node(module, unres, &tpdf->type, UNRES_TYPEDEF_DFLT, (struct lllys_node *)(&tpdf->dflt)) == -1) {
        goto error;
    }

    /* finish extensions parsing */
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(tpdf->ext, (c_ext + tpdf->ext_size) * sizeof *tpdf->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        tpdf->ext = reallocated;

        /* init memory */
        memset(&tpdf->ext[tpdf->ext_size], 0, c_ext * sizeof *tpdf->ext);

        LLLY_TREE_FOR_SAFE(yin->child, next, node) {
            rc = lllyp_yin_fill_ext(tpdf, LLLYEXT_PAR_TYPE, 0, 0, module, node, &tpdf->ext, tpdf->ext_size, unres);
            tpdf->ext_size++;
            if (rc) {
                goto error;
            }
        }
    }

    for (i = 0; i < tpdf->ext_size; ++i) {
        /* set flag, which represent LLLYEXT_OPT_VALID */
        if (tpdf->ext[i]->flags & LLLYEXT_OPT_VALID) {
            tpdf->flags |= LLLYS_VALID_EXT;
            break;
        }
    }

    return EXIT_SUCCESS;

error:
    return EXIT_FAILURE;
}

static int
fill_yin_extension(struct lllys_module *module, struct lllyxml_elem *yin, struct lllys_ext *ext, struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    const char *value;
    struct lllyxml_elem *child, *node, *next, *next2;
    int c_ext = 0, rc;
    void *reallocated;

    GETVAL(ctx, value, yin, "name");

    if (lllyp_check_identifier(ctx, value, LLLY_IDENT_EXTENSION, module, NULL)) {
        goto error;
    }
    ext->name = lllydict_insert(ctx, value, strlen(value));

    if (read_yin_common(module, NULL, ext, LLLYEXT_PAR_EXT, yin, OPT_MODULE, unres)) {
        goto error;
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, node) {
        if (strcmp(node->ns->value, LLLY_NSYIN)) {
            /* possible extension instance */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, ext->ext_size, "extensions", "extension", error);
            c_ext++;
        } else if (!strcmp(node->name, "argument")) {
            /* argument */
            GETVAL(ctx, value, node, "name");
            ext->argument = lllydict_insert(ctx, value, strlen(value));
            if (lllyp_yin_parse_subnode_ext(module, ext, LLLYEXT_PAR_EXT, node, LLLYEXT_SUBSTMT_ARGUMENT, 0, unres)) {
                goto error;
            }

            /* yin-element */
            LLLY_TREE_FOR_SAFE(node->child, next2, child) {
                if (child->ns == node->ns && !strcmp(child->name, "yin-element")) {
                    GETVAL(ctx, value, child, "value");
                    if (llly_strequal(value, "true", 0)) {
                        ext->flags |= LLLYS_YINELEM;
                    }

                    if (lllyp_yin_parse_subnode_ext(module, ext, LLLYEXT_PAR_EXT, child, LLLYEXT_SUBSTMT_YINELEM, 0, unres)) {
                        goto error;
                    }
                } else if (child->ns) {
                    /* unexpected YANG statement */
                    LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, child->name, child->name);
                    goto error;
                } /* else garbage, but save resource needed for unlinking */
            }

            lllyxml_free(ctx, node);
        } else {
            /* unexpected YANG statement */
            LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, node->name, node->name);
            goto error;
        }
    }

    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(ext->ext, (c_ext + ext->ext_size) * sizeof *ext->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        ext->ext = reallocated;

        /* init memory */
        memset(&ext->ext[ext->ext_size], 0, c_ext * sizeof *ext->ext);

        /* process the extension instances of the extension itself */
        LLLY_TREE_FOR_SAFE(yin->child, next, node) {
            rc = lllyp_yin_fill_ext(ext, LLLYEXT_PAR_EXT, 0, 0, module, node, &ext->ext, ext->ext_size, unres);
            ext->ext_size++;
            if (rc) {
                goto error;
            }
        }
    }

    /* search for plugin */
    ext->plugin = ext_get_plugin(ext->name, ext->module->name, ext->module->rev ? ext->module->rev[0].date : NULL);

    return EXIT_SUCCESS;

error:
    return EXIT_FAILURE;
}

/* logs directly */
static int
fill_yin_feature(struct lllys_module *module, struct lllyxml_elem *yin, struct lllys_feature *f, struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    const char *value;
    struct lllyxml_elem *child, *next;
    int c_ftrs = 0, c_ext = 0, ret;
    void *reallocated;

    GETVAL(ctx, value, yin, "name");
    if (lllyp_check_identifier(ctx, value, LLLY_IDENT_FEATURE, module, NULL)) {
        goto error;
    }
    f->name = lllydict_insert(ctx, value, strlen(value));
    f->module = module;

    if (read_yin_common(module, NULL, f, LLLYEXT_PAR_FEATURE, yin, 0, unres)) {
        goto error;
    }

    LLLY_TREE_FOR(yin->child, child) {
        if (strcmp(child->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, f->ext_size, "extensions", "feature", error);
            c_ext++;
        } else if (!strcmp(child->name, "if-feature")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, f->iffeature_size, "if-feature", "feature", error);
            c_ftrs++;
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
            goto error;
        }
    }

    if (c_ftrs) {
        f->iffeature = calloc(c_ftrs, sizeof *f->iffeature);
        LLLY_CHECK_ERR_GOTO(!f->iffeature, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(f->ext, (c_ext + f->ext_size) * sizeof *f->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        f->ext = reallocated;

        /* init memory */
        memset(&f->ext[f->ext_size], 0, c_ext * sizeof *f->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, child) {
        if (strcmp(child->ns->value, LLLY_NSYIN)) {
            /* extension */
            ret = lllyp_yin_fill_ext(f, LLLYEXT_PAR_FEATURE, 0, 0, module, child, &f->ext, f->ext_size, unres);
            f->ext_size++;
            if (ret) {
                goto error;
            }
        } else { /* if-feature */
            ret = fill_yin_iffeature((struct lllys_node *)f, 1, child, &f->iffeature[f->iffeature_size], unres);
            f->iffeature_size++;
            if (ret) {
                goto error;
            }
        }
    }

    /* check for circular dependencies */
    if (f->iffeature_size) {
        if (unres_schema_add_node(module, unres, f, UNRES_FEATURE, NULL) == -1) {
            goto error;
        }
    }

    return EXIT_SUCCESS;

error:
    return EXIT_FAILURE;
}

/* logs directly */
static int
fill_yin_must(struct lllys_module *module, struct lllyxml_elem *yin, struct lllys_restr *must, struct unres_schema *unres)
{
    int ret = EXIT_FAILURE;
    const char *value;

    must->expr = NULL;
    GETVAL(module->ctx, value, yin, "condition");
    must->expr = transform_schema2json(module, value);
    if (!must->expr) {
        goto error;
    }

    ret = read_restr_substmt(module, must, yin, unres);

error:
    if (ret) {
        lllydict_remove(module->ctx, must->expr);
        must->expr = NULL;
    }
    return ret;
}

static int
fill_yin_revision(struct lllys_module *module, struct lllyxml_elem *yin, struct lllys_revision *rev,
                  struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllyxml_elem *next, *child;
    const char *value;

    GETVAL(ctx, value, yin, "date");
    if (lllyp_check_date(ctx, value)) {
        goto error;
    }
    memcpy(rev->date, value, LLLY_REV_SIZE - 1);

    LLLY_TREE_FOR_SAFE(yin->child, next, child) {
        if (!child->ns) {
            /* garbage */
            continue;
        } else if (strcmp(child->ns->value, LLLY_NSYIN)) {
            /* possible extension instance */
            if (lllyp_yin_parse_subnode_ext(module, rev, LLLYEXT_PAR_REVISION,
                                          child, LLLYEXT_SUBSTMT_SELF, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(child->name, "description")) {
            if (rev->dsc) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(module, rev, LLLYEXT_PAR_REVISION,
                                          child, LLLYEXT_SUBSTMT_DESCRIPTION, 0, unres)) {
                goto error;
            }
            rev->dsc = read_yin_subnode(ctx, child, "text");
            if (!rev->dsc) {
                goto error;
            }
        } else if (!strcmp(child->name, "reference")) {
            if (rev->ref) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(module, rev, LLLYEXT_PAR_REVISION,
                                          child, LLLYEXT_SUBSTMT_REFERENCE, 0, unres)) {
                goto error;
            }
            rev->ref = read_yin_subnode(ctx, child, "text");
            if (!rev->ref) {
                goto error;
            }
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
            goto error;
        }
    }

    return EXIT_SUCCESS;

error:
    return EXIT_FAILURE;
}

static int
fill_yin_unique(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin, struct lllys_unique *unique,
                struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    int i, j, ret = EXIT_FAILURE;
    const char *orig;
    char *value, *vaux, *start = NULL, c;
    struct unres_list_uniq *unique_info;

    /* get unique value (list of leafs supposed to be unique */
    GETVAL(ctx, orig, yin, "tag");

    /* count the number of unique leafs in the value */
    start = value = vaux = strdup(orig);
    LLLY_CHECK_ERR_GOTO(!vaux, LOGMEM(ctx), error);
    while ((vaux = strpbrk(vaux, " \t\n"))) {
        YIN_CHECK_ARRAY_OVERFLOW_CODE(ctx, unique->expr_size, unique->expr_size, "referenced items", "unique",
                                      unique->expr_size = 0; goto error);
        unique->expr_size++;
        while (isspace(*vaux)) {
            vaux++;
        }
    }
    unique->expr_size++;
    unique->expr = calloc(unique->expr_size, sizeof *unique->expr);
    LLLY_CHECK_ERR_GOTO(!unique->expr, LOGMEM(ctx), error);

    for (i = 0; i < unique->expr_size; i++) {
        vaux = strpbrk(value, " \t\n");
        if (vaux) {
            c = *vaux;
            *vaux = '\0';
        }

        /* store token into unique structure */
        unique->expr[i] = transform_schema2json(module, value);
        if (vaux) {
            *vaux = c;
        }

        /* check that the expression does not repeat */
        for (j = 0; j < i; j++) {
            if (llly_strequal(unique->expr[j], unique->expr[i], 1)) {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, unique->expr[i], "unique");
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "The identifier is not unique");
                goto error;
            }
        }

        /* try to resolve leaf */
        if (unres) {
            unique_info = malloc(sizeof *unique_info);
            LLLY_CHECK_ERR_GOTO(!unique_info, LOGMEM(ctx), error);
            unique_info->list = parent;
            unique_info->expr = unique->expr[i];
            unique_info->trg_type = &unique->trg_type;
            if (unres_schema_add_node(module, unres, unique_info, UNRES_LIST_UNIQ, NULL) == -1){
                goto error;
            }
        } else {
            if (resolve_unique(parent, unique->expr[i], &unique->trg_type)) {
                goto error;
            }
        }

        /* move to next token */
        value = vaux;
        while (value && isspace(*value)) {
            value++;
        }
    }

    ret =  EXIT_SUCCESS;

error:
    free(start);
    return ret;
}

/* logs directly
 *
 * type: 0 - min, 1 - max
 */
static int
deviate_minmax(struct lllys_node *target, struct lllyxml_elem *node, struct lllys_deviate *d, int type)
{
    const char *value;
    char *endptr;
    unsigned long val;
    uint32_t *ui32val, *min, *max;
    struct llly_ctx *ctx = target->module->ctx;

    /* del min/max is forbidden */
    if (d->mod == LLLY_DEVIATE_DEL) {
        LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, (type ? "max-elements" : "min-elements"), "deviate delete");
        goto error;
    }

    /* check target node type */
    if (target->nodetype == LLLYS_LEAFLIST) {
        max = &((struct lllys_node_leaflist *)target)->max;
        min = &((struct lllys_node_leaflist *)target)->min;
    } else if (target->nodetype == LLLYS_LIST) {
        max = &((struct lllys_node_list *)target)->max;
        min = &((struct lllys_node_list *)target)->min;
    } else {
        LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name);
        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Target node does not allow \"%s\" property.", node->name);
        goto error;
    }

    GETVAL(ctx, value, node, "value");
    while (isspace(value[0])) {
        value++;
    }

    if (type && !strcmp(value, "unbounded")) {
        d->max = val = 0;
        d->max_set = 1;
        ui32val = max;
    } else {
        /* convert it to uint32_t */
        errno = 0;
        endptr = NULL;
        val = strtoul(value, &endptr, 10);
        if (*endptr || value[0] == '-' || errno || val > UINT32_MAX) {
            LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, node->name);
            goto error;
        }
        if (type) {
            d->max = (uint32_t)val;
            d->max_set = 1;
            ui32val = max;
        } else {
            d->min = (uint32_t)val;
            d->min_set = 1;
            ui32val = min;
        }
    }

    if (d->mod == LLLY_DEVIATE_ADD) {
        /* check that there is no current value */
        if (*ui32val) {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->name);
            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Adding property that already exists.");
            goto error;
        }
    } else if (d->mod == LLLY_DEVIATE_RPL) {
        /* unfortunately, there is no way to check reliably that there
         * was a value before, it could have been the default */
    }

    /* add (already checked) and replace */
    /* set new value specified in deviation */
    *ui32val = (uint32_t)val;

    /* check min-elements is smaller than max-elements */
    if (*max && *min > *max) {
        if (type) {
            LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "max-elements");
            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "\"max-elements\" is smaller than \"min-elements\".");
        } else {
            LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "min-elements");
            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "\"min-elements\" is bigger than \"max-elements\".");
        }
        goto error;
    }

    return EXIT_SUCCESS;

error:
    return EXIT_FAILURE;
}

/* logs directly */
static int
fill_yin_deviation(struct lllys_module *module, struct lllyxml_elem *yin, struct lllys_deviation *dev,
                   struct unres_schema *unres)
{
    const char *value, **stritem;
    struct lllyxml_elem *next, *next2, *child, *develem;
    int c_dev = 0, c_must, c_uniq, c_dflt, c_ext = 0;
    int f_min = 0, f_max = 0; /* flags */
    int i, j, k, rc;
    unsigned int u;
    struct llly_ctx *ctx = module->ctx;
    struct lllys_deviate *d = NULL;
    struct lllys_node *node, *parent, *dev_target = NULL;
    struct lllys_node_choice *choice = NULL;
    struct lllys_node_leaf *leaf = NULL;
    struct llly_set *dflt_check = llly_set_new(), *set;
    struct lllys_node_list *list = NULL;
    struct lllys_node_leaflist *llist = NULL;
    struct lllys_node_inout *inout;
    struct lllys_type *t = NULL;
    uint8_t *trg_must_size = NULL;
    struct lllys_restr **trg_must = NULL;
    struct unres_schema tmp_unres;
    struct lllys_module *mod;
    void *reallocated;
    size_t deviate_must_index;

    GETVAL(ctx, value, yin, "target-node");
    dev->target_name = transform_schema2json(module, value);
    if (!dev->target_name) {
        goto error;
    }

    /* resolve target node */
    rc = resolve_schema_nodeid(dev->target_name, NULL, module, &set, 0, 1);
    if (rc == -1) {
        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, dev->target_name, yin->name);
        llly_set_free(set);
        goto error;
    }
    dev_target = set->set.s[0];
    llly_set_free(set);

    if (dev_target->module == lllys_main_module(module)) {
        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, dev->target_name, yin->name);
        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Deviating own module is not allowed.");
        goto error;
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, child) {
        if (!child->ns ) {
            /* garbage */
            lllyxml_free(ctx, child);
            continue;
        } else if (strcmp(child->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, dev->ext_size, "extensions", "deviation", error);
            c_ext++;
            continue;
        } else if (!strcmp(child->name, "description")) {
            if (dev->dsc) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(module, dev, LLLYEXT_PAR_DEVIATION, child, LLLYEXT_SUBSTMT_DESCRIPTION, 0, unres)) {
                goto error;
            }
            dev->dsc = read_yin_subnode(ctx, child, "text");
            if (!dev->dsc) {
                goto error;
            }
        } else if (!strcmp(child->name, "reference")) {
            if (dev->ref) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(module, dev, LLLYEXT_PAR_DEVIATION, child, LLLYEXT_SUBSTMT_REFERENCE, 0, unres)) {
                goto error;
            }
            dev->ref = read_yin_subnode(ctx, child, "text");
            if (!dev->ref) {
                goto error;
            }
        } else if (!strcmp(child->name, "deviate")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_dev, dev->deviate_size, "deviates", "deviation", error);
            c_dev++;

            /* skip lllyxml_free() at the end of the loop, node will be
             * further processed later
             */
            continue;

        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
            goto error;
        }

        lllyxml_free(ctx, child);
    }

    if (c_dev) {
        dev->deviate = calloc(c_dev, sizeof *dev->deviate);
        LLLY_CHECK_ERR_GOTO(!dev->deviate, LOGMEM(ctx), error);
    } else {
        LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "deviate", "deviation");
        goto error;
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(dev->ext, (c_ext + dev->ext_size) * sizeof *dev->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        dev->ext = reallocated;

        /* init memory */
        memset(&dev->ext[dev->ext_size], 0, c_ext * sizeof *dev->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, develem) {
        if (strcmp(develem->ns->value, LLLY_NSYIN)) {
            /* deviation's extension */
            rc = lllyp_yin_fill_ext(dev, LLLYEXT_PAR_DEVIATION, 0, 0, module, develem, &dev->ext, dev->ext_size, unres);
            dev->ext_size++;
            if (rc) {
                goto error;
            }
            continue;
        }

        /* deviate */
        /* init */
        f_min = 0;
        f_max = 0;
        c_must = 0;
        c_uniq = 0;
        c_dflt = 0;
        c_ext = 0;

        /* get deviation type */
        GETVAL(ctx, value, develem, "value");
        if (!strcmp(value, "not-supported")) {
            dev->deviate[dev->deviate_size].mod = LLLY_DEVIATE_NO;
            /* no other deviate statement is expected,
             * not-supported deviation must be the only deviation of the target
             */
            if (dev->deviate_size || develem->next) {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, develem->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "\"not-supported\" deviation cannot be combined with any other deviation.");
                goto error;
            }

            /* you cannot remove a key leaf */
            if ((dev_target->nodetype == LLLYS_LEAF) && lllys_parent(dev_target) && (lllys_parent(dev_target)->nodetype == LLLYS_LIST)) {
                for (i = 0; i < ((struct lllys_node_list *)lllys_parent(dev_target))->keys_size; ++i) {
                    if (((struct lllys_node_list *)lllys_parent(dev_target))->keys[i] == (struct lllys_node_leaf *)dev_target) {
                        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, develem->name);
                        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "\"not-supported\" deviation cannot remove a list key.");
                        goto error;
                    }
                }
            }

            /* unlink and store the original node */
            parent = dev_target->parent;
            lllys_node_unlink(dev_target);
            if (parent) {
                if (parent->nodetype & (LLLYS_AUGMENT | LLLYS_USES)) {
                    /* hack for augment, because when the original will be sometime reconnected back, we actually need
                     * to reconnect it to both - the augment and its target (which is deduced from the deviations target
                     * path), so we need to remember the augment as an addition */
                    /* remember uses parent so we can reconnect to it */
                    dev_target->parent = parent;
                } else if (parent->nodetype & (LLLYS_RPC | LLLYS_ACTION)) {
                    /* re-create implicit node */
                    inout = calloc(1, sizeof *inout);
                    LLLY_CHECK_ERR_GOTO(!inout, LOGMEM(ctx), error);

                    inout->nodetype = dev_target->nodetype;
                    inout->name = lllydict_insert(ctx, (inout->nodetype == LLLYS_INPUT) ? "input" : "output", 0);
                    inout->module = dev_target->module;
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
            dev->orig_node = dev_target;

        } else if (!strcmp(value, "add")) {
            dev->deviate[dev->deviate_size].mod = LLLY_DEVIATE_ADD;
        } else if (!strcmp(value, "replace")) {
            dev->deviate[dev->deviate_size].mod = LLLY_DEVIATE_RPL;
        } else if (!strcmp(value, "delete")) {
            dev->deviate[dev->deviate_size].mod = LLLY_DEVIATE_DEL;
        } else {
            LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, develem->name);
            goto error;
        }
        d = &dev->deviate[dev->deviate_size];
        dev->deviate_size++;

        /* store a shallow copy of the original node */
        if (!dev->orig_node) {
            memset(&tmp_unres, 0, sizeof tmp_unres);
            dev->orig_node = lllys_node_dup(dev_target->module, NULL, dev_target, &tmp_unres, 1);
            /* just to be safe */
            if (tmp_unres.count) {
                LOGINT(ctx);
                goto error;
            }
        }

        /* process deviation properties */
        LLLY_TREE_FOR_SAFE(develem->child, next2, child) {
            if (!child->ns) {
                /* garbage */
                lllyxml_free(ctx, child);
                continue;
            } else if  (strcmp(child->ns->value, LLLY_NSYIN)) {
                /* extensions */
                YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, d->ext_size, "extensions", "deviate", error);
                c_ext++;
            } else if (d->mod == LLLY_DEVIATE_NO) {
                /* no YIN substatement expected in this case */
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                goto error;
            } else if (!strcmp(child->name, "config")) {
                if (d->flags & LLLYS_CONFIG_MASK) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                    goto error;
                }

                /* for we deviate from RFC 6020 and allow config property even it is/is not
                 * specified in the target explicitly since config property inherits. So we expect
                 * that config is specified in every node. But for delete, we check that the value
                 * is the same as here in deviation
                 */
                GETVAL(ctx, value, child, "value");
                if (!strcmp(value, "false")) {
                    d->flags |= LLLYS_CONFIG_R;
                } else if (!strcmp(value, "true")) {
                    d->flags |= LLLYS_CONFIG_W;
                } else {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, child->name);
                    goto error;
                }

                if (d->mod == LLLY_DEVIATE_DEL) {
                    /* del config is forbidden */
                    LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, "config", "deviate delete");
                    goto error;
                } else { /* add and replace are the same in this case */
                    /* remove current config value of the target ... */
                    dev_target->flags &= ~LLLYS_CONFIG_MASK;

                    /* ... and replace it with the value specified in deviation */
                    dev_target->flags |= d->flags & LLLYS_CONFIG_MASK;
                }

                if (lllyp_yin_parse_subnode_ext(module, d, LLLYEXT_PAR_DEVIATE, child, LLLYEXT_SUBSTMT_CONFIG, 0, unres)) {
                    goto error;
                }
            } else if (!strcmp(child->name, "default")) {
                if (lllyp_yin_parse_subnode_ext(module, d, LLLYEXT_PAR_DEVIATE, child, LLLYEXT_SUBSTMT_DEFAULT, c_dflt, unres)) {
                    goto error;
                }
                YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_dflt, d->dflt_size, "defaults", "deviate", error);
                c_dflt++;

                /* check target node type */
                if (module->version < 2 && dev_target->nodetype == LLLYS_LEAFLIST) {
                    LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, "default");
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Target node does not allow \"default\" property.");
                    goto error;
                } else if (c_dflt > 1 && dev_target->nodetype != LLLYS_LEAFLIST) { /* from YANG 1.1 */
                    LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, "default");
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Target node does not allow multiple \"default\" properties.");
                    goto error;
                } else if (c_dflt == 1 && (!(dev_target->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_CHOICE)))) {
                    LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, "default");
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Target node does not allow \"default\" property.");
                    goto error;
                }

                /* skip lllyxml_free() at the end of the loop, this node will be processed later */
                continue;

            } else if (!strcmp(child->name, "mandatory")) {
                if (d->flags & LLLYS_MAND_MASK) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                    goto error;
                }

                /* check target node type */
                if (!(dev_target->nodetype & (LLLYS_LEAF | LLLYS_CHOICE | LLLYS_ANYDATA))) {
                    LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Target node does not allow \"%s\" property.", child->name);
                    goto error;
                }

                GETVAL(ctx, value, child, "value");
                if (!strcmp(value, "false")) {
                    d->flags |= LLLYS_MAND_FALSE;
                } else if (!strcmp(value, "true")) {
                    d->flags |= LLLYS_MAND_TRUE;
                } else {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, child->name);
                    goto error;
                }

                if (d->mod == LLLY_DEVIATE_ADD) {
                    /* check that there is no current value */
                    if (dev_target->flags & LLLYS_MAND_MASK) {
                        LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Adding property that already exists.");
                        goto error;
                    }

                    /* check collision with default-stmt */
                    if (d->flags & LLLYS_MAND_TRUE) {
                        if (dev_target->nodetype == LLLYS_CHOICE) {
                            if (((struct lllys_node_choice *)(dev_target))->dflt) {
                                LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, child->name, child->parent->name);
                                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL,
                                       "Adding the \"mandatory\" statement is forbidden on choice with the \"default\" statement.");
                                goto error;
                            }
                        } else if (dev_target->nodetype == LLLYS_LEAF) {
                            if (((struct lllys_node_leaf *)(dev_target))->dflt) {
                                LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, child->name, child->parent->name);
                                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL,
                                       "Adding the \"mandatory\" statement is forbidden on leaf with the \"default\" statement.");
                                goto error;
                            }
                        }
                    }

                    dev_target->flags |= d->flags & LLLYS_MAND_MASK;
                } else if (d->mod == LLLY_DEVIATE_RPL) {
                    /* check that there was a value before */
                    if (!(dev_target->flags & LLLYS_MAND_MASK)) {
                        LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Replacing a property that does not exist.");
                        goto error;
                    }

                    dev_target->flags &= ~LLLYS_MAND_MASK;
                    dev_target->flags |= d->flags & LLLYS_MAND_MASK;
                } else if (d->mod == LLLY_DEVIATE_DEL) {
                    /* del mandatory is forbidden */
                    LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, "mandatory", "deviate delete");
                    goto error;
                }

                /* check for mandatory node in default case, first find the closest parent choice to the changed node */
                for (parent = dev_target->parent;
                     parent && !(parent->nodetype & (LLLYS_CHOICE | LLLYS_GROUPING | LLLYS_ACTION));
                     parent = parent->parent) {
                    if (parent->nodetype == LLLYS_CONTAINER && ((struct lllys_node_container *)parent)->presence) {
                        /* stop also on presence containers */
                        break;
                    }
                }
                /* and if it is a choice with the default case, check it for presence of a mandatory node in it */
                if (parent && parent->nodetype == LLLYS_CHOICE && ((struct lllys_node_choice *)parent)->dflt) {
                    if (lllyp_check_mandatory_choice(parent)) {
                        goto error;
                    }
                }

                if (lllyp_yin_parse_subnode_ext(module, d, LLLYEXT_PAR_DEVIATE, child, LLLYEXT_SUBSTMT_MANDATORY, 0, unres)) {
                    goto error;
                }
            } else if (!strcmp(child->name, "min-elements")) {
                if (f_min) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                    goto error;
                }
                f_min = 1;

                if (deviate_minmax(dev_target, child, d, 0)) {
                    goto error;
                }
                if (lllyp_yin_parse_subnode_ext(module, d, LLLYEXT_PAR_DEVIATE, child, LLLYEXT_SUBSTMT_MIN, 0, unres)) {
                    goto error;
                }
            } else if (!strcmp(child->name, "max-elements")) {
                if (f_max) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                    goto error;
                }
                f_max = 1;

                if (deviate_minmax(dev_target, child, d, 1)) {
                    goto error;
                }
                if (lllyp_yin_parse_subnode_ext(module, d, LLLYEXT_PAR_DEVIATE, child, LLLYEXT_SUBSTMT_MAX, 0, unres)) {
                    goto error;
                }
            } else if (!strcmp(child->name, "must")) {
                YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_must, d->must_size, "musts", "deviate", error);
                c_must++;
                /* skip lllyxml_free() at the end of the loop, this node will be processed later */
                continue;
            } else if (!strcmp(child->name, "type")) {
                if (d->type) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                    goto error;
                }

                /* add, del type is forbidden */
                if (d->mod == LLLY_DEVIATE_ADD) {
                    LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, "type", "deviate add");
                    goto error;
                } else if (d->mod == LLLY_DEVIATE_DEL) {
                    LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, "type", "deviate delete");
                    goto error;
                }

                /* check target node type */
                if (dev_target->nodetype == LLLYS_LEAF) {
                    t = &((struct lllys_node_leaf *)dev_target)->type;
                    if (((struct lllys_node_leaf *)dev_target)->dflt) {
                        llly_set_add(dflt_check, dev_target, 0);
                    }
                } else if (dev_target->nodetype == LLLYS_LEAFLIST) {
                    t = &((struct lllys_node_leaflist *)dev_target)->type;
                    if (((struct lllys_node_leaflist *)dev_target)->dflt) {
                        llly_set_add(dflt_check, dev_target, 0);
                    }
                } else {
                    LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Target node does not allow \"%s\" property.", child->name);
                    goto error;
                }

                /* replace */
                lllys_type_free(ctx, t, NULL);
                memset(t, 0, sizeof (struct lllys_type));
                /* HACK for unres */
                t->der = (struct lllys_tpdf *)child;
                t->parent = (struct lllys_tpdf *)dev_target;
                if (unres_schema_add_node(module, unres, t, UNRES_TYPE_DER, dev_target) == -1) {
                    goto error;
                }
                d->type = t;
            } else if (!strcmp(child->name, "unique")) {
                if (lllyp_yin_parse_subnode_ext(module, d, LLLYEXT_PAR_DEVIATE, child, LLLYEXT_SUBSTMT_UNIQUE, c_uniq, unres)) {
                    goto error;
                }
                YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_uniq, d->unique_size, "uniques", "deviate", error);
                c_uniq++;
                /* skip lllyxml_free() at the end of the loop, this node will be processed later */
                continue;
            } else if (!strcmp(child->name, "units")) {
                if (d->units) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                    goto error;
                }

                /* check target node type */
                if (dev_target->nodetype == LLLYS_LEAFLIST) {
                    stritem = &((struct lllys_node_leaflist *)dev_target)->units;
                } else if (dev_target->nodetype == LLLYS_LEAF) {
                    stritem = &((struct lllys_node_leaf *)dev_target)->units;
                } else {
                    LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Target node does not allow \"%s\" property.", child->name);
                    goto error;
                }

                /* get units value */
                GETVAL(ctx, value, child, "name");
                d->units = lllydict_insert(ctx, value, 0);

                /* apply to target */
                if (d->mod == LLLY_DEVIATE_ADD) {
                    /* check that there is no current value */
                    if (*stritem) {
                        LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Adding property that already exists.");
                        goto error;
                    }

                    *stritem = lllydict_insert(ctx, value, 0);
                } else if (d->mod == LLLY_DEVIATE_RPL) {
                    /* check that there was a value before */
                    if (!*stritem) {
                        LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Replacing a property that does not exist.");
                        goto error;
                    }

                    lllydict_remove(ctx, *stritem);
                    *stritem = lllydict_insert(ctx, value, 0);
                } else if (d->mod == LLLY_DEVIATE_DEL) {
                    /* check values */
                    if (!llly_strequal(*stritem, d->units, 1)) {
                        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, child->name);
                        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Value differs from the target being deleted.");
                        goto error;
                    }
                    /* remove current units value of the target */
                    lllydict_remove(ctx, *stritem);
                    (*stritem) = NULL;

                    /* remove its extensions */
                    j = -1;
                    while ((j = lllys_ext_iter(dev_target->ext, dev_target->ext_size, j + 1, LLLYEXT_SUBSTMT_UNITS)) != -1) {
                        lllyp_ext_instance_rm(ctx, &dev_target->ext, &dev_target->ext_size, j);
                        --j;
                    }
                }

                if (lllyp_yin_parse_subnode_ext(module, d, LLLYEXT_PAR_DEVIATE, child, LLLYEXT_SUBSTMT_UNITS, 0, unres)) {
                    goto error;
                }
            } else {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                goto error;
            }

            /* do not free sub, it could have been unlinked and stored in unres */
        }

        if (c_must) {
            /* check target node type */
            switch (dev_target->nodetype) {
            case LLLYS_LEAF:
                trg_must = &((struct lllys_node_leaf *)dev_target)->must;
                trg_must_size = &((struct lllys_node_leaf *)dev_target)->must_size;
                break;
            case LLLYS_CONTAINER:
                trg_must = &((struct lllys_node_container *)dev_target)->must;
                trg_must_size = &((struct lllys_node_container *)dev_target)->must_size;
                break;
            case LLLYS_LEAFLIST:
                trg_must = &((struct lllys_node_leaflist *)dev_target)->must;
                trg_must_size = &((struct lllys_node_leaflist *)dev_target)->must_size;
                break;
            case LLLYS_LIST:
                trg_must = &((struct lllys_node_list *)dev_target)->must;
                trg_must_size = &((struct lllys_node_list *)dev_target)->must_size;
                break;
            case LLLYS_ANYXML:
            case LLLYS_ANYDATA:
                trg_must = &((struct lllys_node_anydata *)dev_target)->must;
                trg_must_size = &((struct lllys_node_anydata *)dev_target)->must_size;
                break;
            default:
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, "must");
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Target node does not allow \"must\" property.");
                goto error;
            }

            dev_target->flags &= ~(LLLYS_XPCONF_DEP | LLLYS_XPSTATE_DEP);

            if (d->mod == LLLY_DEVIATE_RPL) {
                /* replace must is forbidden */
                LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, "must", "deviate replace");
                goto error;
            } else if (d->mod == LLLY_DEVIATE_ADD) {
                /* reallocate the must array of the target */
                struct lllys_restr *must = llly_realloc(*trg_must, (c_must + *trg_must_size) * sizeof *d->must);
                LLLY_CHECK_ERR_GOTO(!must, LOGMEM(ctx), error);
                *trg_must = must;
                d->must = calloc(c_must, sizeof *d->must);
                d->must_size = c_must;
            } else { /* LLLY_DEVIATE_DEL */
                d->must = calloc(c_must, sizeof *d->must);
            }
            LLLY_CHECK_ERR_GOTO(!d->must, LOGMEM(ctx), error);
        }
        if (c_uniq) {
            /* replace unique is forbidden */
            if (d->mod == LLLY_DEVIATE_RPL) {
                LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, "unique", "deviate replace");
                goto error;
            }

            /* check target node type */
            if (dev_target->nodetype != LLLYS_LIST) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, "unique");
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Target node does not allow \"unique\" property.");
                goto error;
            }

            list = (struct lllys_node_list *)dev_target;
            if (d->mod == LLLY_DEVIATE_ADD) {
                /* reallocate the unique array of the target */
                d->unique = llly_realloc(list->unique, (c_uniq + list->unique_size) * sizeof *d->unique);
                LLLY_CHECK_ERR_GOTO(!d->unique, LOGMEM(ctx), error);
                list->unique = d->unique;
                d->unique = &list->unique[list->unique_size];
                d->unique_size = c_uniq;
            } else { /* LLLY_DEVIATE_DEL */
                d->unique = calloc(c_uniq, sizeof *d->unique);
                LLLY_CHECK_ERR_GOTO(!d->unique, LOGMEM(ctx), error);
            }
        }
        if (c_dflt) {
            if (d->mod == LLLY_DEVIATE_ADD) {
                /* check that there is no current value */
                if ((dev_target->nodetype == LLLYS_LEAF && ((struct lllys_node_leaf *)dev_target)->dflt) ||
                        (dev_target->nodetype == LLLYS_CHOICE && ((struct lllys_node_choice *)dev_target)->dflt)) {
                    LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, "default");
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Adding property that already exists.");
                    goto error;
                }

                /* check collision with mandatory/min-elements */
                if ((dev_target->flags & LLLYS_MAND_TRUE) ||
                        (dev_target->nodetype == LLLYS_LEAFLIST && ((struct lllys_node_leaflist *)dev_target)->min)) {
                    LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, "default", "deviation");
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL,
                           "Adding the \"default\" statement is forbidden on %s statement.",
                           (dev_target->flags & LLLYS_MAND_TRUE) ? "nodes with the \"mandatory\"" : "leaflists with non-zero \"min-elements\"");
                    goto error;
                }
            } else if (d->mod == LLLY_DEVIATE_RPL) {
                /* check that there was a value before */
                if (((dev_target->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) && !((struct lllys_node_leaf *)dev_target)->dflt) ||
                        (dev_target->nodetype == LLLYS_CHOICE && !((struct lllys_node_choice *)dev_target)->dflt)) {
                    LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, "default");
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Replacing a property that does not exist.");
                    goto error;
                }
            }

            if (dev_target->nodetype == LLLYS_LEAFLIST) {
                /* reallocate default list in the target */
                llist = (struct lllys_node_leaflist *)dev_target;
                if (d->mod == LLLY_DEVIATE_ADD) {
                    /* reallocate (enlarge) the unique array of the target */
                    llist->dflt = llly_realloc(llist->dflt, (c_dflt + llist->dflt_size) * sizeof *d->dflt);
                    LLLY_CHECK_ERR_GOTO(!llist->dflt, LOGMEM(ctx), error);
                } else if (d->mod == LLLY_DEVIATE_RPL) {
                    /* reallocate (replace) the unique array of the target */
                    for (i = 0; i < llist->dflt_size; i++) {
                        lllydict_remove(ctx, llist->dflt[i]);
                    }
                    llist->dflt = llly_realloc(llist->dflt, c_dflt * sizeof *d->dflt);
                    llist->dflt_size = 0;
                    LLLY_CHECK_ERR_GOTO(!llist->dflt, LOGMEM(ctx), error);
                }
            }
            d->dflt = calloc(c_dflt, sizeof *d->dflt);
            LLLY_CHECK_ERR_GOTO(!d->dflt, LOGMEM(ctx), error);
        }
        if (c_ext) {
            /* some extensions may be already present from the substatements */
            reallocated = realloc(d->ext, (c_ext + d->ext_size) * sizeof *d->ext);
            LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
            d->ext = reallocated;

            /* init memory */
            memset(&d->ext[d->ext_size], 0, c_ext * sizeof *d->ext);
        }

        /* process deviation properties with 0..n cardinality */
        deviate_must_index = 0;
        LLLY_TREE_FOR_SAFE(develem->child, next2, child) {
            if (strcmp(child->ns->value, LLLY_NSYIN)) {
                /* extension */
                if (lllyp_yin_fill_ext(d, LLLYEXT_PAR_DEVIATE, 0, 0, module, child, &d->ext, d->ext_size, unres)) {
                    goto error;
                }
                d->ext_size++;
            } else if (!strcmp(child->name, "must")) {
                if (d->mod == LLLY_DEVIATE_DEL) {
                    if (fill_yin_must(module, child, &d->must[d->must_size], unres)) {
                        goto error;
                    }

                    /* find must to delete, we are ok with just matching conditions */
                    for (i = 0; i < *trg_must_size; i++) {
                        if (llly_strequal(d->must[d->must_size].expr, (*trg_must)[i].expr, 1)) {
                            /* we have a match, free the must structure ... */
                            lllys_restr_free(ctx, &((*trg_must)[i]), NULL);
                            /* ... and maintain the array */
                            (*trg_must_size)--;
                            if (i != *trg_must_size) {
                                (*trg_must)[i].expr = (*trg_must)[*trg_must_size].expr;
                                (*trg_must)[i].dsc = (*trg_must)[*trg_must_size].dsc;
                                (*trg_must)[i].ref = (*trg_must)[*trg_must_size].ref;
                                (*trg_must)[i].eapptag = (*trg_must)[*trg_must_size].eapptag;
                                (*trg_must)[i].emsg = (*trg_must)[*trg_must_size].emsg;
                            }
                            if (!(*trg_must_size)) {
                                free(*trg_must);
                                *trg_must = NULL;
                            } else {
                                (*trg_must)[*trg_must_size].expr = NULL;
                                (*trg_must)[*trg_must_size].dsc = NULL;
                                (*trg_must)[*trg_must_size].ref = NULL;
                                (*trg_must)[*trg_must_size].eapptag = NULL;
                                (*trg_must)[*trg_must_size].emsg = NULL;
                            }

                            i = -1; /* set match flag */
                            break;
                        }
                    }
                    d->must_size++;
                    if (i != -1) {
                        /* no match found */
                        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL,
                               d->must[d->must_size - 1].expr, child->name);
                        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Value does not match any must from the target.");
                        goto error;
                    }
                } else { /* replace or add */
                    memset(&((*trg_must)[*trg_must_size]), 0, sizeof **trg_must);
                    if (fill_yin_must(module, child, &((*trg_must)[*trg_must_size]), unres)) {
                        goto error;
                    }
                    memcpy(d->must + deviate_must_index, &((*trg_must)[*trg_must_size]), sizeof *d->must);
                    ++deviate_must_index;
                    (*trg_must_size)++;
                }

                /* check XPath dependencies again */
                if (*trg_must_size && !(ctx->models.flags & LLLY_CTX_TRUSTED) &&
                        (unres_schema_add_node(module, unres, dev_target, UNRES_XPATH, NULL) == -1)) {
                    goto error;
                }
            } else if (!strcmp(child->name, "unique")) {
                if (d->mod == LLLY_DEVIATE_DEL) {
                    memset(&d->unique[d->unique_size], 0, sizeof *d->unique);
                    if (fill_yin_unique(module, dev_target, child, &d->unique[d->unique_size], NULL)) {
                        d->unique_size++;
                        goto error;
                    }

                    /* find unique structures to delete */
                    for (i = 0; i < list->unique_size; i++) {
                        if (list->unique[i].expr_size != d->unique[d->unique_size].expr_size) {
                            continue;
                        }

                        for (j = 0; j < d->unique[d->unique_size].expr_size; j++) {
                            if (!llly_strequal(list->unique[i].expr[j], d->unique[d->unique_size].expr[j], 1)) {
                                break;
                            }
                        }

                        if (j == d->unique[d->unique_size].expr_size) {
                            /* we have a match, free the unique structure ... */
                            for (j = 0; j < list->unique[i].expr_size; j++) {
                                lllydict_remove(ctx, list->unique[i].expr[j]);
                            }
                            free(list->unique[i].expr);
                            /* ... and maintain the array */
                            list->unique_size--;
                            if (i != list->unique_size) {
                                list->unique[i].expr_size = list->unique[list->unique_size].expr_size;
                                list->unique[i].expr = list->unique[list->unique_size].expr;
                            }

                            if (!list->unique_size) {
                                free(list->unique);
                                list->unique = NULL;
                            } else {
                                list->unique[list->unique_size].expr_size = 0;
                                list->unique[list->unique_size].expr = NULL;
                            }

                            k = i; /* remember index for removing extensions */
                            i = -1; /* set match flag */
                            break;
                        }
                    }

                    d->unique_size++;
                    if (i != -1) {
                        /* no match found */
                        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, lllyxml_get_attr(child, "tag", NULL), child->name);
                        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Value differs from the target being deleted.");
                        goto error;
                    }

                    /* remove extensions of this unique instance from the target node */
                    j = -1;
                    while ((j = lllys_ext_iter(dev_target->ext, dev_target->ext_size, j + 1, LLLYEXT_SUBSTMT_UNIQUE)) != -1) {
                        if (dev_target->ext[j]->insubstmt_index == k) {
                            lllyp_ext_instance_rm(ctx, &dev_target->ext, &dev_target->ext_size, j);
                            --j;
                        } else if (dev_target->ext[j]->insubstmt_index > k) {
                            /* decrease the substatement index of the extension because of the changed array of uniques */
                            dev_target->ext[j]->insubstmt_index--;
                        }
                    }
                } else { /* replace or add */
                    memset(&list->unique[list->unique_size], 0, sizeof *list->unique);
                    i = fill_yin_unique(module, dev_target, child, &list->unique[list->unique_size], NULL);
                    list->unique_size++;
                    if (i) {
                        goto error;
                    }
                }
            } else if (!strcmp(child->name, "default")) {
                GETVAL(ctx, value, child, "value");
                u = strlen(value);
                d->dflt[d->dflt_size++] = lllydict_insert(ctx, value, u);

                if (dev_target->nodetype == LLLYS_CHOICE) {
                    choice = (struct lllys_node_choice *)dev_target;
                    rc = resolve_choice_default_schema_nodeid(value, choice->child, (const struct lllys_node **)&node);
                    if (rc || !node) {
                        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "default");
                        goto error;
                    }
                    if (d->mod == LLLY_DEVIATE_DEL) {
                        if (!choice->dflt || (choice->dflt != node)) {
                            LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "default");
                            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Value differs from the target being deleted.");
                            goto error;
                        }
                        choice->dflt = NULL;
                        /* remove extensions of this default instance from the target node */
                        j = -1;
                        while ((j = lllys_ext_iter(dev_target->ext, dev_target->ext_size, j + 1, LLLYEXT_SUBSTMT_DEFAULT)) != -1) {
                            lllyp_ext_instance_rm(ctx, &dev_target->ext, &dev_target->ext_size, j);
                            --j;
                        }
                    } else { /* add or replace */
                        choice->dflt = node;
                        if (!choice->dflt) {
                            /* default branch not found */
                            LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "default");
                            goto error;
                        }
                    }
                } else if (dev_target->nodetype == LLLYS_LEAF) {
                    leaf = (struct lllys_node_leaf *)dev_target;
                    if (d->mod == LLLY_DEVIATE_DEL) {
                        if (!leaf->dflt || !llly_strequal(leaf->dflt, value, 1)) {
                            LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "default");
                            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Value differs from the target being deleted.");
                            goto error;
                        }
                        /* remove value */
                        lllydict_remove(ctx, leaf->dflt);
                        leaf->dflt = NULL;
                        leaf->flags &= ~LLLYS_DFLTJSON;

                        /* remove extensions of this default instance from the target node */
                        j = -1;
                        while ((j = lllys_ext_iter(dev_target->ext, dev_target->ext_size, j + 1, LLLYEXT_SUBSTMT_DEFAULT)) != -1) {
                            lllyp_ext_instance_rm(ctx, &dev_target->ext, &dev_target->ext_size, j);
                            --j;
                        }
                    } else { /* add (already checked) and replace */
                        /* remove value */
                        lllydict_remove(ctx, leaf->dflt);
                        leaf->flags &= ~LLLYS_DFLTJSON;

                        /* set new value */
                        leaf->dflt = lllydict_insert(ctx, value, u);

                        /* remember to check it later (it may not fit now, because the type can be deviated too) */
                        llly_set_add(dflt_check, dev_target, 0);
                    }
                } else { /* LLLYS_LEAFLIST */
                    llist = (struct lllys_node_leaflist *)dev_target;
                    if (d->mod == LLLY_DEVIATE_DEL) {
                        /* find and remove the value in target list */
                        for (i = 0; i < llist->dflt_size; i++) {
                            if (llist->dflt[i] && llly_strequal(llist->dflt[i], value, 1)) {
                                /* match, remove the value */
                                lllydict_remove(ctx, llist->dflt[i]);
                                llist->dflt[i] = NULL;

                                /* remove extensions of this default instance from the target node */
                                j = -1;
                                while ((j = lllys_ext_iter(dev_target->ext, dev_target->ext_size, j + 1, LLLYEXT_SUBSTMT_DEFAULT)) != -1) {
                                    if (dev_target->ext[j]->insubstmt_index == i) {
                                        lllyp_ext_instance_rm(ctx, &dev_target->ext, &dev_target->ext_size, j);
                                        --j;
                                    } else if (dev_target->ext[j]->insubstmt_index > i) {
                                        /* decrease the substatement index of the extension because of the changed array of defaults */
                                        dev_target->ext[j]->insubstmt_index--;
                                    }
                                }
                                break;
                            }
                        }
                        if (i == llist->dflt_size) {
                            LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "default");
                            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "The default value to delete not found in the target node.");
                            goto error;
                        }
                    } else {
                        /* add or replace, anyway we place items into the deviate's list
                           which propagates to the target */
                        /* we just want to check that the value isn't already in the list */
                        for (i = 0; i < llist->dflt_size; i++) {
                            if (llly_strequal(llist->dflt[i], value, 1)) {
                                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "default");
                                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Duplicated default value \"%s\".", value);
                                goto error;
                            }
                        }
                        /* store it in target node */
                        llist->dflt[llist->dflt_size++] = lllydict_insert(ctx, value, u);

                        /* remember to check it later (it may not fit now, but the type can be deviated too) */
                        llly_set_add(dflt_check, dev_target, 0);
                        llist->flags &= ~LLLYS_DFLTJSON;
                    }
                }
            }
        }

        if (c_dflt && dev_target->nodetype == LLLYS_LEAFLIST && d->mod == LLLY_DEVIATE_DEL) {
            /* consolidate the final list in the target after removing items from it */
            llist = (struct lllys_node_leaflist *)dev_target;
            for (i = j = 0; j < llist->dflt_size; j++) {
                llist->dflt[i] = llist->dflt[j];
                if (llist->dflt[i]) {
                    i++;
                }
            }
            llist->dflt_size = i + 1;
        }
    }

    /* now check whether default value, if any, matches the type */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED)) {
        for (u = 0; u < dflt_check->number; ++u) {
            value = NULL;
            rc = EXIT_SUCCESS;
            if (dflt_check->set.s[u]->nodetype == LLLYS_LEAF) {
                leaf = (struct lllys_node_leaf *)dflt_check->set.s[u];
                value = leaf->dflt;
                rc = unres_schema_add_node(module, unres, &leaf->type, UNRES_TYPE_DFLT, (struct lllys_node *)(&leaf->dflt));
            } else { /* LLLYS_LEAFLIST */
                llist = (struct lllys_node_leaflist *)dflt_check->set.s[u];
                for (j = 0; j < llist->dflt_size; j++) {
                    rc = unres_schema_add_node(module, unres, &llist->type, UNRES_TYPE_DFLT,
                                            (struct lllys_node *)(&llist->dflt[j]));
                    if (rc == -1) {
                        value = llist->dflt[j];
                        break;
                    }
                }

            }
            if (rc == -1) {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "default");
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL,
                    "The default value \"%s\" of the deviated node \"%s\" no longer matches its type.",
                    dev->target_name);
                goto error;
            }
        }
    }

    /* mark all the affected modules as deviated and implemented */
    for(parent = dev_target; parent; parent = lllys_parent(parent)) {
        mod = lllys_node_module(parent);
        if (module != mod) {
            mod->deviated = 1;            /* main module */
            parent->module->deviated = 1; /* possible submodule */
            if (!mod->implemented) {
                mod->implemented = 1;
                if (unres_schema_add_node(mod, unres, NULL, UNRES_MOD_IMPLEMENT, NULL) == -1) {
                    goto error;
                }
            }
        }
    }

    llly_set_free(dflt_check);
    return EXIT_SUCCESS;

error:
    llly_set_free(dflt_check);
    return EXIT_FAILURE;
}

/* logs directly */
static int
fill_yin_augment(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin, struct lllys_node_augment *aug,
                 int options, struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    const char *value;
    struct lllyxml_elem *sub, *next;
    struct lllys_node *node;
    int ret, c_ftrs = 0, c_ext = 0;
    void *reallocated;

    aug->nodetype = LLLYS_AUGMENT;
    GETVAL(ctx, value, yin, "target-node");
    aug->target_name = transform_schema2json(module, value);
    if (!aug->target_name) {
        goto error;
    }
    aug->parent = parent;

    if (read_yin_common(module, NULL, aug, LLLYEXT_PAR_NODE, yin, OPT_MODULE, unres)) {
        goto error;
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, aug->ext_size, "extensions", "augment", error);
            c_ext++;
            continue;
        } else if (!strcmp(sub->name, "if-feature")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, aug->iffeature_size, "if-features", "augment", error);
            c_ftrs++;
            continue;
        } else if (!strcmp(sub->name, "when")) {
            if (aug->when) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                goto error;
            }

            aug->when = read_yin_when(module, sub, unres);
            if (!aug->when) {
                lllyxml_free(ctx, sub);
                goto error;
            }
            lllyxml_free(ctx, sub);
            continue;

        /* check allowed data sub-statements */
        } else if (!strcmp(sub->name, "container")) {
            node = read_yin_container(module, (struct lllys_node *)aug, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf-list")) {
            node = read_yin_leaflist(module, (struct lllys_node *)aug, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf")) {
            node = read_yin_leaf(module, (struct lllys_node *)aug, sub, options, unres);
        } else if (!strcmp(sub->name, "list")) {
            node = read_yin_list(module, (struct lllys_node *)aug, sub, options, unres);
        } else if (!strcmp(sub->name, "uses")) {
            node = read_yin_uses(module, (struct lllys_node *)aug, sub, options, unres);
        } else if (!strcmp(sub->name, "choice")) {
            node = read_yin_choice(module, (struct lllys_node *)aug, sub, options, unres);
        } else if (!strcmp(sub->name, "case")) {
            node = read_yin_case(module, (struct lllys_node *)aug, sub, options, unres);
        } else if (!strcmp(sub->name, "anyxml")) {
            node = read_yin_anydata(module, (struct lllys_node *)aug, sub, LLLYS_ANYXML, options, unres);
        } else if (!strcmp(sub->name, "anydata")) {
            node = read_yin_anydata(module, (struct lllys_node *)aug, sub, LLLYS_ANYDATA, options, unres);
        } else if (!strcmp(sub->name, "action")) {
            node = read_yin_rpc_action(module, (struct lllys_node *)aug, sub, options, unres);
        } else if (!strcmp(sub->name, "notification")) {
            node = read_yin_notif(module, (struct lllys_node *)aug, sub, options, unres);
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, sub->name);
            goto error;
        }

        if (!node) {
            goto error;
        }

        node = NULL;
        lllyxml_free(ctx, sub);
    }

    if (c_ftrs) {
        aug->iffeature = calloc(c_ftrs, sizeof *aug->iffeature);
        LLLY_CHECK_ERR_GOTO(!aug->iffeature, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(aug->ext, (c_ext + aug->ext_size) * sizeof *aug->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        aug->ext = reallocated;

        /* init memory */
        memset(&aug->ext[aug->ext_size], 0, c_ext * sizeof *aug->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            ret = lllyp_yin_fill_ext(aug, LLLYEXT_PAR_NODE, 0, 0, module, sub, &aug->ext, aug->ext_size, unres);
            aug->ext_size++;
            if (ret) {
                goto error;
            }
        } else if (!strcmp(sub->name, "if-feature")) {
            ret = fill_yin_iffeature((struct lllys_node *)aug, 0, sub, &aug->iffeature[aug->iffeature_size], unres);
            aug->iffeature_size++;
            if (ret) {
                goto error;
            }
            lllyxml_free(ctx, sub);
        }
    }

    /* aug->child points to the parsed nodes, they must now be
     * connected to the tree and adjusted (if possible right now).
     * However, if this is augment in a uses (parent is NULL), it gets resolved
     * when the uses does and cannot be resolved now for sure
     * (the grouping was not yet copied into uses).
     */
    if (!parent) {
        if (unres_schema_add_node(module, unres, aug, UNRES_AUGMENT, NULL) == -1) {
            goto error;
        }
    }

    /* check XPath dependencies */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) && aug->when) {
        if (options & LLLYS_PARSE_OPT_INGRP) {
            if (lllyxp_node_check_syntax((struct lllys_node *)aug)) {
                goto error;
            }
        } else {
            if (unres_schema_add_node(module, unres, (struct lllys_node *)aug, UNRES_XPATH, NULL) == -1) {
                goto error;
            }
        }
    }

    return EXIT_SUCCESS;

error:
    return EXIT_FAILURE;
}

/* logs directly */
static int
fill_yin_refine(struct lllys_node *uses, struct lllyxml_elem *yin, struct lllys_refine *rfn, struct unres_schema *unres)
{
    struct llly_ctx *ctx = uses->module->ctx;
    struct lllys_module *module;
    struct lllyxml_elem *sub, *next;
    const char *value;
    char *endptr;
    int f_mand = 0, f_min = 0, f_max = 0;
    int c_must = 0, c_ftrs = 0, c_dflt = 0, c_ext = 0;
    int r;
    unsigned long int val;
    void *reallocated;

    assert(uses);
    module = uses->module; /* shorthand */

    GETVAL(ctx, value, yin, "target-node");
    rfn->target_name = transform_schema2json(module, value);
    if (!rfn->target_name) {
        goto error;
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (!sub->ns) {
            /* garbage */
        } else if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, rfn->ext_size, "extensions", "refine", error);
            c_ext++;
            continue;

        } else if (!strcmp(sub->name, "description")) {
            if (rfn->dsc) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(module, rfn, LLLYEXT_PAR_REFINE, sub, LLLYEXT_SUBSTMT_DESCRIPTION, 0, unres)) {
                goto error;
            }

            rfn->dsc = read_yin_subnode(ctx, sub, "text");
            if (!rfn->dsc) {
                goto error;
            }
        } else if (!strcmp(sub->name, "reference")) {
            if (rfn->ref) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(module, rfn, LLLYEXT_PAR_REFINE, sub, LLLYEXT_SUBSTMT_REFERENCE, 0, unres)) {
                goto error;
            }

            rfn->ref = read_yin_subnode(ctx, sub, "text");
            if (!rfn->ref) {
                goto error;
            }
        } else if (!strcmp(sub->name, "config")) {
            if (rfn->flags & LLLYS_CONFIG_MASK) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                goto error;
            }
            GETVAL(ctx, value, sub, "value");
            if (!strcmp(value, "false")) {
                rfn->flags |= LLLYS_CONFIG_R;
            } else if (!strcmp(value, "true")) {
                rfn->flags |= LLLYS_CONFIG_W;
            } else {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, sub->name);
                goto error;
            }
            rfn->flags |= LLLYS_CONFIG_SET;

            if (lllyp_yin_parse_subnode_ext(module, rfn, LLLYEXT_PAR_REFINE, sub, LLLYEXT_SUBSTMT_CONFIG, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(sub->name, "default")) {
            /* leaf, leaf-list or choice */

            /* check possibility of statements combination */
            if (rfn->target_type) {
                if (c_dflt) {
                    /* multiple defaults are allowed only in leaf-list */
                    if (module->version < 2) {
                        LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                        goto error;
                    }
                    rfn->target_type &= LLLYS_LEAFLIST;
                } else {
                    if (module->version < 2) {
                        rfn->target_type &= (LLLYS_LEAF | LLLYS_CHOICE);
                    } else {
                        /* YANG 1.1 */
                        rfn->target_type &= (LLLYS_LEAFLIST | LLLYS_LEAF | LLLYS_CHOICE);
                    }
                }
                if (!rfn->target_type) {
                    LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Invalid refine target nodetype for the substatements.");
                    goto error;
                }
            } else {
                if (module->version < 2) {
                    rfn->target_type = LLLYS_LEAF | LLLYS_CHOICE;
                } else {
                    /* YANG 1.1 */
                    rfn->target_type = LLLYS_LEAFLIST | LLLYS_LEAF | LLLYS_CHOICE;
                }
            }

            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_dflt, rfn->dflt_size, "defaults", "refine", error);
            if (lllyp_yin_parse_subnode_ext(module, rfn, LLLYEXT_PAR_REFINE, sub, LLLYEXT_SUBSTMT_DEFAULT, c_dflt, unres)) {
                goto error;
            }
            c_dflt++;
            continue;
        } else if (!strcmp(sub->name, "mandatory")) {
            /* leaf, choice or anyxml */
            if (f_mand) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                goto error;
            }
            /* just checking the flags in leaf is not sufficient, we would allow
             * multiple mandatory statements with the "false" value
             */
            f_mand = 1;

            /* check possibility of statements combination */
            if (rfn->target_type) {
                rfn->target_type &= (LLLYS_LEAF | LLLYS_CHOICE | LLLYS_ANYDATA);
                if (!rfn->target_type) {
                    LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Invalid refine target nodetype for the substatements.");
                    goto error;
                }
            } else {
                rfn->target_type = LLLYS_LEAF | LLLYS_CHOICE | LLLYS_ANYDATA;
            }

            GETVAL(ctx, value, sub, "value");
            if (!strcmp(value, "true")) {
                rfn->flags |= LLLYS_MAND_TRUE;
            } else if (!strcmp(value, "false")) {
                rfn->flags |= LLLYS_MAND_FALSE;
            } else {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, sub->name);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(module, rfn, LLLYEXT_PAR_REFINE, sub, LLLYEXT_SUBSTMT_MANDATORY, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(sub->name, "min-elements")) {
            /* list or leaf-list */
            if (f_min) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                goto error;
            }
            f_min = 1;

            /* check possibility of statements combination */
            if (rfn->target_type) {
                rfn->target_type &= (LLLYS_LIST | LLLYS_LEAFLIST);
                if (!rfn->target_type) {
                    LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Invalid refine target nodetype for the substatements.");
                    goto error;
                }
            } else {
                rfn->target_type = LLLYS_LIST | LLLYS_LEAFLIST;
            }

            GETVAL(ctx, value, sub, "value");
            while (isspace(value[0])) {
                value++;
            }

            /* convert it to uint32_t */
            errno = 0;
            endptr = NULL;
            val = strtoul(value, &endptr, 10);
            if (*endptr || value[0] == '-' || errno || val > UINT32_MAX) {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, sub->name);
                goto error;
            }
            rfn->mod.list.min = (uint32_t) val;
            rfn->flags |= LLLYS_RFN_MINSET;

            if (lllyp_yin_parse_subnode_ext(module, rfn, LLLYEXT_PAR_REFINE, sub, LLLYEXT_SUBSTMT_MIN, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(sub->name, "max-elements")) {
            /* list or leaf-list */
            if (f_max) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                goto error;
            }
            f_max = 1;

            /* check possibility of statements combination */
            if (rfn->target_type) {
                rfn->target_type &= (LLLYS_LIST | LLLYS_LEAFLIST);
                if (!rfn->target_type) {
                    LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Invalid refine target nodetype for the substatements.");
                    goto error;
                }
            } else {
                rfn->target_type = LLLYS_LIST | LLLYS_LEAFLIST;
            }

            GETVAL(ctx, value, sub, "value");
            while (isspace(value[0])) {
                value++;
            }

            if (!strcmp(value, "unbounded")) {
                rfn->mod.list.max = 0;
            } else {
                /* convert it to uint32_t */
                errno = 0;
                endptr = NULL;
                val = strtoul(value, &endptr, 10);
                if (*endptr || value[0] == '-' || errno || val == 0 || val > UINT32_MAX) {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, sub->name);
                    goto error;
                }
                rfn->mod.list.max = (uint32_t) val;
            }
            rfn->flags |= LLLYS_RFN_MAXSET;

            if (lllyp_yin_parse_subnode_ext(module, rfn, LLLYEXT_PAR_REFINE, sub, LLLYEXT_SUBSTMT_MAX, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(sub->name, "presence")) {
            /* container */
            if (rfn->mod.presence) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                goto error;
            }

            /* check possibility of statements combination */
            if (rfn->target_type) {
                rfn->target_type &= LLLYS_CONTAINER;
                if (!rfn->target_type) {
                    LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Invalid refine target nodetype for the substatements.");
                    goto error;
                }
            } else {
                rfn->target_type = LLLYS_CONTAINER;
            }

            GETVAL(ctx, value, sub, "value");
            rfn->mod.presence = lllydict_insert(ctx, value, strlen(value));

            if (lllyp_yin_parse_subnode_ext(module, rfn, LLLYEXT_PAR_REFINE, sub, LLLYEXT_SUBSTMT_PRESENCE, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(sub->name, "must")) {
            /* leaf leaf-list, list, container or anyxml */
            /* check possibility of statements combination */
            if (rfn->target_type) {
                rfn->target_type &= (LLLYS_LEAF | LLLYS_LIST | LLLYS_LEAFLIST | LLLYS_CONTAINER | LLLYS_ANYDATA);
                if (!rfn->target_type) {
                    LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Invalid refine target nodetype for the substatements.");
                    goto error;
                }
            } else {
                rfn->target_type = LLLYS_LEAF | LLLYS_LIST | LLLYS_LEAFLIST | LLLYS_CONTAINER | LLLYS_ANYDATA;
            }

            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_must, rfn->must_size, "musts", "refine", error);
            c_must++;
            continue;

        } else if ((module->version >= 2) && !strcmp(sub->name, "if-feature")) {
            /* leaf, leaf-list, list, container, choice, case, anydata or anyxml */
            /* check possibility of statements combination */
            if (rfn->target_type) {
                rfn->target_type &= (LLLYS_LEAF | LLLYS_LIST | LLLYS_LEAFLIST | LLLYS_CONTAINER | LLLYS_CHOICE | LLLYS_CASE | LLLYS_ANYDATA);
                if (!rfn->target_type) {
                    LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, sub->name, yin->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Invalid refine target nodetype for the substatements.");
                    goto error;
                }
            } else {
                rfn->target_type = LLLYS_LEAF | LLLYS_LIST | LLLYS_LEAFLIST | LLLYS_CONTAINER | LLLYS_CHOICE | LLLYS_CASE | LLLYS_ANYDATA;
            }

            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, rfn->iffeature_size, "if-feature", "refine", error);
            c_ftrs++;
            continue;
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, sub->name);
            goto error;
        }

        lllyxml_free(ctx, sub);
    }

    /* process nodes with cardinality of 0..n */
    if (c_must) {
        rfn->must = calloc(c_must, sizeof *rfn->must);
        LLLY_CHECK_ERR_GOTO(!rfn->must, LOGMEM(ctx), error);
    }
    if (c_ftrs) {
        rfn->iffeature = calloc(c_ftrs, sizeof *rfn->iffeature);
        LLLY_CHECK_ERR_GOTO(!rfn->iffeature, LOGMEM(ctx), error);
    }
    if (c_dflt) {
        rfn->dflt = calloc(c_dflt, sizeof *rfn->dflt);
        LLLY_CHECK_ERR_GOTO(!rfn->dflt, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(rfn->ext, (c_ext + rfn->ext_size) * sizeof *rfn->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        rfn->ext = reallocated;

        /* init memory */
        memset(&rfn->ext[rfn->ext_size], 0, c_ext * sizeof *rfn->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            r = lllyp_yin_fill_ext(rfn, LLLYEXT_PAR_REFINE, 0, 0, module, sub, &rfn->ext, rfn->ext_size, unres);
            rfn->ext_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "if-feature")) {
            r = fill_yin_iffeature(uses, 0, sub, &rfn->iffeature[rfn->iffeature_size], unres);
            rfn->iffeature_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "must")) {
            r = fill_yin_must(module, sub, &rfn->must[rfn->must_size], unres);
            rfn->must_size++;
            if (r) {
                goto error;
            }
        } else { /* default */
            GETVAL(ctx, value, sub, "value");

            /* check for duplicity */
            for (r = 0; r < rfn->dflt_size; r++) {
                if (llly_strequal(rfn->dflt[r], value, 1)) {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "default");
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Duplicated default value \"%s\".", value);
                    goto error;
                }
            }
            rfn->dflt[rfn->dflt_size++] = lllydict_insert(ctx, value, strlen(value));
        }
    }

    return EXIT_SUCCESS;

error:
    return EXIT_FAILURE;
}

/* logs directly */
static int
fill_yin_import(struct lllys_module *module, struct lllyxml_elem *yin, struct lllys_import *imp, struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllyxml_elem *child, *next, exts;
    const char *value;
    int r, c_ext = 0;
    void *reallocated;

    /* init */
    memset(&exts, 0, sizeof exts);

    LLLY_TREE_FOR_SAFE(yin->child, next, child) {
        if (!child->ns) {
            /* garbage */
            continue;
        } else if (strcmp(child->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, imp->ext_size, "extensions", "import", error);
            c_ext++;
            lllyxml_unlink_elem(ctx, child, 2);
            lllyxml_add_child(ctx, &exts, child);
        } else if (!strcmp(child->name, "prefix")) {
            GETVAL(ctx, value, child, "value");
            if (lllyp_check_identifier(ctx, value, LLLY_IDENT_PREFIX, module, NULL)) {
                goto error;
            }
            imp->prefix = lllydict_insert(ctx, value, strlen(value));

            if (lllyp_yin_parse_subnode_ext(module, imp, LLLYEXT_PAR_IMPORT, child, LLLYEXT_SUBSTMT_PREFIX, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(child->name, "revision-date")) {
            if (imp->rev[0]) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            GETVAL(ctx, value, child, "date");
            if (lllyp_check_date(ctx, value)) {
                goto error;
            }
            memcpy(imp->rev, value, LLLY_REV_SIZE - 1);

            if (lllyp_yin_parse_subnode_ext(module, imp, LLLYEXT_PAR_IMPORT, child, LLLYEXT_SUBSTMT_REVISIONDATE, 0, unres)) {
                goto error;
            }
        } else if ((module->version >= 2) && !strcmp(child->name, "description")) {
            if (imp->dsc) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(module, imp, LLLYEXT_PAR_IMPORT, child, LLLYEXT_SUBSTMT_DESCRIPTION, 0, unres)) {
                goto error;
            }
            imp->dsc = read_yin_subnode(ctx, child, "text");
            if (!imp->dsc) {
                goto error;
            }
        } else if ((module->version >= 2) && !strcmp(child->name, "reference")) {
            if (imp->ref) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(module, imp, LLLYEXT_PAR_IMPORT, child, LLLYEXT_SUBSTMT_REFERENCE, 0, unres)) {
                goto error;
            }
            imp->ref = read_yin_subnode(ctx, child, "text");
            if (!imp->ref) {
                goto error;
            }
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
            goto error;
        }
    }

    /* check mandatory information */
    if (!imp->prefix) {
        LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "prefix", yin->name);
        goto error;
    }

    /* process extensions */
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(imp->ext, (c_ext + imp->ext_size) * sizeof *imp->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        imp->ext = reallocated;

        /* init memory */
        memset(&imp->ext[imp->ext_size], 0, c_ext * sizeof *imp->ext);

        LLLY_TREE_FOR_SAFE(exts.child, next, child) {
            /* extension */
            r = lllyp_yin_fill_ext(imp, LLLYEXT_PAR_IMPORT, 0, 0, module, child, &imp->ext, imp->ext_size, unres);
            imp->ext_size++;
            if (r) {
                goto error;
            }
        }
    }

    GETVAL(ctx, value, yin, "module");
    return lllyp_check_import(module, value, imp);

error:
    while (exts.child) {
        lllyxml_free(ctx, exts.child);
    }
    return EXIT_FAILURE;
}

/* logs directly
 * returns:
 *  0 - inc successfully filled
 * -1 - error
 */
static int
fill_yin_include(struct lllys_module *module, struct lllys_submodule *submodule, struct lllyxml_elem *yin,
                 struct lllys_include *inc, struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllyxml_elem *child, *next, exts;
    const char *value;
    int r, c_ext = 0;
    void *reallocated;

    /* init */
    memset(&exts, 0, sizeof exts);

    LLLY_TREE_FOR_SAFE(yin->child, next, child) {
        if (!child->ns) {
            /* garbage */
            continue;
        } else if (strcmp(child->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, inc->ext_size, "extensions", "include", error);
            c_ext++;
            lllyxml_unlink_elem(ctx, child, 2);
            lllyxml_add_child(ctx, &exts, child);
        } else if (!strcmp(child->name, "revision-date")) {
            if (inc->rev[0]) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, "revision-date", yin->name);
                goto error;
            }
            GETVAL(ctx, value, child, "date");
            if (lllyp_check_date(ctx, value)) {
                goto error;
            }
            memcpy(inc->rev, value, LLLY_REV_SIZE - 1);

            if (lllyp_yin_parse_subnode_ext(module, inc, LLLYEXT_PAR_INCLUDE, child, LLLYEXT_SUBSTMT_REVISIONDATE, 0, unres)) {
                goto error;
            }
        } else if ((module->version >= 2) && !strcmp(child->name, "description")) {
            if (inc->dsc) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(module, inc, LLLYEXT_PAR_INCLUDE, child, LLLYEXT_SUBSTMT_DESCRIPTION, 0, unres)) {
                goto error;
            }
            inc->dsc = read_yin_subnode(ctx, child, "text");
            if (!inc->dsc) {
                goto error;
            }
        } else if ((module->version >= 2) && !strcmp(child->name, "reference")) {
            if (inc->ref) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(module, inc, LLLYEXT_PAR_INCLUDE, child, LLLYEXT_SUBSTMT_REFERENCE, 0, unres)) {
                goto error;
            }
            inc->ref = read_yin_subnode(ctx, child, "text");
            if (!inc->ref) {
                goto error;
            }
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
            goto error;
        }
    }

    /* process extensions */
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(inc->ext, (c_ext + inc->ext_size) * sizeof *inc->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        inc->ext = reallocated;

        /* init memory */
        memset(&inc->ext[inc->ext_size], 0, c_ext * sizeof *inc->ext);

        LLLY_TREE_FOR_SAFE(exts.child, next, child) {
            /* extension */
            r = lllyp_yin_fill_ext(inc, LLLYEXT_PAR_INCLUDE, 0, 0, module, child, &inc->ext, inc->ext_size, unres);
            inc->ext_size++;
            if (r) {
                goto error;
            }
        }
    }

    GETVAL(ctx, value, yin, "module");
    return lllyp_check_include(submodule ? (struct lllys_module *)submodule : module, value, inc, unres);

error:
    return -1;
}

/* logs directly
 *
 * Covers:
 * description, reference, status, optionaly config
 *
 */
static int
read_yin_common(struct lllys_module *module, struct lllys_node *parent, void *stmt, LLLYEXT_PAR stmt_type,
                struct lllyxml_elem *xmlnode, int opt, struct unres_schema *unres)
{
    struct lllys_node *node = stmt, *p;
    const char *value;
    struct lllyxml_elem *sub, *next;
    struct llly_ctx *const ctx = module->ctx;
    char *str;

    if (opt & OPT_MODULE) {
        node->module = module;
    }

    if (opt & OPT_IDENT) {
        GETVAL(ctx, value, xmlnode, "name");
        if (lllyp_check_identifier(ctx, value, LLLY_IDENT_NAME, NULL, NULL)) {
            goto error;
        }
        node->name = lllydict_insert(ctx, value, strlen(value));
    }

    /* process local parameters */
    LLLY_TREE_FOR_SAFE(xmlnode->child, next, sub) {
        if (!sub->ns) {
            /* garbage */
            lllyxml_free(ctx, sub);
            continue;
        }
        if  (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* possibly an extension, keep the node for later processing, so skipping lllyxml_free() */
            continue;
        }

        if (!strcmp(sub->name, "description")) {
            if (node->dsc) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, sub->name, xmlnode->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(module, stmt, stmt_type, sub, LLLYEXT_SUBSTMT_DESCRIPTION, 0, unres)) {
                goto error;
            }

            node->dsc = read_yin_subnode(ctx, sub, "text");
            if (!node->dsc) {
                goto error;
            }
        } else if (!strcmp(sub->name, "reference")) {
            if (node->ref) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, sub->name, xmlnode->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(module, stmt, stmt_type, sub, LLLYEXT_SUBSTMT_REFERENCE, 0, unres)) {
                goto error;
            }

            node->ref = read_yin_subnode(ctx, sub, "text");
            if (!node->ref) {
                goto error;
            }
        } else if (!strcmp(sub->name, "status")) {
            if (node->flags & LLLYS_STATUS_MASK) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, sub->name, xmlnode->name);
                goto error;
            }
            GETVAL(ctx, value, sub, "value");
            if (!strcmp(value, "current")) {
                node->flags |= LLLYS_STATUS_CURR;
            } else if (!strcmp(value, "deprecated")) {
                node->flags |= LLLYS_STATUS_DEPRC;
            } else if (!strcmp(value, "obsolete")) {
                node->flags |= LLLYS_STATUS_OBSLT;
            } else {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, sub->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(module, stmt, stmt_type, sub, LLLYEXT_SUBSTMT_STATUS, 0, unres)) {
                goto error;
            }
        } else if ((opt & (OPT_CFG_PARSE | OPT_CFG_IGNORE)) && !strcmp(sub->name, "config")) {
            if (opt & OPT_CFG_PARSE) {
                if (node->flags & LLLYS_CONFIG_MASK) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, sub->name, xmlnode->name);
                    goto error;
                }
                GETVAL(ctx, value, sub, "value");
                if (!strcmp(value, "false")) {
                    node->flags |= LLLYS_CONFIG_R;
                } else if (!strcmp(value, "true")) {
                    node->flags |= LLLYS_CONFIG_W;
                } else {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, sub->name);
                    goto error;
                }
                node->flags |= LLLYS_CONFIG_SET;

                if (lllyp_yin_parse_subnode_ext(module, stmt, stmt_type, sub, LLLYEXT_SUBSTMT_CONFIG, 0, unres)) {
                    goto error;
                }
            }
        } else {
            /* skip the lllyxml_free */
            continue;
        }
        lllyxml_free(ctx, sub);
    }

    if ((opt & OPT_CFG_INHERIT) && !(node->flags & LLLYS_CONFIG_MASK)) {
        /* get config flag from parent */
        if (parent) {
            node->flags |= parent->flags & LLLYS_CONFIG_MASK;
        } else if (!parent) {
            /* default config is true */
            node->flags |= LLLYS_CONFIG_W;
        }
    }

    if (parent && (parent->flags & (LLLYS_STATUS_DEPRC | LLLYS_STATUS_OBSLT))) {
        /* status is not inherited by specification, but it not make sense to have
         * current in deprecated or deprecated in obsolete, so we print warning
         * and fix the schema by inheriting */
        if (!(node->flags & (LLLYS_STATUS_MASK))) {
            /* status not explicitely specified on the current node -> inherit */
            if (stmt_type == LLLYEXT_PAR_NODE) {
                p = node->parent;
                node->parent = parent;
                str = lllys_path(node, LLLYS_PATH_FIRST_PREFIX);
                node->parent = p;
            } else {
                str = lllys_path(parent, LLLYS_PATH_FIRST_PREFIX);
            }
            LOGWRN(ctx, "Missing status in %s subtree (%s), inheriting.",
                   parent->flags & LLLYS_STATUS_DEPRC ? "deprecated" : "obsolete", str);
            free(str);
            node->flags |= parent->flags & LLLYS_STATUS_MASK;
        } else if ((parent->flags & LLLYS_STATUS_MASK) > (node->flags & LLLYS_STATUS_MASK)) {
            /* invalid combination of statuses */
            switch (node->flags & LLLYS_STATUS_MASK) {
            case 0:
            case LLLYS_STATUS_CURR:
                LOGVAL(ctx, LLLYE_INSTATUS, LLLY_VLOG_LYS, parent, "current", xmlnode->name, "is child of",
                       parent->flags & LLLYS_STATUS_DEPRC ? "deprecated" : "obsolete", parent->name);
                break;
            case LLLYS_STATUS_DEPRC:
                LOGVAL(ctx, LLLYE_INSTATUS, LLLY_VLOG_LYS, parent, "deprecated", xmlnode->name, "is child of",
                       "obsolete", parent->name);
                break;
            }
            goto error;
        }
    }

    return EXIT_SUCCESS;

error:
    return EXIT_FAILURE;
}

/* logs directly */
static struct lllys_when *
read_yin_when(struct lllys_module *module, struct lllyxml_elem *yin, struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllys_when *retval = NULL;
    struct lllyxml_elem *child, *next;
    const char *value;

    retval = calloc(1, sizeof *retval);
    LLLY_CHECK_ERR_RETURN(!retval, LOGMEM(ctx), NULL);

    GETVAL(ctx, value, yin, "condition");
    retval->cond = transform_schema2json(module, value);
    if (!retval->cond) {
        goto error;
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, child) {
        if (!child->ns) {
            /* garbage */
            continue;
        } else if (strcmp(child->ns->value, LLLY_NSYIN)) {
            /* extensions */
            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_WHEN, child, LLLYEXT_SUBSTMT_SELF, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(child->name, "description")) {
            if (retval->dsc) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_WHEN, child, LLLYEXT_SUBSTMT_DESCRIPTION, 0, unres)) {
                goto error;
            }

            retval->dsc = read_yin_subnode(ctx, child, "text");
            if (!retval->dsc) {
                goto error;
            }
        } else if (!strcmp(child->name, "reference")) {
            if (retval->ref) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_WHEN, child, LLLYEXT_SUBSTMT_REFERENCE, 0, unres)) {
                goto error;
            }

            retval->ref = read_yin_subnode(ctx, child, "text");
            if (!retval->ref) {
                goto error;
            }
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
            goto error;
        }
    }

    return retval;

error:
    lllys_when_free(ctx, retval, NULL);
    return NULL;
}

/* logs directly */
static struct lllys_node *
read_yin_case(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin, int options,
              struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllyxml_elem *sub, *next, root;
    struct lllys_node_case *cs;
    struct lllys_node *retval, *node = NULL;
    int c_ftrs = 0, c_ext = 0, ret;
    void *reallocated;

    /* init */
    memset(&root, 0, sizeof root);

    cs = calloc(1, sizeof *cs);
    LLLY_CHECK_ERR_RETURN(!cs, LOGMEM(ctx), NULL);
    cs->nodetype = LLLYS_CASE;
    cs->prev = (struct lllys_node *)cs;
    retval = (struct lllys_node *)cs;

    if (read_yin_common(module, parent, retval, LLLYEXT_PAR_NODE, yin,
            OPT_IDENT | OPT_MODULE | (!(options & LLLYS_PARSE_OPT_CFG_MASK) ? OPT_CFG_INHERIT : 0), unres)) {
        goto error;
    }

    LOGDBG(LLLY_LDGYIN, "parsing %s statement \"%s\"", yin->name, retval->name);

    /* insert the node into the schema tree */
    if (lllys_node_addchild(parent, lllys_main_module(module), retval, options)) {
        goto error;
    }

    /* process choice's specific children */
    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, retval->ext_size, "extensions", "case", error);
            c_ext++;
        } else if (!strcmp(sub->name, "container") ||
                !strcmp(sub->name, "leaf-list") ||
                !strcmp(sub->name, "leaf") ||
                !strcmp(sub->name, "list") ||
                !strcmp(sub->name, "uses") ||
                !strcmp(sub->name, "choice") ||
                !strcmp(sub->name, "anyxml") ||
                !strcmp(sub->name, "anydata")) {

            lllyxml_unlink_elem(ctx, sub, 2);
            lllyxml_add_child(ctx, &root, sub);
        } else if (!strcmp(sub->name, "if-feature")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, retval->iffeature_size, "if-features", "case", error);
            c_ftrs++;
        } else if (!strcmp(sub->name, "when")) {
            if (cs->when) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }

            cs->when = read_yin_when(module, sub, unres);
            if (!cs->when) {
                goto error;
            }

            lllyxml_free(ctx, sub);
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, retval, sub->name);
            goto error;
        }
    }

    if (c_ftrs) {
        cs->iffeature = calloc(c_ftrs, sizeof *cs->iffeature);
        LLLY_CHECK_ERR_GOTO(!cs->iffeature, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(retval->ext, (c_ext + retval->ext_size) * sizeof *retval->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        retval->ext = reallocated;

        /* init memory */
        memset(&retval->ext[retval->ext_size], 0, c_ext * sizeof *retval->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            ret = lllyp_yin_fill_ext(retval, LLLYEXT_PAR_NODE, 0, 0, module, sub, &retval->ext, retval->ext_size, unres);
            retval->ext_size++;
            if (ret) {
                goto error;
            }
        } else {
            /* if-feature */
            ret = fill_yin_iffeature(retval, 0, sub, &cs->iffeature[cs->iffeature_size], unres);
            cs->iffeature_size++;
            if (ret) {
                goto error;
            }
        }
    }

    /* last part - process data nodes */
    LLLY_TREE_FOR_SAFE(root.child, next, sub) {
        if (!strcmp(sub->name, "container")) {
            node = read_yin_container(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf-list")) {
            node = read_yin_leaflist(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf")) {
            node = read_yin_leaf(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "list")) {
            node = read_yin_list(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "choice")) {
            node = read_yin_choice(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "uses")) {
            node = read_yin_uses(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "anyxml")) {
            node = read_yin_anydata(module, retval, sub, LLLYS_ANYXML, options, unres);
        } else if (!strcmp(sub->name, "anydata")) {
            node = read_yin_anydata(module, retval, sub, LLLYS_ANYDATA, options, unres);
        }
        if (!node) {
            goto error;
        }

        lllyxml_free(ctx, sub);
    }

    /* check XPath dependencies */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) && cs->when) {
        if (options & LLLYS_PARSE_OPT_INGRP) {
            if (lllyxp_node_check_syntax(retval)) {
                goto error;
            }
        } else {
            if (unres_schema_add_node(module, unres, retval, UNRES_XPATH, NULL) == -1) {
                goto error;
            }
        }
    }

    return retval;

error:
    while (root.child) {
        lllyxml_free(ctx, root.child);
    }
    lllys_node_free(retval, NULL, 0);

    return NULL;
}

/* logs directly */
static struct lllys_node *
read_yin_choice(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin, int options,
                struct unres_schema *unres)
{
    struct lllyxml_elem *sub, *next, *dflt = NULL;
    struct llly_ctx *const ctx = module->ctx;
    struct lllys_node *retval, *node = NULL;
    struct lllys_node_choice *choice;
    const char *value;
    int f_mand = 0, c_ftrs = 0, c_ext = 0, ret;
    void *reallocated;

    choice = calloc(1, sizeof *choice);
    LLLY_CHECK_ERR_RETURN(!choice, LOGMEM(ctx), NULL);

    choice->nodetype = LLLYS_CHOICE;
    choice->prev = (struct lllys_node *)choice;
    retval = (struct lllys_node *)choice;

    if (read_yin_common(module, parent, retval, LLLYEXT_PAR_NODE, yin,
            OPT_IDENT | OPT_MODULE | ((options & LLLYS_PARSE_OPT_CFG_IGNORE) ? OPT_CFG_IGNORE :
                (options & LLLYS_PARSE_OPT_CFG_NOINHERIT) ? OPT_CFG_PARSE : OPT_CFG_PARSE | OPT_CFG_INHERIT),
            unres)) {
        goto error;
    }

    LOGDBG(LLLY_LDGYIN, "parsing %s statement \"%s\"", yin->name, retval->name);

    /* insert the node into the schema tree */
    if (lllys_node_addchild(parent, lllys_main_module(module), retval, options)) {
        goto error;
    }

    /* process choice's specific children */
    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, retval->ext_size, "extensions", "choice", error);
            c_ext++;
            /* keep it for later processing, skip lllyxml_free() */
            continue;
        } else if (!strcmp(sub->name, "container")) {
            if (!(node = read_yin_container(module, retval, sub, options, unres))) {
                goto error;
            }
        } else if (!strcmp(sub->name, "leaf-list")) {
            if (!(node = read_yin_leaflist(module, retval, sub, options, unres))) {
                goto error;
            }
        } else if (!strcmp(sub->name, "leaf")) {
            if (!(node = read_yin_leaf(module, retval, sub, options, unres))) {
                goto error;
            }
        } else if (!strcmp(sub->name, "list")) {
            if (!(node = read_yin_list(module, retval, sub, options, unres))) {
                goto error;
            }
        } else if (!strcmp(sub->name, "case")) {
            if (!(node = read_yin_case(module, retval, sub, options, unres))) {
                goto error;
            }
        } else if (!strcmp(sub->name, "anyxml")) {
            if (!(node = read_yin_anydata(module, retval, sub, LLLYS_ANYXML, options, unres))) {
                goto error;
            }
        } else if (!strcmp(sub->name, "anydata")) {
            if (!(node = read_yin_anydata(module, retval, sub, LLLYS_ANYDATA, options, unres))) {
                goto error;
            }
        } else if (!strcmp(sub->name, "default")) {
            if (dflt) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_DEFAULT, 0, unres)) {
                goto error;
            }

            dflt = sub;
            lllyxml_unlink_elem(ctx, dflt, 0);
            continue;
            /* skip lllyxml_free() at the end of the loop, the sub node is processed later as dflt */

        } else if (!strcmp(sub->name, "mandatory")) {
            if (f_mand) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            /* just checking the flags in leaf is not sufficient, we would allow
             * multiple mandatory statements with the "false" value
             */
            f_mand = 1;

            GETVAL(ctx, value, sub, "value");
            if (!strcmp(value, "true")) {
                choice->flags |= LLLYS_MAND_TRUE;
            } else if (!strcmp(value, "false")) {
                choice->flags |= LLLYS_MAND_FALSE;
            } else {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, sub->name);
                goto error;
            }                   /* else false is the default value, so we can ignore it */

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_MANDATORY, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(sub->name, "when")) {
            if (choice->when) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }

            choice->when = read_yin_when(module, sub, unres);
            if (!choice->when) {
                goto error;
            }
        } else if (!strcmp(sub->name, "if-feature")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, retval->iffeature_size, "if-features", "choice", error);
            c_ftrs++;

            /* skip lllyxml_free() at the end of the loop, the sub node is processed later */
            continue;
        } else if (module->version >= 2 && !strcmp(sub->name, "choice")) {
            if (!(node = read_yin_choice(module, retval, sub, options, unres))) {
                goto error;
            }
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, retval, sub->name);
            goto error;
        }

        node = NULL;
        lllyxml_free(ctx, sub);
    }

    if (c_ftrs) {
        choice->iffeature = calloc(c_ftrs, sizeof *choice->iffeature);
        LLLY_CHECK_ERR_GOTO(!choice->iffeature, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(retval->ext, (c_ext + retval->ext_size) * sizeof *retval->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        retval->ext = reallocated;

        /* init memory */
        memset(&retval->ext[retval->ext_size], 0, c_ext * sizeof *retval->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            ret = lllyp_yin_fill_ext(retval, LLLYEXT_PAR_NODE, 0, 0, module, sub, &retval->ext, retval->ext_size, unres);
            retval->ext_size++;
            if (ret) {
                goto error;
            }
        } else {
            ret = fill_yin_iffeature(retval, 0, sub, &choice->iffeature[choice->iffeature_size], unres);
            choice->iffeature_size++;
            if (ret) {
                goto error;
            }
        }
    }

    /* check - default is prohibited in combination with mandatory */
    if (dflt && (choice->flags & LLLYS_MAND_TRUE)) {
        LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_LYS, retval, "default", "choice");
        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "The \"default\" statement is forbidden on choices with \"mandatory\".");
        goto error;
    }

    /* link default with the case */
    if (dflt) {
        GETVAL(ctx, value, dflt, "value");
        if (unres_schema_add_str(module, unres, choice, UNRES_CHOICE_DFLT, value) == -1) {
            goto error;
        }
        lllyxml_free(ctx, dflt);
    }

    /* check XPath dependencies */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) && choice->when) {
        if (options & LLLYS_PARSE_OPT_INGRP) {
            if (lllyxp_node_check_syntax(retval)) {
                goto error;
            }
        } else {
            if (unres_schema_add_node(module, unres, retval, UNRES_XPATH, NULL) == -1) {
                goto error;
            }
        }
    }

    return retval;

error:
    lllyxml_free(ctx, dflt);
    lllys_node_free(retval, NULL, 0);
    return NULL;
}

/* logs directly */
static struct lllys_node *
read_yin_anydata(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin, LLLYS_NODE type,
                 int options, struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllys_node *retval;
    struct lllys_node_anydata *anyxml;
    struct lllyxml_elem *sub, *next;
    const char *value;
    int r;
    int f_mand = 0;
    int c_must = 0, c_ftrs = 0, c_ext = 0;
    void *reallocated;

    anyxml = calloc(1, sizeof *anyxml);
    LLLY_CHECK_ERR_RETURN(!anyxml, LOGMEM(ctx), NULL);

    anyxml->nodetype = type;
    anyxml->prev = (struct lllys_node *)anyxml;
    retval = (struct lllys_node *)anyxml;

    if (read_yin_common(module, parent, retval, LLLYEXT_PAR_NODE, yin,
            OPT_IDENT | OPT_MODULE | ((options & LLLYS_PARSE_OPT_CFG_IGNORE) ? OPT_CFG_IGNORE :
            (options & LLLYS_PARSE_OPT_CFG_NOINHERIT) ? OPT_CFG_PARSE : OPT_CFG_PARSE | OPT_CFG_INHERIT), unres)) {
        goto error;
    }

    LOGDBG(LLLY_LDGYIN, "parsing %s statement \"%s\"", yin->name, retval->name);

    /* insert the node into the schema tree */
    if (lllys_node_addchild(parent, lllys_main_module(module), retval, options)) {
        goto error;
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, retval->ext_size, "extensions", "anydata", error);
            c_ext++;
        } else if (!strcmp(sub->name, "mandatory")) {
            if (f_mand) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            /* just checking the flags in leaf is not sufficient, we would allow
             * multiple mandatory statements with the "false" value
             */
            f_mand = 1;

            GETVAL(ctx, value, sub, "value");
            if (!strcmp(value, "true")) {
                anyxml->flags |= LLLYS_MAND_TRUE;
            } else if (!strcmp(value, "false")) {
                anyxml->flags |= LLLYS_MAND_FALSE;
            } else {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, sub->name);
                goto error;
            }
            /* else false is the default value, so we can ignore it */

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_MANDATORY, 0, unres)) {
                goto error;
            }
            lllyxml_free(ctx, sub);
        } else if (!strcmp(sub->name, "when")) {
            if (anyxml->when) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }

            anyxml->when = read_yin_when(module, sub, unres);
            if (!anyxml->when) {
                lllyxml_free(ctx, sub);
                goto error;
            }
            lllyxml_free(ctx, sub);
        } else if (!strcmp(sub->name, "must")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_must, anyxml->must_size, "musts", "anydata", error);
            c_must++;
        } else if (!strcmp(sub->name, "if-feature")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, retval->iffeature_size, "if-features", "anydata", error);
            c_ftrs++;

        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, retval, sub->name);
            goto error;
        }
    }

    /* middle part - process nodes with cardinality of 0..n */
    if (c_must) {
        anyxml->must = calloc(c_must, sizeof *anyxml->must);
        LLLY_CHECK_ERR_GOTO(!anyxml->must, LOGMEM(ctx), error);
    }
    if (c_ftrs) {
        anyxml->iffeature = calloc(c_ftrs, sizeof *anyxml->iffeature);
        LLLY_CHECK_ERR_GOTO(!anyxml->iffeature, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(retval->ext, (c_ext + retval->ext_size) * sizeof *retval->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        retval->ext = reallocated;

        /* init memory */
        memset(&retval->ext[retval->ext_size], 0, c_ext * sizeof *retval->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            r = lllyp_yin_fill_ext(retval, LLLYEXT_PAR_NODE, 0, 0, module, sub, &retval->ext, retval->ext_size, unres);
            retval->ext_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "must")) {
            r = fill_yin_must(module, sub, &anyxml->must[anyxml->must_size], unres);
            anyxml->must_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "if-feature")) {
            r = fill_yin_iffeature(retval, 0, sub, &anyxml->iffeature[anyxml->iffeature_size], unres);
            anyxml->iffeature_size++;
            if (r) {
                goto error;
            }
        }
    }

    /* check XPath dependencies */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) && (anyxml->when || anyxml->must)) {
        if (options & LLLYS_PARSE_OPT_INGRP) {
            if (lllyxp_node_check_syntax(retval)) {
                goto error;
            }
        } else {
            if (unres_schema_add_node(module, unres, retval, UNRES_XPATH, NULL) == -1) {
                goto error;
            }
        }
    }

    for (r = 0; r < retval->ext_size; ++r) {
        /* set flag, which represent LLLYEXT_OPT_VALID */
        if (retval->ext[r]->flags & LLLYEXT_OPT_VALID) {
            retval->flags |= LLLYS_VALID_EXT;
            break;
        }
    }

    return retval;

error:
    lllys_node_free(retval, NULL, 0);
    return NULL;
}

/* logs directly */
static struct lllys_node *
read_yin_leaf(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin, int options,
              struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllys_node *retval;
    struct lllys_node_leaf *leaf;
    struct lllyxml_elem *sub, *next;
    const char *value;
    int r, has_type = 0;
    int c_must = 0, c_ftrs = 0, f_mand = 0, c_ext = 0;
    void *reallocated;

    leaf = calloc(1, sizeof *leaf);
    LLLY_CHECK_ERR_RETURN(!leaf, LOGMEM(ctx), NULL);

    leaf->nodetype = LLLYS_LEAF;
    leaf->prev = (struct lllys_node *)leaf;
    retval = (struct lllys_node *)leaf;

    if (read_yin_common(module, parent, retval, LLLYEXT_PAR_NODE, yin,
            OPT_IDENT | OPT_MODULE | ((options & LLLYS_PARSE_OPT_CFG_IGNORE) ? OPT_CFG_IGNORE :
                (options & LLLYS_PARSE_OPT_CFG_NOINHERIT) ? OPT_CFG_PARSE : OPT_CFG_PARSE | OPT_CFG_INHERIT),
            unres)) {
        goto error;
    }

    LOGDBG(LLLY_LDGYIN, "parsing %s statement \"%s\"", yin->name, retval->name);

    /* insert the node into the schema tree */
    if (lllys_node_addchild(parent, lllys_main_module(module), retval, options)) {
        goto error;
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, retval->ext_size, "extensions", "leaf", error);
            c_ext++;
            continue;
        } else if (!strcmp(sub->name, "type")) {
            if (has_type) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            /* HACK for unres */
            leaf->type.der = (struct lllys_tpdf *)sub;
            leaf->type.parent = (struct lllys_tpdf *)leaf;
            /* postpone type resolution when if-feature parsing is done since we need
             * if-feature for check_leafref_features() */
            has_type = 1;
        } else if (!strcmp(sub->name, "default")) {
            if (leaf->dflt) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            GETVAL(ctx, value, sub, "value");
            leaf->dflt = lllydict_insert(ctx, value, strlen(value));

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_DEFAULT, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(sub->name, "units")) {
            if (leaf->units) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            GETVAL(ctx, value, sub, "name");
            leaf->units = lllydict_insert(ctx, value, strlen(value));

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_UNITS, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(sub->name, "mandatory")) {
            if (f_mand) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            /* just checking the flags in leaf is not sufficient, we would allow
             * multiple mandatory statements with the "false" value
             */
            f_mand = 1;

            GETVAL(ctx, value, sub, "value");
            if (!strcmp(value, "true")) {
                leaf->flags |= LLLYS_MAND_TRUE;
            } else if (!strcmp(value, "false")) {
                leaf->flags |= LLLYS_MAND_FALSE;
            } else {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, sub->name);
                goto error;
            }                   /* else false is the default value, so we can ignore it */

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_MANDATORY, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(sub->name, "when")) {
            if (leaf->when) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }

            leaf->when = read_yin_when(module, sub, unres);
            if (!leaf->when) {
                goto error;
            }

        } else if (!strcmp(sub->name, "must")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_must, leaf->must_size, "musts", "leaf", error);
            c_must++;
            continue;
        } else if (!strcmp(sub->name, "if-feature")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, retval->iffeature_size, "musts", "leaf", error);
            c_ftrs++;
            continue;

        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, retval, sub->name);
            goto error;
        }

        /* do not free sub, it could have been unlinked and stored in unres */
    }

    /* check mandatory parameters */
    if (!has_type) {
        LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_LYS, retval, "type", yin->name);
        goto error;
    }
    if (leaf->dflt && (leaf->flags & LLLYS_MAND_TRUE)) {
        LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_LYS, retval, "mandatory", "leaf");
        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL,
               "The \"mandatory\" statement is forbidden on leaf with the \"default\" statement.");
        goto error;
    }

    /* middle part - process nodes with cardinality of 0..n */
    if (c_must) {
        leaf->must = calloc(c_must, sizeof *leaf->must);
        LLLY_CHECK_ERR_GOTO(!leaf->must, LOGMEM(ctx), error);
    }
    if (c_ftrs) {
        leaf->iffeature = calloc(c_ftrs, sizeof *leaf->iffeature);
        LLLY_CHECK_ERR_GOTO(!leaf->iffeature, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(retval->ext, (c_ext + retval->ext_size) * sizeof *retval->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        retval->ext = reallocated;

        /* init memory */
        memset(&retval->ext[retval->ext_size], 0, c_ext * sizeof *retval->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            r = lllyp_yin_fill_ext(retval, LLLYEXT_PAR_NODE, 0, 0, module, sub, &retval->ext, retval->ext_size, unres);
            retval->ext_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "must")) {
            r = fill_yin_must(module, sub, &leaf->must[leaf->must_size], unres);
            leaf->must_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "if-feature")) {
            r = fill_yin_iffeature(retval, 0, sub, &leaf->iffeature[leaf->iffeature_size], unres);
            leaf->iffeature_size++;
            if (r) {
                goto error;
            }
        }
    }

    /* finalize type parsing */
    if (unres_schema_add_node(module, unres, &leaf->type, UNRES_TYPE_DER, retval) == -1) {
        leaf->type.der = NULL;
        goto error;
    }

    /* check default value (if not defined, there still could be some restrictions
     * that need to be checked against a default value from a derived type) */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) &&
            (unres_schema_add_node(module, unres, &leaf->type, UNRES_TYPE_DFLT,
                                   (struct lllys_node *)(&leaf->dflt)) == -1)) {
        goto error;
    }

    /* check XPath dependencies */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) && (leaf->when || leaf->must)) {
        if (options & LLLYS_PARSE_OPT_INGRP) {
            if (lllyxp_node_check_syntax(retval)) {
                goto error;
            }
        } else {
            if (unres_schema_add_node(module, unres, retval, UNRES_XPATH, NULL) == -1) {
                goto error;
            }
        }
    }

    for (r = 0; r < retval->ext_size; ++r) {
        /* set flag, which represent LLLYEXT_OPT_VALID */
        if (retval->ext[r]->flags & LLLYEXT_OPT_VALID) {
            retval->flags |= LLLYS_VALID_EXT;
            break;
        }
    }

    return retval;

error:
    lllys_node_free(retval, NULL, 0);
    return NULL;
}

/* logs directly */
static struct lllys_node *
read_yin_leaflist(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin, int options,
                  struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllys_node *retval;
    struct lllys_node_leaflist *llist;
    struct lllyxml_elem *sub, *next;
    const char *value;
    char *endptr;
    unsigned long val;
    int r, has_type = 0;
    int c_must = 0, c_ftrs = 0, c_dflt = 0, c_ext = 0;
    int f_ordr = 0, f_min = 0, f_max = 0;
    void *reallocated;

    llist = calloc(1, sizeof *llist);
    LLLY_CHECK_ERR_RETURN(!llist, LOGMEM(ctx), NULL);

    llist->nodetype = LLLYS_LEAFLIST;
    llist->prev = (struct lllys_node *)llist;
    retval = (struct lllys_node *)llist;

    if (read_yin_common(module, parent, retval, LLLYEXT_PAR_NODE, yin,
            OPT_IDENT | OPT_MODULE | ((options & LLLYS_PARSE_OPT_CFG_IGNORE) ? OPT_CFG_IGNORE :
                (options & LLLYS_PARSE_OPT_CFG_NOINHERIT) ? OPT_CFG_PARSE : OPT_CFG_PARSE | OPT_CFG_INHERIT),
            unres)) {
        goto error;
    }

    LOGDBG(LLLY_LDGYIN, "parsing %s statement \"%s\"", yin->name, retval->name);

    /* insert the node into the schema tree */
    if (lllys_node_addchild(parent, lllys_main_module(module), retval, options)) {
        goto error;
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, retval->ext_size, "extensions", "leaf-list", error);
            c_ext++;
            continue;
        } else if (!strcmp(sub->name, "type")) {
            if (has_type) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            /* HACK for unres */
            llist->type.der = (struct lllys_tpdf *)sub;
            llist->type.parent = (struct lllys_tpdf *)llist;
            /* postpone type resolution when if-feature parsing is done since we need
             * if-feature for check_leafref_features() */
            has_type = 1;
        } else if (!strcmp(sub->name, "units")) {
            if (llist->units) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            GETVAL(ctx, value, sub, "name");
            llist->units = lllydict_insert(ctx, value, strlen(value));

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_UNITS, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(sub->name, "ordered-by")) {
            if (f_ordr) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            /* just checking the flags in llist is not sufficient, we would
             * allow multiple ordered-by statements with the "system" value
             */
            f_ordr = 1;

            if (llist->flags & LLLYS_CONFIG_R) {
                /* RFC 6020, 7.7.5 - ignore ordering when the list represents
                 * state data
                 */
                lllyxml_free(ctx, sub);
                continue;
            }

            GETVAL(ctx, value, sub, "value");
            if (!strcmp(value, "user")) {
                llist->flags |= LLLYS_USERORDERED;
            } else if (strcmp(value, "system")) {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, sub->name);
                goto error;
            } /* else system is the default value, so we can ignore it */

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_ORDEREDBY, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(sub->name, "must")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_must, llist->must_size, "musts", "leaf-list", error);
            c_must++;
            continue;
        } else if (!strcmp(sub->name, "if-feature")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, retval->iffeature_size, "if-features", "leaf-list", error);
            c_ftrs++;
            continue;
        } else if ((module->version >= 2) && !strcmp(sub->name, "default")) {
            /* read the default's extension instances */
            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_DEFAULT, c_dflt, unres)) {
                goto error;
            }

            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_dflt, llist->dflt_size, "defaults", "leaf-list", error);
            c_dflt++;
            continue;

        } else if (!strcmp(sub->name, "min-elements")) {
            if (f_min) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            f_min = 1;

            GETVAL(ctx, value, sub, "value");
            while (isspace(value[0])) {
                value++;
            }

            /* convert it to uint32_t */
            errno = 0;
            endptr = NULL;
            val = strtoul(value, &endptr, 10);
            if (*endptr || value[0] == '-' || errno || val > UINT32_MAX) {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, sub->name);
                goto error;
            }
            llist->min = (uint32_t) val;
            if (llist->max && (llist->min > llist->max)) {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, sub->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "\"min-elements\" is bigger than \"max-elements\".");
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_MIN, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(sub->name, "max-elements")) {
            if (f_max) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            f_max = 1;

            GETVAL(ctx, value, sub, "value");
            while (isspace(value[0])) {
                value++;
            }

            if (!strcmp(value, "unbounded")) {
                llist->max = 0;
            } else {
                /* convert it to uint32_t */
                errno = 0;
                endptr = NULL;
                val = strtoul(value, &endptr, 10);
                if (*endptr || value[0] == '-' || errno || val == 0 || val > UINT32_MAX) {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, sub->name);
                    goto error;
                }
                llist->max = (uint32_t) val;
                if (llist->min > llist->max) {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, sub->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "\"max-elements\" is smaller than \"min-elements\".");
                    goto error;
                }
            }

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_MAX, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(sub->name, "when")) {
            if (llist->when) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }

            llist->when = read_yin_when(module, sub, unres);
            if (!llist->when) {
                goto error;
            }
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, retval, sub->name);
            goto error;
        }

        /* do not free sub, it could have been unlinked and stored in unres */
    }

    /* check constraints */
    if (!has_type) {
        LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_LYS, retval, "type", yin->name);
        goto error;
    }

    /* middle part - process nodes with cardinality of 0..n */
    if (c_must) {
        llist->must = calloc(c_must, sizeof *llist->must);
        LLLY_CHECK_ERR_GOTO(!llist->must, LOGMEM(ctx), error);
    }
    if (c_ftrs) {
        llist->iffeature = calloc(c_ftrs, sizeof *llist->iffeature);
        LLLY_CHECK_ERR_GOTO(!llist->iffeature, LOGMEM(ctx), error);
    }
    if (c_dflt) {
        llist->dflt = calloc(c_dflt, sizeof *llist->dflt);
        LLLY_CHECK_ERR_GOTO(!llist->dflt, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(retval->ext, (c_ext + retval->ext_size) * sizeof *retval->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        retval->ext = reallocated;

        /* init memory */
        memset(&retval->ext[retval->ext_size], 0, c_ext * sizeof *retval->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            r = lllyp_yin_fill_ext(retval, LLLYEXT_PAR_NODE, 0, 0, module, sub, &retval->ext, retval->ext_size, unres);
            retval->ext_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "must")) {
            r = fill_yin_must(module, sub, &llist->must[llist->must_size], unres);
            llist->must_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "if-feature")) {
            r = fill_yin_iffeature(retval, 0, sub, &llist->iffeature[llist->iffeature_size], unres);
            llist->iffeature_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "default")) {
            GETVAL(ctx, value, sub, "value");

            /* check for duplicity in case of configuration data,
             * in case of status data duplicities are allowed */
            if (llist->flags & LLLYS_CONFIG_W) {
                for (r = 0; r < llist->dflt_size; r++) {
                    if (llly_strequal(llist->dflt[r], value, 1)) {
                        LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, "default");
                        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "Duplicated default value \"%s\".", value);
                        goto error;
                    }
                }
            }
            llist->dflt[llist->dflt_size++] = lllydict_insert(ctx, value, strlen(value));
        }
    }

    /* finalize type parsing */
    if (unres_schema_add_node(module, unres, &llist->type, UNRES_TYPE_DER, retval) == -1) {
        llist->type.der = NULL;
        goto error;
    }

    if (llist->dflt_size && llist->min) {
        LOGVAL(ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_LYS, retval, "min-elements", "leaf-list");
        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL,
               "The \"min-elements\" statement with non-zero value is forbidden on leaf-lists with the \"default\" statement.");
        goto error;
    }

    /* check default value (if not defined, there still could be some restrictions
     * that need to be checked against a default value from a derived type) */
    for (r = 0; r < llist->dflt_size; r++) {
        if (!(ctx->models.flags & LLLY_CTX_TRUSTED) &&
                (unres_schema_add_node(module, unres, &llist->type, UNRES_TYPE_DFLT,
                                       (struct lllys_node *)(&llist->dflt[r])) == -1)) {
            goto error;
        }
    }

    /* check XPath dependencies */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) && (llist->when || llist->must)) {
        if (options & LLLYS_PARSE_OPT_INGRP) {
            if (lllyxp_node_check_syntax(retval)) {
                goto error;
            }
        } else {
            if (unres_schema_add_node(module, unres, retval, UNRES_XPATH, NULL) == -1) {
                goto error;
            }
        }
    }

    for (r = 0; r < retval->ext_size; ++r) {
        /* set flag, which represent LLLYEXT_OPT_VALID */
        if (retval->ext[r]->flags & LLLYEXT_OPT_VALID) {
            retval->flags |= LLLYS_VALID_EXT;
            break;
        }
    }

    return retval;

error:
    lllys_node_free(retval, NULL, 0);
    return NULL;
}

/* logs directly */
static struct lllys_node *
read_yin_list(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin, int options,
              struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllys_node *retval, *node;
    struct lllys_node_list *list;
    struct lllyxml_elem *sub, *next, root, uniq;
    int r;
    int c_tpdf = 0, c_must = 0, c_uniq = 0, c_ftrs = 0, c_ext = 0;
    int f_ordr = 0, f_max = 0, f_min = 0;
    const char *value;
    char *auxs;
    unsigned long val;
    void *reallocated;

    /* init */
    memset(&root, 0, sizeof root);
    memset(&uniq, 0, sizeof uniq);

    list = calloc(1, sizeof *list);
    LLLY_CHECK_ERR_RETURN(!list, LOGMEM(ctx), NULL);

    list->nodetype = LLLYS_LIST;
    list->prev = (struct lllys_node *)list;
    retval = (struct lllys_node *)list;

    if (read_yin_common(module, parent, retval, LLLYEXT_PAR_NODE, yin,
            OPT_IDENT | OPT_MODULE | ((options & LLLYS_PARSE_OPT_CFG_IGNORE) ? OPT_CFG_IGNORE :
                (options & LLLYS_PARSE_OPT_CFG_NOINHERIT) ? OPT_CFG_PARSE : OPT_CFG_PARSE | OPT_CFG_INHERIT),
            unres)) {
        goto error;
    }

    LOGDBG(LLLY_LDGYIN, "parsing %s statement \"%s\"", yin->name, retval->name);

    /* insert the node into the schema tree */
    if (lllys_node_addchild(parent, lllys_main_module(module), retval, options)) {
        goto error;
    }

    /* process list's specific children */
    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, retval->ext_size, "extensions", "list", error);
            c_ext++;
            continue;

        /* data statements */
        } else if (!strcmp(sub->name, "container") ||
                !strcmp(sub->name, "leaf-list") ||
                !strcmp(sub->name, "leaf") ||
                !strcmp(sub->name, "list") ||
                !strcmp(sub->name, "choice") ||
                !strcmp(sub->name, "uses") ||
                !strcmp(sub->name, "grouping") ||
                !strcmp(sub->name, "anyxml") ||
                !strcmp(sub->name, "anydata") ||
                !strcmp(sub->name, "action") ||
                !strcmp(sub->name, "notification")) {
            lllyxml_unlink_elem(ctx, sub, 2);
            lllyxml_add_child(ctx, &root, sub);

            /* array counters */
        } else if (!strcmp(sub->name, "key")) {
            /* check cardinality 0..1 */
            if (list->keys_size) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, list->name);
                goto error;
            }

            /* count the number of keys */
            GETVAL(ctx, value, sub, "value");
            list->keys_str = lllydict_insert(ctx, value, 0);
            while ((value = strpbrk(value, " \t\n"))) {
                list->keys_size++;
                while (isspace(*value)) {
                    value++;
                }
            }
            list->keys_size++;
            list->keys = calloc(list->keys_size, sizeof *list->keys);
            LLLY_CHECK_ERR_GOTO(!list->keys, LOGMEM(ctx), error);

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_KEY, 0, unres)) {
                goto error;
            }
            lllyxml_free(ctx, sub);
        } else if (!strcmp(sub->name, "unique")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_uniq, list->unique_size, "uniques", "list", error);
            c_uniq++;
            lllyxml_unlink_elem(ctx, sub, 2);
            lllyxml_add_child(ctx, &uniq, sub);
        } else if (!strcmp(sub->name, "typedef")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_tpdf, list->tpdf_size, "typedefs", "list", error);
            c_tpdf++;
        } else if (!strcmp(sub->name, "must")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_must, list->must_size, "musts", "list", error);
            c_must++;
        } else if (!strcmp(sub->name, "if-feature")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, retval->iffeature_size, "if-features", "list", error);
            c_ftrs++;

            /* optional stetments */
        } else if (!strcmp(sub->name, "ordered-by")) {
            if (f_ordr) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            /* just checking the flags in llist is not sufficient, we would
             * allow multiple ordered-by statements with the "system" value
             */
            f_ordr = 1;

            if (list->flags & LLLYS_CONFIG_R) {
                /* RFC 6020, 7.7.5 - ignore ordering when the list represents
                 * state data
                 */
                lllyxml_free(ctx, sub);
                continue;
            }

            GETVAL(ctx, value, sub, "value");
            if (!strcmp(value, "user")) {
                list->flags |= LLLYS_USERORDERED;
            } else if (strcmp(value, "system")) {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, sub->name);
                goto error;
            } /* else system is the default value, so we can ignore it */

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_ORDEREDBY, 0, unres)) {
                goto error;
            }
            lllyxml_free(ctx, sub);
        } else if (!strcmp(sub->name, "min-elements")) {
            if (f_min) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            f_min = 1;

            GETVAL(ctx, value, sub, "value");
            while (isspace(value[0])) {
                value++;
            }

            /* convert it to uint32_t */
            errno = 0;
            auxs = NULL;
            val = strtoul(value, &auxs, 10);
            if (*auxs || value[0] == '-' || errno || val > UINT32_MAX) {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, sub->name);
                goto error;
            }
            list->min = (uint32_t) val;
            if (list->max && (list->min > list->max)) {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, sub->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "\"min-elements\" is bigger than \"max-elements\".");
                lllyxml_free(ctx, sub);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_MIN, 0, unres)) {
                goto error;
            }
            lllyxml_free(ctx, sub);
        } else if (!strcmp(sub->name, "max-elements")) {
            if (f_max) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            f_max = 1;

            GETVAL(ctx, value, sub, "value");
            while (isspace(value[0])) {
                value++;
            }

            if (!strcmp(value, "unbounded")) {
                list->max = 0;;
            } else {
                /* convert it to uint32_t */
                errno = 0;
                auxs = NULL;
                val = strtoul(value, &auxs, 10);
                if (*auxs || value[0] == '-' || errno || val == 0 || val > UINT32_MAX) {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, sub->name);
                    goto error;
                }
                list->max = (uint32_t) val;
                if (list->min > list->max) {
                    LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_LYS, retval, value, sub->name);
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "\"max-elements\" is smaller than \"min-elements\".");
                    goto error;
                }
            }
            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_MAX, 0, unres)) {
                goto error;
            }
            lllyxml_free(ctx, sub);
        } else if (!strcmp(sub->name, "when")) {
            if (list->when) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }

            list->when = read_yin_when(module, sub, unres);
            if (!list->when) {
                lllyxml_free(ctx, sub);
                goto error;
            }
            lllyxml_free(ctx, sub);
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, retval, sub->name);
            goto error;
        }
    }

    /* check - if list is configuration, key statement is mandatory
     * (but only if we are not in a grouping or augment, then the check is deferred) */
    for (node = retval; node && !(node->nodetype & (LLLYS_GROUPING | LLLYS_AUGMENT | LLLYS_EXT)); node = node->parent);
    if (!node && (list->flags & LLLYS_CONFIG_W) && !list->keys_str) {
        LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_LYS, retval, "key", "list");
        goto error;
    }

    /* middle part - process nodes with cardinality of 0..n except the data nodes */
    if (c_tpdf) {
        list->tpdf = calloc(c_tpdf, sizeof *list->tpdf);
        LLLY_CHECK_ERR_GOTO(!list->tpdf, LOGMEM(ctx), error);
    }
    if (c_must) {
        list->must = calloc(c_must, sizeof *list->must);
        LLLY_CHECK_ERR_GOTO(!list->must, LOGMEM(ctx), error);
    }
    if (c_ftrs) {
        list->iffeature = calloc(c_ftrs, sizeof *list->iffeature);
        LLLY_CHECK_ERR_GOTO(!list->iffeature, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(retval->ext, (c_ext + retval->ext_size) * sizeof *retval->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        retval->ext = reallocated;

        /* init memory */
        memset(&retval->ext[retval->ext_size], 0, c_ext * sizeof *retval->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            r = lllyp_yin_fill_ext(retval, LLLYEXT_PAR_NODE, 0, 0, module, sub, &retval->ext, retval->ext_size, unres);
            retval->ext_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "typedef")) {
            r = fill_yin_typedef(module, retval, sub, &list->tpdf[list->tpdf_size], unres);
            list->tpdf_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "if-feature")) {
            r = fill_yin_iffeature(retval, 0, sub, &list->iffeature[list->iffeature_size], unres);
            list->iffeature_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "must")) {
            r = fill_yin_must(module, sub, &list->must[list->must_size], unres);
            list->must_size++;
            if (r) {
                goto error;
            }
        }
    }

    /* last part - process data nodes */
    LLLY_TREE_FOR_SAFE(root.child, next, sub) {
        if (!strcmp(sub->name, "container")) {
            node = read_yin_container(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf-list")) {
            node = read_yin_leaflist(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf")) {
            node = read_yin_leaf(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "list")) {
            node = read_yin_list(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "choice")) {
            node = read_yin_choice(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "uses")) {
            node = read_yin_uses(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "grouping")) {
            node = read_yin_grouping(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "anyxml")) {
            node = read_yin_anydata(module, retval, sub, LLLYS_ANYXML, options, unres);
        } else if (!strcmp(sub->name, "anydata")) {
            node = read_yin_anydata(module, retval, sub, LLLYS_ANYDATA, options, unres);
        } else if (!strcmp(sub->name, "action")) {
            node = read_yin_rpc_action(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "notification")) {
            node = read_yin_notif(module, retval, sub, options, unres);
        } else {
            LOGINT(ctx);
            goto error;
        }
        if (!node) {
            goto error;
        }

        lllyxml_free(ctx, sub);
    }

    if (list->keys_str) {
        if (unres_schema_add_node(module, unres, list, UNRES_LIST_KEYS, NULL) == -1) {
            goto error;
        }
    } /* else config false list without a key, key_str presence in case of config true is checked earlier */

    /* process unique statements */
    if (c_uniq) {
        list->unique = calloc(c_uniq, sizeof *list->unique);
        LLLY_CHECK_ERR_GOTO(!list->unique, LOGMEM(ctx), error);

        LLLY_TREE_FOR_SAFE(uniq.child, next, sub) {
            r = fill_yin_unique(module, retval, sub, &list->unique[list->unique_size], unres);
            list->unique_size++;
            if (r) {
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub,
                                     LLLYEXT_SUBSTMT_UNIQUE, list->unique_size - 1, unres)) {
                goto error;
            }
            lllyxml_free(ctx, sub);
        }
    }

    /* check XPath dependencies */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) && (list->when || list->must)) {
        if (options & LLLYS_PARSE_OPT_INGRP) {
            if (lllyxp_node_check_syntax(retval)) {
                goto error;
            }
        } else {
            if (unres_schema_add_node(module, unres, retval, UNRES_XPATH, NULL) == -1) {
                goto error;
            }
        }
    }

    for (r = 0; r < retval->ext_size; ++r) {
        /* set flag, which represent LLLYEXT_OPT_VALID */
        if (retval->ext[r]->flags & LLLYEXT_OPT_VALID) {
            retval->flags |= LLLYS_VALID_EXT;
            if (retval->ext[r]->flags & LLLYEXT_OPT_VALID_SUBTREE) {
                retval->flags |= LLLYS_VALID_EXT_SUBTREE;
                break;
            }
        }
    }

    return retval;

error:

    lllys_node_free(retval, NULL, 0);
    while (root.child) {
        lllyxml_free(ctx, root.child);
    }
    while (uniq.child) {
        lllyxml_free(ctx, uniq.child);
    }

    return NULL;
}

/* logs directly */
static struct lllys_node *
read_yin_container(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin, int options,
                   struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllyxml_elem *sub, *next, root;
    struct lllys_node *node = NULL;
    struct lllys_node *retval;
    struct lllys_node_container *cont;
    const char *value;
    void *reallocated;
    int r;
    int c_tpdf = 0, c_must = 0, c_ftrs = 0, c_ext = 0;

    /* init */
    memset(&root, 0, sizeof root);

    cont = calloc(1, sizeof *cont);
    LLLY_CHECK_ERR_RETURN(!cont, LOGMEM(ctx), NULL);

    cont->nodetype = LLLYS_CONTAINER;
    cont->prev = (struct lllys_node *)cont;
    retval = (struct lllys_node *)cont;

    if (read_yin_common(module, parent, retval, LLLYEXT_PAR_NODE, yin,
            OPT_IDENT | OPT_MODULE | ((options & LLLYS_PARSE_OPT_CFG_IGNORE) ? OPT_CFG_IGNORE :
                (options & LLLYS_PARSE_OPT_CFG_NOINHERIT) ? OPT_CFG_PARSE : OPT_CFG_PARSE | OPT_CFG_INHERIT),
            unres)) {
        goto error;
    }

    LOGDBG(LLLY_LDGYIN, "parsing %s statement \"%s\"", yin->name, retval->name);

    /* insert the node into the schema tree */
    if (lllys_node_addchild(parent, lllys_main_module(module), retval, options)) {
        goto error;
    }

    /* process container's specific children */
    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, retval->ext_size, "extensions", "container", error);
            c_ext++;
        } else if (!strcmp(sub->name, "presence")) {
            if (cont->presence) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            GETVAL(ctx, value, sub, "value");
            cont->presence = lllydict_insert(ctx, value, strlen(value));

            if (lllyp_yin_parse_subnode_ext(module, retval, LLLYEXT_PAR_NODE, sub, LLLYEXT_SUBSTMT_PRESENCE, 0, unres)) {
                goto error;
            }
            lllyxml_free(ctx, sub);
        } else if (!strcmp(sub->name, "when")) {
            if (cont->when) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }

            cont->when = read_yin_when(module, sub, unres);
            if (!cont->when) {
                lllyxml_free(ctx, sub);
                goto error;
            }
            lllyxml_free(ctx, sub);

            /* data statements */
        } else if (!strcmp(sub->name, "container") ||
                !strcmp(sub->name, "leaf-list") ||
                !strcmp(sub->name, "leaf") ||
                !strcmp(sub->name, "list") ||
                !strcmp(sub->name, "choice") ||
                !strcmp(sub->name, "uses") ||
                !strcmp(sub->name, "grouping") ||
                !strcmp(sub->name, "anyxml") ||
                !strcmp(sub->name, "anydata") ||
                !strcmp(sub->name, "action") ||
                !strcmp(sub->name, "notification")) {
            lllyxml_unlink_elem(ctx, sub, 2);
            lllyxml_add_child(ctx, &root, sub);

            /* array counters */
        } else if (!strcmp(sub->name, "typedef")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_tpdf, cont->tpdf_size, "typedefs", "container", error);
            c_tpdf++;
        } else if (!strcmp(sub->name, "must")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_must, cont->must_size, "musts", "container", error);
            c_must++;
        } else if (!strcmp(sub->name, "if-feature")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, retval->iffeature_size, "if-features", "container", error);
            c_ftrs++;
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, retval, sub->name);
            goto error;
        }
    }

    /* middle part - process nodes with cardinality of 0..n except the data nodes */
    if (c_tpdf) {
        cont->tpdf = calloc(c_tpdf, sizeof *cont->tpdf);
        LLLY_CHECK_ERR_GOTO(!cont->tpdf, LOGMEM(ctx), error);
    }
    if (c_must) {
        cont->must = calloc(c_must, sizeof *cont->must);
        LLLY_CHECK_ERR_GOTO(!cont->must, LOGMEM(ctx), error);
    }
    if (c_ftrs) {
        cont->iffeature = calloc(c_ftrs, sizeof *cont->iffeature);
        LLLY_CHECK_ERR_GOTO(!cont->iffeature, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(retval->ext, (c_ext + retval->ext_size) * sizeof *retval->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        retval->ext = reallocated;

        /* init memory */
        memset(&retval->ext[retval->ext_size], 0, c_ext * sizeof *retval->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            r = lllyp_yin_fill_ext(retval, LLLYEXT_PAR_NODE, 0, 0, module, sub, &retval->ext, retval->ext_size, unres);
            retval->ext_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "typedef")) {
            r = fill_yin_typedef(module, retval, sub, &cont->tpdf[cont->tpdf_size], unres);
            cont->tpdf_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "must")) {
            r = fill_yin_must(module, sub, &cont->must[cont->must_size], unres);
            cont->must_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "if-feature")) {
            r = fill_yin_iffeature(retval, 0, sub, &cont->iffeature[cont->iffeature_size], unres);
            cont->iffeature_size++;
            if (r) {
                goto error;
            }
        }
    }

    /* last part - process data nodes */
    LLLY_TREE_FOR_SAFE(root.child, next, sub) {
        if (!strcmp(sub->name, "container")) {
            node = read_yin_container(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf-list")) {
            node = read_yin_leaflist(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf")) {
            node = read_yin_leaf(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "list")) {
            node = read_yin_list(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "choice")) {
            node = read_yin_choice(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "uses")) {
            node = read_yin_uses(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "grouping")) {
            node = read_yin_grouping(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "anyxml")) {
            node = read_yin_anydata(module, retval, sub, LLLYS_ANYXML, options, unres);
        } else if (!strcmp(sub->name, "anydata")) {
            node = read_yin_anydata(module, retval, sub, LLLYS_ANYDATA, options, unres);
        } else if (!strcmp(sub->name, "action")) {
            node = read_yin_rpc_action(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "notification")) {
            node = read_yin_notif(module, retval, sub, options, unres);
        }
        if (!node) {
            goto error;
        }

        lllyxml_free(ctx, sub);
    }

    /* check XPath dependencies */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) && (cont->when || cont->must)) {
        if (options & LLLYS_PARSE_OPT_INGRP) {
            if (lllyxp_node_check_syntax(retval)) {
                goto error;
            }
        } else {
            if (unres_schema_add_node(module, unres, retval, UNRES_XPATH, NULL) == -1) {
                goto error;
            }
        }
    }

    for (r = 0; r < retval->ext_size; ++r) {
        /* set flag, which represent LLLYEXT_OPT_VALID */
        if (retval->ext[r]->flags & LLLYEXT_OPT_VALID) {
            retval->flags |= LLLYS_VALID_EXT;
            if (retval->ext[r]->flags & LLLYEXT_OPT_VALID_SUBTREE) {
                retval->flags |= LLLYS_VALID_EXT_SUBTREE;
                break;
            }
        }
    }

    return retval;

error:
    lllys_node_free(retval, NULL, 0);
    while (root.child) {
        lllyxml_free(ctx, root.child);
    }
    return NULL;
}

/* logs directly */
static struct lllys_node *
read_yin_grouping(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin, int options,
                  struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllyxml_elem *sub, *next, root;
    struct lllys_node *node = NULL;
    struct lllys_node *retval;
    struct lllys_node_grp *grp;
    int r;
    int c_tpdf = 0, c_ext = 0;
    void *reallocated;

    /* init */
    memset(&root, 0, sizeof root);

    grp = calloc(1, sizeof *grp);
    LLLY_CHECK_ERR_RETURN(!grp, LOGMEM(ctx), NULL);

    grp->nodetype = LLLYS_GROUPING;
    grp->prev = (struct lllys_node *)grp;
    retval = (struct lllys_node *)grp;

    if (read_yin_common(module, parent, retval, LLLYEXT_PAR_NODE, yin, OPT_IDENT | OPT_MODULE , unres)) {
        goto error;
    }

    LOGDBG(LLLY_LDGYIN, "parsing %s statement \"%s\"", yin->name, retval->name);

    /* insert the node into the schema tree */
    if (lllys_node_addchild(parent, lllys_main_module(module), retval, options)) {
        goto error;
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, retval->ext_size, "extensions", "grouping", error);
            c_ext++;

        /* data statements */
        } else if (!strcmp(sub->name, "container") ||
                !strcmp(sub->name, "leaf-list") ||
                !strcmp(sub->name, "leaf") ||
                !strcmp(sub->name, "list") ||
                !strcmp(sub->name, "choice") ||
                !strcmp(sub->name, "uses") ||
                !strcmp(sub->name, "grouping") ||
                !strcmp(sub->name, "anyxml") ||
                !strcmp(sub->name, "anydata") ||
                !strcmp(sub->name, "action") ||
                !strcmp(sub->name, "notification")) {
            lllyxml_unlink_elem(ctx, sub, 2);
            lllyxml_add_child(ctx, &root, sub);

            /* array counters */
        } else if (!strcmp(sub->name, "typedef")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_tpdf, grp->tpdf_size, "typedefs", "grouping", error);
            c_tpdf++;
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, retval, sub->name);
            goto error;
        }
    }

    /* middle part - process nodes with cardinality of 0..n except the data nodes */
    if (c_tpdf) {
        grp->tpdf = calloc(c_tpdf, sizeof *grp->tpdf);
        LLLY_CHECK_ERR_GOTO(!grp->tpdf, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(retval->ext, (c_ext + retval->ext_size) * sizeof *retval->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        retval->ext = reallocated;

        /* init memory */
        memset(&retval->ext[retval->ext_size], 0, c_ext * sizeof *retval->ext);
    }
    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            r = lllyp_yin_fill_ext(retval, LLLYEXT_PAR_NODE, 0, 0, module, sub, &retval->ext, retval->ext_size, unres);
            retval->ext_size++;
            if (r) {
                goto error;
            }
        } else {
            /* typedef */
            r = fill_yin_typedef(module, retval, sub, &grp->tpdf[grp->tpdf_size], unres);
            grp->tpdf_size++;
            if (r) {
                goto error;
            }
        }
    }

    /* last part - process data nodes */
    if (!root.child) {
        LOGWRN(ctx, "Grouping \"%s\" without children.", retval->name);
    }
    options |= LLLYS_PARSE_OPT_INGRP;
    LLLY_TREE_FOR_SAFE(root.child, next, sub) {
        if (!strcmp(sub->name, "container")) {
            node = read_yin_container(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf-list")) {
            node = read_yin_leaflist(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf")) {
            node = read_yin_leaf(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "list")) {
            node = read_yin_list(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "choice")) {
            node = read_yin_choice(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "uses")) {
            node = read_yin_uses(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "grouping")) {
            node = read_yin_grouping(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "anyxml")) {
            node = read_yin_anydata(module, retval, sub, LLLYS_ANYXML, options, unres);
        } else if (!strcmp(sub->name, "anydata")) {
            node = read_yin_anydata(module, retval, sub, LLLYS_ANYDATA, options, unres);
        } else if (!strcmp(sub->name, "action")) {
            node = read_yin_rpc_action(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "notification")) {
            node = read_yin_notif(module, retval, sub, options, unres);
        }
        if (!node) {
            goto error;
        }

        lllyxml_free(ctx, sub);
    }

    return retval;

error:
    lllys_node_free(retval, NULL, 0);
    while (root.child) {
        lllyxml_free(ctx, root.child);
    }
    return NULL;
}

/* logs directly */
static struct lllys_node *
read_yin_input_output(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
                      int options, struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllyxml_elem *sub, *next, root;
    struct lllys_node *node = NULL;
    struct lllys_node *retval = NULL;
    struct lllys_node_inout *inout;
    int r;
    int c_tpdf = 0, c_must = 0, c_ext = 0;

    /* init */
    memset(&root, 0, sizeof root);

    inout = calloc(1, sizeof *inout);
    LLLY_CHECK_ERR_RETURN(!inout, LOGMEM(ctx), NULL);
    inout->prev = (struct lllys_node *)inout;

    if (!strcmp(yin->name, "input")) {
        inout->nodetype = LLLYS_INPUT;
        inout->name = lllydict_insert(ctx, "input", 0);
    } else if (!strcmp(yin->name, "output")) {
        inout->nodetype = LLLYS_OUTPUT;
        inout->name = lllydict_insert(ctx, "output", 0);
    } else {
        LOGINT(ctx);
        free(inout);
        goto error;
    }

    retval = (struct lllys_node *)inout;
    retval->module = module;

    LOGDBG(LLLY_LDGYIN, "parsing %s statement \"%s\"", yin->name, retval->name);

    /* insert the node into the schema tree */
    if (lllys_node_addchild(parent, lllys_main_module(module), retval, options)) {
        goto error;
    }

    /* data statements */
    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (!sub->ns) {
            /* garbage */
            lllyxml_free(ctx, sub);
        } else if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, retval->ext_size, "extensions",
                                          inout->nodetype == LLLYS_INPUT ? "input" : "output", error);
            c_ext++;
        } else if (!strcmp(sub->name, "container") ||
                !strcmp(sub->name, "leaf-list") ||
                !strcmp(sub->name, "leaf") ||
                !strcmp(sub->name, "list") ||
                !strcmp(sub->name, "choice") ||
                !strcmp(sub->name, "uses") ||
                !strcmp(sub->name, "grouping") ||
                !strcmp(sub->name, "anyxml") ||
                !strcmp(sub->name, "anydata")) {
            lllyxml_unlink_elem(ctx, sub, 2);
            lllyxml_add_child(ctx, &root, sub);

            /* array counters */
        } else if (!strcmp(sub->name, "typedef")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_tpdf, inout->tpdf_size, "typedefs",
                                          inout->nodetype == LLLYS_INPUT ? "input" : "output", error);
            c_tpdf++;

        } else if ((module->version >= 2) && !strcmp(sub->name, "must")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_must, inout->must_size, "musts",
                                          inout->nodetype == LLLYS_INPUT ? "input" : "output", error);
            c_must++;

        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, retval, sub->name);
            goto error;
        }
    }

    /* middle part - process nodes with cardinality of 0..n except the data nodes */
    if (c_tpdf) {
        inout->tpdf = calloc(c_tpdf, sizeof *inout->tpdf);
        LLLY_CHECK_ERR_GOTO(!inout->tpdf, LOGMEM(ctx), error);
    }
    if (c_must) {
        inout->must = calloc(c_must, sizeof *inout->must);
        LLLY_CHECK_ERR_GOTO(!inout->must, LOGMEM(ctx), error);
    }
    if (c_ext) {
        inout->ext = calloc(c_ext, sizeof *inout->ext);
        LLLY_CHECK_ERR_GOTO(!inout->ext, LOGMEM(ctx), error);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            r = lllyp_yin_fill_ext(retval, LLLYEXT_PAR_NODE, 0, 0, module, sub, &retval->ext, retval->ext_size, unres);
            retval->ext_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "must")) {
            r = fill_yin_must(module, sub, &inout->must[inout->must_size], unres);
            inout->must_size++;
            if (r) {
                goto error;
            }
        } else { /* typedef */
            r = fill_yin_typedef(module, retval, sub, &inout->tpdf[inout->tpdf_size], unres);
            inout->tpdf_size++;
            if (r) {
                goto error;
            }
        }
    }

    /* last part - process data nodes */
    options |= LLLYS_PARSE_OPT_CFG_IGNORE;
    LLLY_TREE_FOR_SAFE(root.child, next, sub) {
        if (!strcmp(sub->name, "container")) {
            node = read_yin_container(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf-list")) {
            node = read_yin_leaflist(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf")) {
            node = read_yin_leaf(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "list")) {
            node = read_yin_list(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "choice")) {
            node = read_yin_choice(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "uses")) {
            node = read_yin_uses(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "grouping")) {
            node = read_yin_grouping(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "anyxml")) {
            node = read_yin_anydata(module, retval, sub, LLLYS_ANYXML, options, unres);
        } else if (!strcmp(sub->name, "anydata")) {
            node = read_yin_anydata(module, retval, sub, LLLYS_ANYDATA, options, unres);
        }
        if (!node) {
            goto error;
        }

        lllyxml_free(ctx, sub);
    }

    /* check XPath dependencies */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) && inout->must) {
        if (options & LLLYS_PARSE_OPT_INGRP) {
            if (lllyxp_node_check_syntax(retval)) {
                goto error;
            }
        } else {
            if (unres_schema_add_node(module, unres, retval, UNRES_XPATH, NULL) == -1) {
                goto error;
            }
        }
    }

    return retval;

error:
    lllys_node_free(retval, NULL, 0);
    while (root.child) {
        lllyxml_free(ctx, root.child);
    }
    return NULL;
}

/* logs directly */
static struct lllys_node *
read_yin_notif(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
               int options, struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllyxml_elem *sub, *next, root;
    struct lllys_node *node = NULL;
    struct lllys_node *retval;
    struct lllys_node_notif *notif;
    int r;
    int c_tpdf = 0, c_ftrs = 0, c_must = 0, c_ext = 0;
    void *reallocated;

    if (parent && (module->version < 2)) {
        LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, parent, "notification");
        return NULL;
    }

    memset(&root, 0, sizeof root);

    notif = calloc(1, sizeof *notif);
    LLLY_CHECK_ERR_RETURN(!notif, LOGMEM(ctx), NULL);

    notif->nodetype = LLLYS_NOTIF;
    notif->prev = (struct lllys_node *)notif;
    retval = (struct lllys_node *)notif;

    if (read_yin_common(module, parent, retval, LLLYEXT_PAR_NODE, yin, OPT_IDENT | OPT_MODULE, unres)) {
        goto error;
    }

    LOGDBG(LLLY_LDGYIN, "parsing %s statement \"%s\"", yin->name, retval->name);

    /* insert the node into the schema tree */
    if (lllys_node_addchild(parent, lllys_main_module(module), retval, options)) {
        goto error;
    }

    /* process rpc's specific children */
    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, retval->ext_size, "extensions", "notification", error);
            c_ext++;
            continue;

        /* data statements */
        } else if (!strcmp(sub->name, "container") ||
                !strcmp(sub->name, "leaf-list") ||
                !strcmp(sub->name, "leaf") ||
                !strcmp(sub->name, "list") ||
                !strcmp(sub->name, "choice") ||
                !strcmp(sub->name, "uses") ||
                !strcmp(sub->name, "grouping") ||
                !strcmp(sub->name, "anyxml") ||
                !strcmp(sub->name, "anydata")) {
            lllyxml_unlink_elem(ctx, sub, 2);
            lllyxml_add_child(ctx, &root, sub);

            /* array counters */
        } else if (!strcmp(sub->name, "typedef")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_tpdf, notif->tpdf_size, "typedefs", "notification", error);
            c_tpdf++;
        } else if (!strcmp(sub->name, "if-feature")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, retval->iffeature_size, "if-features", "notification", error);
            c_ftrs++;
        } else if ((module->version >= 2) && !strcmp(sub->name, "must")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_must, notif->must_size, "musts", "notification", error);
            c_must++;
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, retval, sub->name);
            goto error;
        }
    }

    /* middle part - process nodes with cardinality of 0..n except the data nodes */
    if (c_tpdf) {
        notif->tpdf = calloc(c_tpdf, sizeof *notif->tpdf);
        LLLY_CHECK_ERR_GOTO(!notif->tpdf, LOGMEM(ctx), error);
    }
    if (c_ftrs) {
        notif->iffeature = calloc(c_ftrs, sizeof *notif->iffeature);
        LLLY_CHECK_ERR_GOTO(!notif->iffeature, LOGMEM(ctx), error);
    }
    if (c_must) {
        notif->must = calloc(c_must, sizeof *notif->must);
        LLLY_CHECK_ERR_GOTO(!notif->must, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(retval->ext, (c_ext + retval->ext_size) * sizeof *retval->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        retval->ext = reallocated;

        /* init memory */
        memset(&retval->ext[retval->ext_size], 0, c_ext * sizeof *retval->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            r = lllyp_yin_fill_ext(retval, LLLYEXT_PAR_NODE, 0, 0, module, sub, &retval->ext, retval->ext_size, unres);
            retval->ext_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "typedef")) {
            r = fill_yin_typedef(module, retval, sub, &notif->tpdf[notif->tpdf_size], unres);
            notif->tpdf_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "if-feature")) {
            r = fill_yin_iffeature(retval, 0, sub, &notif->iffeature[notif->iffeature_size], unres);
            notif->iffeature_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "must")) {
            r = fill_yin_must(module, sub, &notif->must[notif->must_size], unres);
            notif->must_size++;
            if (r) {
                goto error;
            }
        }
    }

    /* last part - process data nodes */
    options |= LLLYS_PARSE_OPT_CFG_IGNORE;
    LLLY_TREE_FOR_SAFE(root.child, next, sub) {
        if (!strcmp(sub->name, "container")) {
            node = read_yin_container(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf-list")) {
            node = read_yin_leaflist(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "leaf")) {
            node = read_yin_leaf(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "list")) {
            node = read_yin_list(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "choice")) {
            node = read_yin_choice(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "uses")) {
            node = read_yin_uses(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "grouping")) {
            node = read_yin_grouping(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "anyxml")) {
            node = read_yin_anydata(module, retval, sub, LLLYS_ANYXML, options, unres);
        } else if (!strcmp(sub->name, "anydata")) {
            node = read_yin_anydata(module, retval, sub, LLLYS_ANYDATA, options, unres);
        }
        if (!node) {
            goto error;
        }

        lllyxml_free(ctx, sub);
    }

    /* check XPath dependencies */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) && notif->must) {
        if (options & LLLYS_PARSE_OPT_INGRP) {
            if (lllyxp_node_check_syntax(retval)) {
                goto error;
            }
        } else {
            if (unres_schema_add_node(module, unres, retval, UNRES_XPATH, NULL) == -1) {
                goto error;
            }
        }
    }

    return retval;

error:
    lllys_node_free(retval, NULL, 0);
    while (root.child) {
        lllyxml_free(ctx, root.child);
    }
    return NULL;
}

/* logs directly */
static struct lllys_node *
read_yin_rpc_action(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
                    int options, struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllyxml_elem *sub, *next, root;
    struct lllys_node *node = NULL;
    struct lllys_node *retval;
    struct lllys_node_rpc_action *rpc;
    int r;
    int c_tpdf = 0, c_ftrs = 0, c_input = 0, c_output = 0, c_ext = 0;
    void *reallocated;

    if (!strcmp(yin->name, "action")) {
        if (module->version < 2) {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, parent, "action");
            return NULL;
        }
        for (node = parent; node; node = lllys_parent(node)) {
            if ((node->nodetype & (LLLYS_RPC | LLLYS_ACTION | LLLYS_NOTIF))
                    || ((node->nodetype == LLLYS_LIST) && !((struct lllys_node_list *)node)->keys_size)) {
                LOGVAL(ctx, LLLYE_INPAR, LLLY_VLOG_LYS, parent, strnodetype(node->nodetype), "action");
                return NULL;
            }
        }
    }

    /* init */
    memset(&root, 0, sizeof root);

    rpc = calloc(1, sizeof *rpc);
    LLLY_CHECK_ERR_RETURN(!rpc, LOGMEM(ctx), NULL);

    rpc->nodetype = (!strcmp(yin->name, "rpc") ? LLLYS_RPC : LLLYS_ACTION);
    rpc->prev = (struct lllys_node *)rpc;
    retval = (struct lllys_node *)rpc;

    if (read_yin_common(module, parent, retval, LLLYEXT_PAR_NODE, yin, OPT_IDENT | OPT_MODULE, unres)) {
        goto error;
    }

    LOGDBG(LLLY_LDGYIN, "parsing %s statement \"%s\"", yin->name, retval->name);

    /* insert the node into the schema tree */
    if (lllys_node_addchild(parent, lllys_main_module(module), retval, options)) {
        goto error;
    }

    /* process rpc's specific children */
    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, retval->ext_size, "extensions",
                                          rpc->nodetype == LLLYS_RPC ? "rpc" : "action", error);
            c_ext++;
            continue;
        } else if (!strcmp(sub->name, "input")) {
            if (c_input) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            c_input++;
            lllyxml_unlink_elem(ctx, sub, 2);
            lllyxml_add_child(ctx, &root, sub);
        } else if (!strcmp(sub->name, "output")) {
            if (c_output) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }
            c_output++;
            lllyxml_unlink_elem(ctx, sub, 2);
            lllyxml_add_child(ctx, &root, sub);

            /* data statements */
        } else if (!strcmp(sub->name, "grouping")) {
            lllyxml_unlink_elem(ctx, sub, 2);
            lllyxml_add_child(ctx, &root, sub);

            /* array counters */
        } else if (!strcmp(sub->name, "typedef")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_tpdf, rpc->tpdf_size, "typedefs",
                                          rpc->nodetype == LLLYS_RPC ? "rpc" : "action", error);
            c_tpdf++;
        } else if (!strcmp(sub->name, "if-feature")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, retval->iffeature_size, "if-features",
                                          rpc->nodetype == LLLYS_RPC ? "rpc" : "action", error);
            c_ftrs++;
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, retval, sub->name);
            goto error;
        }
    }

    /* middle part - process nodes with cardinality of 0..n except the data nodes */
    if (c_tpdf) {
        rpc->tpdf = calloc(c_tpdf, sizeof *rpc->tpdf);
        LLLY_CHECK_ERR_GOTO(!rpc->tpdf, LOGMEM(ctx), error);
    }
    if (c_ftrs) {
        rpc->iffeature = calloc(c_ftrs, sizeof *rpc->iffeature);
        LLLY_CHECK_ERR_GOTO(!rpc->iffeature, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(retval->ext, (c_ext + retval->ext_size) * sizeof *retval->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        retval->ext = reallocated;

        /* init memory */
        memset(&retval->ext[retval->ext_size], 0, c_ext * sizeof *retval->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            r = lllyp_yin_fill_ext(retval, LLLYEXT_PAR_NODE, 0, 0, module, sub, &retval->ext, retval->ext_size, unres);
            retval->ext_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "typedef")) {
            r = fill_yin_typedef(module, retval, sub, &rpc->tpdf[rpc->tpdf_size], unres);
            rpc->tpdf_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "if-feature")) {
            r = fill_yin_iffeature(retval, 0, sub, &rpc->iffeature[rpc->iffeature_size], unres);
            rpc->iffeature_size++;
            if (r) {
                goto error;
            }
        }
    }

    /* last part - process data nodes */
    LLLY_TREE_FOR_SAFE(root.child, next, sub) {
        if (!strcmp(sub->name, "grouping")) {
            node = read_yin_grouping(module, retval, sub, options, unres);
        } else if (!strcmp(sub->name, "input") || !strcmp(sub->name, "output")) {
            node = read_yin_input_output(module, retval, sub, options, unres);
        }
        if (!node) {
            goto error;
        }

        lllyxml_free(ctx, sub);
    }

    return retval;

error:
    lllys_node_free(retval, NULL, 0);
    while (root.child) {
        lllyxml_free(ctx, root.child);
    }
    return NULL;
}

/* logs directly
 *
 * resolve - referenced grouping should be bounded to the namespace (resolved)
 * only when uses does not appear in grouping. In a case of grouping's uses,
 * we just get information but we do not apply augment or refine to it.
 */
static struct lllys_node *
read_yin_uses(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin,
              int options, struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllyxml_elem *sub, *next;
    struct lllys_node *retval;
    struct lllys_node_uses *uses;
    const char *value;
    int c_ref = 0, c_aug = 0, c_ftrs = 0, c_ext = 0;
    int r;
    void *reallocated;

    uses = calloc(1, sizeof *uses);
    LLLY_CHECK_ERR_RETURN(!uses, LOGMEM(ctx), NULL);

    uses->nodetype = LLLYS_USES;
    uses->prev = (struct lllys_node *)uses;
    retval = (struct lllys_node *)uses;

    GETVAL(ctx, value, yin, "name");
    uses->name = lllydict_insert(ctx, value, 0);

    if (read_yin_common(module, parent, retval, LLLYEXT_PAR_NODE, yin, OPT_MODULE, unres)) {
        goto error;
    }

    LOGDBG(LLLY_LDGYIN, "parsing %s statement \"%s\"", yin->name, retval->name);

    /* insert the node into the schema tree */
    if (lllys_node_addchild(parent, lllys_main_module(module), retval, options)) {
        goto error;
    }

    /* get other properties of uses */
    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, retval->ext_size, "extensions", "uses", error);
            c_ext++;
            continue;
        } else if (!strcmp(sub->name, "refine")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ref, uses->refine_size, "refines", "uses", error);
            c_ref++;
        } else if (!strcmp(sub->name, "augment")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_aug, uses->augment_size, "augments", "uses", error);
            c_aug++;
        } else if (!strcmp(sub->name, "if-feature")) {
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, retval->iffeature_size, "if-features", "uses", error);
            c_ftrs++;
        } else if (!strcmp(sub->name, "when")) {
            if (uses->when) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYS, retval, sub->name, yin->name);
                goto error;
            }

            uses->when = read_yin_when(module, sub, unres);
            if (!uses->when) {
                lllyxml_free(ctx, sub);
                goto error;
            }
            lllyxml_free(ctx, sub);
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_LYS, retval, sub->name);
            goto error;
        }
    }

    /* process properties with cardinality 0..n */
    if (c_ref) {
        uses->refine = calloc(c_ref, sizeof *uses->refine);
        LLLY_CHECK_ERR_GOTO(!uses->refine, LOGMEM(ctx), error);
    }
    if (c_aug) {
        uses->augment = calloc(c_aug, sizeof *uses->augment);
        LLLY_CHECK_ERR_GOTO(!uses->augment, LOGMEM(ctx), error);
    }
    if (c_ftrs) {
        uses->iffeature = calloc(c_ftrs, sizeof *uses->iffeature);
        LLLY_CHECK_ERR_GOTO(!uses->iffeature, LOGMEM(ctx), error);
    }
    if (c_ext) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(retval->ext, (c_ext + retval->ext_size) * sizeof *retval->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        retval->ext = reallocated;

        /* init memory */
        memset(&retval->ext[retval->ext_size], 0, c_ext * sizeof *retval->ext);
    }

    LLLY_TREE_FOR_SAFE(yin->child, next, sub) {
        if (strcmp(sub->ns->value, LLLY_NSYIN)) {
            /* extension */
            r = lllyp_yin_fill_ext(retval, LLLYEXT_PAR_NODE, 0, 0, module, sub, &retval->ext, retval->ext_size, unres);
            retval->ext_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "refine")) {
            r = fill_yin_refine(retval, sub, &uses->refine[uses->refine_size], unres);
            uses->refine_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "augment")) {
            r = fill_yin_augment(module, retval, sub, &uses->augment[uses->augment_size], options, unres);
            uses->augment_size++;
            if (r) {
                goto error;
            }
        } else if (!strcmp(sub->name, "if-feature")) {
            r = fill_yin_iffeature(retval, 0, sub, &uses->iffeature[uses->iffeature_size], unres);
            uses->iffeature_size++;
            if (r) {
                goto error;
            }
        }
    }

    if (unres_schema_add_node(module, unres, uses, UNRES_USES, NULL) == -1) {
        goto error;
    }

    /* check XPath dependencies */
    if (!(ctx->models.flags & LLLY_CTX_TRUSTED) && uses->when) {
        if (options & LLLYS_PARSE_OPT_INGRP) {
            if (lllyxp_node_check_syntax(retval)) {
                goto error;
            }
        } else {
            if (unres_schema_add_node(module, unres, retval, UNRES_XPATH, NULL) == -1) {
                goto error;
            }
        }
    }

    return retval;

error:
    lllys_node_free(retval, NULL, 0);
    return NULL;
}

/* logs directly
 *
 * common code for yin_read_module() and yin_read_submodule()
 */
static int
read_sub_module(struct lllys_module *module, struct lllys_submodule *submodule, struct lllyxml_elem *yin,
                struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllyxml_elem *next, *child, root, grps, augs, revs, exts;
    struct lllys_node *node = NULL;
    struct lllys_module *trg;
    const char *value;
    int i, r, ret = -1;
    int version_flag = 0;
    /* (sub)module substatements are ordered in groups, increment this value when moving to another group
     * 0 - header-stmts, 1 - linkage-stmts, 2 - meta-stmts, 3 - revision-stmts, 4 - body-stmts */
    int substmt_group;
    /* just remember last substatement for logging */
    const char *substmt_prev;
    /* counters */
    int c_imp = 0, c_rev = 0, c_tpdf = 0, c_ident = 0, c_inc = 0, c_aug = 0, c_ftrs = 0, c_dev = 0;
    int c_ext = 0, c_extinst = 0;
    void *reallocated;

    /* to simplify code, store the module/submodule being processed as trg */
    trg = submodule ? (struct lllys_module *)submodule : module;

    /* init */
    memset(&root, 0, sizeof root);
    memset(&grps, 0, sizeof grps);
    memset(&augs, 0, sizeof augs);
    memset(&exts, 0, sizeof exts);
    memset(&revs, 0, sizeof revs);

    /*
     * in the first run, we process elements with cardinality of 1 or 0..1 and
     * count elements with cardinality 0..n. Data elements (choices, containers,
     * leafs, lists, leaf-lists) are moved aside to be processed last, since we
     * need have all top-level and groupings already prepared at that time. In
     * the middle loop, we process other elements with carinality of 0..n since
     * we need to allocate arrays to store them.
     */
    substmt_group = 0;
    substmt_prev = NULL;
    LLLY_TREE_FOR_SAFE(yin->child, next, child) {
        if (!child->ns) {
            /* garbage */
            lllyxml_free(ctx, child);
            continue;
        } else if (strcmp(child->ns->value, LLLY_NSYIN)) {
            /* possible extension instance */
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_extinst, trg->ext_size, "extension instances",
                                          submodule ? "submodule" : "module", error);
            lllyxml_unlink_elem(ctx, child, 2);
            lllyxml_add_child(ctx, &exts, child);
            c_extinst++;
        } else if (!submodule && !strcmp(child->name, "namespace")) {
            if (substmt_group > 0) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Statement \"%s\" cannot appear after \"%s\" statement.",
                       child->name, substmt_prev);
                goto error;
            }

            if (trg->ns) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            GETVAL(ctx, value, child, "uri");
            trg->ns = lllydict_insert(ctx, value, strlen(value));

            if (lllyp_yin_parse_subnode_ext(trg, trg, LLLYEXT_PAR_MODULE, child, LLLYEXT_SUBSTMT_NAMESPACE, 0, unres)) {
                goto error;
            }
            lllyxml_free(ctx, child);

            substmt_prev = "namespace";
        } else if (!submodule && !strcmp(child->name, "prefix")) {
            if (substmt_group > 0) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Statement \"%s\" cannot appear after \"%s\" statement.",
                       child->name, substmt_prev);
                goto error;
            }

            if (trg->prefix) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            GETVAL(ctx, value, child, "value");
            if (lllyp_check_identifier(ctx, value, LLLY_IDENT_PREFIX, trg, NULL)) {
                goto error;
            }
            trg->prefix = lllydict_insert(ctx, value, strlen(value));

            if (lllyp_yin_parse_subnode_ext(trg, trg, LLLYEXT_PAR_MODULE, child, LLLYEXT_SUBSTMT_PREFIX, 0, unres)) {
                goto error;
            }
            lllyxml_free(ctx, child);

            substmt_prev = "prefix";
        } else if (submodule && !strcmp(child->name, "belongs-to")) {
            if (substmt_group > 0) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Statement \"%s\" cannot appear after \"%s\" statement.",
                       child->name, substmt_prev);
                goto error;
            }

            if (trg->prefix) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            GETVAL(ctx, value, child, "module");
            if (!llly_strequal(value, submodule->belongsto->name, 1)) {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, child->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(trg, trg, LLLYEXT_PAR_MODULE, child, LLLYEXT_SUBSTMT_BELONGSTO, 0, unres)) {
                goto error;
            }

            /* get the prefix substatement, start with checks */
            if (!child->child) {
                LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "prefix", child->name);
                goto error;
            } else if (strcmp(child->child->name, "prefix")) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->child->name);
                goto error;
            } else if (child->child->next) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->child->next->name);
                goto error;
            }
            /* and now finally get the value */
            GETVAL(ctx, value, child->child, "value");
            /* check here differs from a generic prefix check, since this prefix
             * don't have to be unique
             */
            if (lllyp_check_identifier(ctx, value, LLLY_IDENT_NAME, NULL, NULL)) {
                goto error;
            }
            submodule->prefix = lllydict_insert(ctx, value, strlen(value));

            if (lllyp_yin_parse_subnode_ext(trg, trg, LLLYEXT_PAR_MODULE, child->child, LLLYEXT_SUBSTMT_PREFIX, 0, unres)) {
                goto error;
            }

            /* we are done with belongs-to */
            lllyxml_free(ctx, child);

            substmt_prev = "belongs-to";

            /* counters (statements with n..1 cardinality) */
        } else if (!strcmp(child->name, "import")) {
            if (substmt_group > 1) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Statement \"%s\" cannot appear after \"%s\" statement.",
                       child->name, substmt_prev);
                goto error;
            }
            substmt_group = 1;
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_imp, trg->imp_size, "imports",
                                          submodule ? "submodule" : "module", error);
            c_imp++;

            substmt_prev = "import";
        } else if (!strcmp(child->name, "revision")) {
            if (substmt_group > 3) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Statement \"%s\" cannot appear after \"%s\" statement.",
                       child->name, substmt_prev);
                goto error;
            }
            substmt_group = 3;
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_rev, trg->rev_size, "revisions",
                                          submodule ? "submodule" : "module", error);
            c_rev++;

            lllyxml_unlink_elem(ctx, child, 2);
            lllyxml_add_child(ctx, &revs, child);

            substmt_prev = "revision";
        } else if (!strcmp(child->name, "typedef")) {
            substmt_group = 4;
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_tpdf, trg->tpdf_size, "typedefs",
                                          submodule ? "submodule" : "module", error);
            c_tpdf++;

            substmt_prev = "typedef";
        } else if (!strcmp(child->name, "identity")) {
            substmt_group = 4;
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ident, trg->ident_size, "identities",
                                          submodule ? "submodule" : "module", error);
            c_ident++;

            substmt_prev = "identity";
        } else if (!strcmp(child->name, "include")) {
            if (substmt_group > 1) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Statement \"%s\" cannot appear after \"%s\" statement.",
                       child->name, substmt_prev);
                goto error;
            }
            substmt_group = 1;
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_inc, trg->inc_size, "includes",
                                          submodule ? "submodule" : "module", error);
            c_inc++;

            substmt_prev = "include";
        } else if (!strcmp(child->name, "augment")) {
            substmt_group = 4;
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_aug, trg->augment_size, "augments",
                                          submodule ? "submodule" : "module", error);
            c_aug++;
            /* keep augments separated, processed last */
            lllyxml_unlink_elem(ctx, child, 2);
            lllyxml_add_child(ctx, &augs, child);

            substmt_prev = "augment";
        } else if (!strcmp(child->name, "feature")) {
            substmt_group = 4;
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ftrs, trg->features_size, "features",
                                          submodule ? "submodule" : "module", error);
            c_ftrs++;

            substmt_prev = "feature";

            /* data statements */
        } else if (!strcmp(child->name, "container") ||
                !strcmp(child->name, "leaf-list") ||
                !strcmp(child->name, "leaf") ||
                !strcmp(child->name, "list") ||
                !strcmp(child->name, "choice") ||
                !strcmp(child->name, "uses") ||
                !strcmp(child->name, "anyxml") ||
                !strcmp(child->name, "anydata") ||
                !strcmp(child->name, "rpc") ||
                !strcmp(child->name, "notification")) {
            substmt_group = 4;

            lllyxml_unlink_elem(ctx, child, 2);
            lllyxml_add_child(ctx, &root, child);

            substmt_prev = "data definition";
        } else if (!strcmp(child->name, "grouping")) {
            substmt_group = 4;

            /* keep groupings separated and process them before other data statements */
            lllyxml_unlink_elem(ctx, child, 2);
            lllyxml_add_child(ctx, &grps, child);

            substmt_prev = "grouping";
            /* optional statements */
        } else if (!strcmp(child->name, "description")) {
            if (substmt_group > 2) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Statement \"%s\" cannot appear after \"%s\" statement.",
                       child->name, substmt_prev);
                goto error;
            }
            substmt_group = 2;

            if (trg->dsc) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(trg, trg, LLLYEXT_PAR_MODULE, child, LLLYEXT_SUBSTMT_DESCRIPTION, 0, unres)) {
                goto error;
            }
            trg->dsc = read_yin_subnode(ctx, child, "text");
            lllyxml_free(ctx, child);
            if (!trg->dsc) {
                goto error;
            }

            substmt_prev = "description";
        } else if (!strcmp(child->name, "reference")) {
            if (substmt_group > 2) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Statement \"%s\" cannot appear after \"%s\" statement.",
                       child->name, substmt_prev);
                goto error;
            }
            substmt_group = 2;

            if (trg->ref) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(trg, trg, LLLYEXT_PAR_MODULE, child, LLLYEXT_SUBSTMT_REFERENCE, 0, unres)) {
                goto error;
            }
            trg->ref = read_yin_subnode(ctx, child, "text");
            lllyxml_free(ctx, child);
            if (!trg->ref) {
                goto error;
            }

            substmt_prev = "reference";
        } else if (!strcmp(child->name, "organization")) {
            if (substmt_group > 2) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Statement \"%s\" cannot appear after \"%s\" statement.",
                       child->name, substmt_prev);
                goto error;
            }
            substmt_group = 2;

            if (trg->org) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(trg, trg, LLLYEXT_PAR_MODULE, child, LLLYEXT_SUBSTMT_ORGANIZATION, 0, unres)) {
                goto error;
            }
            trg->org = read_yin_subnode(ctx, child, "text");
            lllyxml_free(ctx, child);
            if (!trg->org) {
                goto error;
            }

            substmt_prev = "organization";
        } else if (!strcmp(child->name, "contact")) {
            if (substmt_group > 2) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Statement \"%s\" cannot appear after \"%s\" statement.",
                       child->name, substmt_prev);
                goto error;
            }
            substmt_group = 2;

            if (trg->contact) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            if (lllyp_yin_parse_subnode_ext(trg, trg, LLLYEXT_PAR_MODULE, child, LLLYEXT_SUBSTMT_CONTACT, 0, unres)) {
                goto error;
            }
            trg->contact = read_yin_subnode(ctx, child, "text");
            lllyxml_free(ctx, child);
            if (!trg->contact) {
                goto error;
            }

            substmt_prev = "contact";
        } else if (!strcmp(child->name, "yang-version")) {
            if (substmt_group > 0) {
                LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Statement \"%s\" cannot appear after \"%s\" statement.",
                       child->name, substmt_prev);
                goto error;
            }

            if (version_flag) {
                LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, child->name, yin->name);
                goto error;
            }
            GETVAL(ctx, value, child, "value");
            if (strcmp(value, "1") && strcmp(value, "1.1")) {
                LOGVAL(ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, "yang-version");
                goto error;
            }
            version_flag = 1;
            if (!strcmp(value, "1")) {
                if (submodule) {
                    if (module->version > 1) {
                        LOGVAL(ctx, LLLYE_INVER, LLLY_VLOG_NONE, NULL);
                        goto error;
                    }
                    submodule->version = 1;
                } else {
                    module->version = 1;
                }
            } else {
                if (submodule) {
                    if (module->version < 2) {
                        LOGVAL(ctx, LLLYE_INVER, LLLY_VLOG_NONE, NULL);
                        goto error;
                    }
                    submodule->version = 2;
                } else {
                    module->version = 2;
                }
            }

            if (lllyp_yin_parse_subnode_ext(trg, trg, LLLYEXT_PAR_MODULE, child, LLLYEXT_SUBSTMT_VERSION, 0, unres)) {
                goto error;
            }
            lllyxml_free(ctx, child);

            substmt_prev = "yang-version";
        } else if (!strcmp(child->name, "extension")) {
            substmt_group = 4;
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_ext, trg->extensions_size, "extensions",
                                          submodule ? "submodule" : "module", error);
            c_ext++;

            substmt_prev = "extension";
        } else if (!strcmp(child->name, "deviation")) {
            substmt_group = 4;
            YIN_CHECK_ARRAY_OVERFLOW_GOTO(ctx, c_dev, trg->deviation_size, "deviations",
                                          submodule ? "submodule" : "module", error);
            c_dev++;

            substmt_prev = "deviation";
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, child->name);
            goto error;
        }
    }

    /* check for mandatory statements */
    if (submodule) {
        if (!submodule->prefix) {
            LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "belongs-to", "submodule");
            goto error;
        }
        if (!version_flag) {
            /* check version compatibility with the main module */
            if (module->version > 1) {
                LOGVAL(ctx, LLLYE_INVER, LLLY_VLOG_NONE, NULL);
                goto error;
            }
        }
    } else {
        if (!trg->ns) {
            LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "namespace", "module");
            goto error;
        }
        if (!trg->prefix) {
            LOGVAL(ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "prefix", "module");
            goto error;
        }
    }

    /* allocate arrays for elements with cardinality of 0..n */
    if (c_imp) {
        trg->imp = calloc(c_imp, sizeof *trg->imp);
        LLLY_CHECK_ERR_GOTO(!trg->imp, LOGMEM(ctx), error);
    }
    if (c_rev) {
        trg->rev = calloc(c_rev, sizeof *trg->rev);
        LLLY_CHECK_ERR_GOTO(!trg->rev, LOGMEM(ctx), error);
    }
    if (c_tpdf) {
        trg->tpdf = calloc(c_tpdf, sizeof *trg->tpdf);
        LLLY_CHECK_ERR_GOTO(!trg->tpdf, LOGMEM(ctx), error);
    }
    if (c_ident) {
        trg->ident = calloc(c_ident, sizeof *trg->ident);
        LLLY_CHECK_ERR_GOTO(!trg->ident, LOGMEM(ctx), error);
    }
    if (c_inc) {
        trg->inc = calloc(c_inc, sizeof *trg->inc);
        LLLY_CHECK_ERR_GOTO(!trg->inc, LOGMEM(ctx), error);
    }
    if (c_aug) {
        trg->augment = calloc(c_aug, sizeof *trg->augment);
        LLLY_CHECK_ERR_GOTO(!trg->augment, LOGMEM(ctx), error);
    }
    if (c_ftrs) {
        trg->features = calloc(c_ftrs, sizeof *trg->features);
        LLLY_CHECK_ERR_GOTO(!trg->features, LOGMEM(ctx), error);
    }
    if (c_dev) {
        trg->deviation = calloc(c_dev, sizeof *trg->deviation);
        LLLY_CHECK_ERR_GOTO(!trg->deviation, LOGMEM(ctx), error);
    }
    if (c_ext) {
        trg->extensions = calloc(c_ext, sizeof *trg->extensions);
        LLLY_CHECK_ERR_GOTO(!trg->extensions, LOGMEM(ctx), error);
    }

    /* middle part 1 - process revision and then check whether this (sub)module was not already parsed, add it there */
    LLLY_TREE_FOR_SAFE(revs.child, next, child) {
        r = fill_yin_revision(trg, child, &trg->rev[trg->rev_size], unres);
        trg->rev_size++;
        if (r) {
            goto error;
        }

        /* check uniqueness of the revision date - not required by RFC */
        for (i = 0; i < (trg->rev_size - 1); i++) {
            if (!strcmp(trg->rev[i].date, trg->rev[trg->rev_size - 1].date)) {
                LOGWRN(ctx, "Module's revisions are not unique (%s).", trg->rev[trg->rev_size - 1].date);
                break;
            }
        }

        lllyxml_free(ctx, child);
    }

    /* check the module with respect to the context now */
    if (!submodule) {
        switch (lllyp_ctx_check_module(module)) {
        case -1:
            goto error;
        case 0:
            break;
        case 1:
            /* it's already there */
            ret = 1;
            goto error;
        }
    }

    /* check first definition of extensions */
    if (c_ext) {
        LLLY_TREE_FOR_SAFE(yin->child, next, child) {
            if (!strcmp(child->name, "extension")) {
                r = fill_yin_extension(trg, child, &trg->extensions[trg->extensions_size], unres);
                trg->extensions_size++;
                if (r) {
                    goto error;
                }

            }
        }
    }

    /* middle part 2 - process nodes with cardinality of 0..n except the data nodes and augments */
    LLLY_TREE_FOR_SAFE(yin->child, next, child) {
        if (!strcmp(child->name, "import")) {
            r = fill_yin_import(trg, child, &trg->imp[trg->imp_size], unres);
            trg->imp_size++;
            if (r) {
                goto error;
            }

        } else if (!strcmp(child->name, "include")) {
            r = fill_yin_include(module, submodule, child, &trg->inc[trg->inc_size], unres);
            trg->inc_size++;
            if (r) {
                goto error;
            }

        } else if (!strcmp(child->name, "typedef")) {
            r = fill_yin_typedef(trg, NULL, child, &trg->tpdf[trg->tpdf_size], unres);
            trg->tpdf_size++;
            if (r) {
                goto error;
            }

        } else if (!strcmp(child->name, "identity")) {
            r = fill_yin_identity(trg, child, &trg->ident[trg->ident_size], unres);
            trg->ident_size++;
            if (r) {
                goto error;
            }

        } else if (!strcmp(child->name, "feature")) {
            r = fill_yin_feature(trg, child, &trg->features[trg->features_size], unres);
            trg->features_size++;
            if (r) {
                goto error;
            }

        } else if (!strcmp(child->name, "deviation")) {
            /* must be implemented in this case */
            trg->implemented = 1;

            r = fill_yin_deviation(trg, child, &trg->deviation[trg->deviation_size], unres);
            trg->deviation_size++;
            if (r) {
                goto error;
            }
        }
    }

    /* process extension instances */
    if (c_extinst) {
        /* some extensions may be already present from the substatements */
        reallocated = realloc(trg->ext, (c_extinst + trg->ext_size) * sizeof *trg->ext);
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(ctx), error);
        trg->ext = reallocated;

        /* init memory */
        memset(&trg->ext[trg->ext_size], 0, c_extinst * sizeof *trg->ext);

        LLLY_TREE_FOR_SAFE(exts.child, next, child) {
            r = lllyp_yin_fill_ext(trg, LLLYEXT_PAR_MODULE, 0, 0, trg, child, &trg->ext, trg->ext_size, unres);
            trg->ext_size++;
            if (r) {
                goto error;
            }
        }
    }

    /* process data nodes. Start with groupings to allow uses
     * refer to them. Submodule's data nodes are stored in the
     * main module data tree.
     */
    LLLY_TREE_FOR_SAFE(grps.child, next, child) {
        node = read_yin_grouping(trg, NULL, child, 0, unres);
        if (!node) {
            goto error;
        }

        lllyxml_free(ctx, child);
    }

    /* parse data nodes, ... */
    LLLY_TREE_FOR_SAFE(root.child, next, child) {

        if (!strcmp(child->name, "container")) {
            node = read_yin_container(trg, NULL, child, 0, unres);
        } else if (!strcmp(child->name, "leaf-list")) {
            node = read_yin_leaflist(trg, NULL, child, 0, unres);
        } else if (!strcmp(child->name, "leaf")) {
            node = read_yin_leaf(trg, NULL, child, 0, unres);
        } else if (!strcmp(child->name, "list")) {
            node = read_yin_list(trg, NULL, child, 0, unres);
        } else if (!strcmp(child->name, "choice")) {
            node = read_yin_choice(trg, NULL, child, 0, unres);
        } else if (!strcmp(child->name, "uses")) {
            node = read_yin_uses(trg, NULL, child, 0, unres);
        } else if (!strcmp(child->name, "anyxml")) {
            node = read_yin_anydata(trg, NULL, child, LLLYS_ANYXML, 0, unres);
        } else if (!strcmp(child->name, "anydata")) {
            node = read_yin_anydata(trg, NULL, child, LLLYS_ANYDATA, 0, unres);
        } else if (!strcmp(child->name, "rpc")) {
            node = read_yin_rpc_action(trg, NULL, child, 0, unres);
        } else if (!strcmp(child->name, "notification")) {
            node = read_yin_notif(trg, NULL, child, 0, unres);
        }
        if (!node) {
            goto error;
        }

        lllyxml_free(ctx, child);
    }

    /* ... and finally augments (last, so we can augment our data, for instance) */
    LLLY_TREE_FOR_SAFE(augs.child, next, child) {
        r = fill_yin_augment(trg, NULL, child, &trg->augment[trg->augment_size], 0, unres);
        trg->augment_size++;

        if (r) {
            goto error;
        }
        lllyxml_free(ctx, child);
    }

    return 0;

error:
    while (root.child) {
        lllyxml_free(ctx, root.child);
    }
    while (grps.child) {
        lllyxml_free(ctx, grps.child);
    }
    while (augs.child) {
        lllyxml_free(ctx, augs.child);
    }
    while (revs.child) {
        lllyxml_free(ctx, revs.child);
    }
    while (exts.child) {
        lllyxml_free(ctx, exts.child);
    }

    return ret;
}

/* logs directly */
struct lllys_submodule *
yin_read_submodule(struct lllys_module *module, const char *data, struct unres_schema *unres)
{
    struct llly_ctx *ctx = module->ctx;
    struct lllyxml_elem *yin;
    struct lllys_submodule *submodule = NULL;
    const char *value;

    yin = lllyxml_parse_mem(ctx, data, LLLYXML_PARSE_NOMIXEDCONTENT);
    if (!yin) {
        return NULL;
    }

    /* check root element */
    if (!yin->name || strcmp(yin->name, "submodule")) {
        LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, yin->name);
        goto error;
    }

    GETVAL(ctx, value, yin, "name");
    if (lllyp_check_identifier(ctx, value, LLLY_IDENT_NAME, NULL, NULL)) {
        goto error;
    }

    submodule = calloc(1, sizeof *submodule);
    LLLY_CHECK_ERR_GOTO(!submodule, LOGMEM(ctx), error);

    submodule->ctx = ctx;
    submodule->name = lllydict_insert(ctx, value, strlen(value));
    submodule->type = 1;
    submodule->implemented = module->implemented;
    submodule->belongsto = module;

    /* add into the list of processed modules */
    if (lllyp_check_circmod_add((struct lllys_module *)submodule)) {
        goto error;
    }

    LOGVRB("Reading submodule \"%s\".", submodule->name);
    /* module cannot be changed in this case and 1 cannot be returned */
    if (read_sub_module(module, submodule, yin, unres)) {
        goto error;
    }

    lllyp_sort_revisions((struct lllys_module *)submodule);

    /* cleanup */
    lllyxml_free(ctx, yin);
    lllyp_check_circmod_pop(ctx);

    LOGVRB("Submodule \"%s\" successfully parsed.", submodule->name);
    return submodule;

error:
    /* cleanup */
    lllyxml_free(ctx, yin);
    if (!submodule) {
        LOGERR(ctx, llly_errno, "Submodule parsing failed.");
        return NULL;
    }

    LOGERR(ctx, llly_errno, "Submodule \"%s\" parsing failed.", submodule->name);

    unres_schema_free((struct lllys_module *)submodule, &unres, 0);
    lllyp_check_circmod_pop(ctx);
    lllys_sub_module_remove_devs_augs((struct lllys_module *)submodule);
    lllys_submodule_module_data_free(submodule);
    lllys_submodule_free(submodule, NULL);
    return NULL;
}

/* logs directly */
struct lllys_module *
yin_read_module_(struct llly_ctx *ctx, struct lllyxml_elem *yin, const char *revision, int implement)
{
    struct lllys_module *module = NULL;
    struct unres_schema *unres;
    const char *value;
    int ret;

    unres = calloc(1, sizeof *unres);
    LLLY_CHECK_ERR_RETURN(!unres, LOGMEM(ctx), NULL);

    /* check root element */
    if (!yin->name || strcmp(yin->name, "module")) {
        if (llly_strequal("submodule", yin->name, 0)) {
            LOGVAL(ctx, LLLYE_SUBMODULE, LLLY_VLOG_NONE, NULL);
        } else {
            LOGVAL(ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, yin->name);
        }
        goto error;
    }

    GETVAL(ctx, value, yin, "name");
    if (lllyp_check_identifier(ctx, value, LLLY_IDENT_NAME, NULL, NULL)) {
        goto error;
    }

    module = calloc(1, sizeof *module);
    LLLY_CHECK_ERR_GOTO(!module, LOGMEM(ctx), error);

    module->ctx = ctx;
    module->name = lllydict_insert(ctx, value, strlen(value));
    module->type = 0;
    module->implemented = (implement ? 1 : 0);

    /* add into the list of processed modules */
    if (lllyp_check_circmod_add(module)) {
        goto error;
    }

    LOGVRB("Reading module \"%s\".", module->name);
    ret = read_sub_module(module, NULL, yin, unres);
    if (ret == -1) {
        goto error;
    }

    if (ret == 1) {
        assert(!unres->count);
    } else {
        /* make this module implemented if was not from start */
        if (!implement && module->implemented && (unres_schema_add_node(module, unres, NULL, UNRES_MOD_IMPLEMENT, NULL) == -1)) {
            goto error;
        }

        /* resolve rest of unres items */
        if (unres->count && resolve_unres_schema(module, unres)) {
            goto error;
        }

        /* check correctness of includes */
        if (lllyp_check_include_missing(module)) {
            goto error;
        }
    }

    lllyp_sort_revisions(module);

    if (lllyp_rfn_apply_ext(module) || lllyp_deviation_apply_ext(module)) {
        goto error;
    }

    if (revision) {
        /* check revision of the parsed model */
        if (!module->rev_size || strcmp(revision, module->rev[0].date)) {
            LOGVRB("Module \"%s\" parsed with the wrong revision (\"%s\" instead \"%s\").",
                   module->name, module->rev[0].date, revision);
            goto error;
        }
    }

    /* add into context if not already there */
    if (!ret) {
        if (lllyp_ctx_add_module(module)) {
            goto error;
        }

        /* remove our submodules from the parsed submodules list */
        lllyp_del_includedup(module, 0);
    } else {
        /* free what was parsed */
        lllys_free(module, NULL, 0, 0);

        /* get the model from the context */
        module = (struct lllys_module *)llly_ctx_get_module(ctx, value, revision, 0);
        assert(module);
    }

    unres_schema_free(NULL, &unres, 0);
    lllyp_check_circmod_pop(ctx);
    LOGVRB("Module \"%s%s%s\" successfully parsed as %s.", module->name, (module->rev_size ? "@" : ""),
           (module->rev_size ? module->rev[0].date : ""), (module->implemented ? "implemented" : "imported"));
    return module;

error:
    /* cleanup */
    unres_schema_free(module, &unres, 1);

    if (!module) {
        if (llly_vecode(ctx) != LLLYVE_SUBMODULE) {
            LOGERR(ctx, llly_errno, "Module parsing failed.");
        }
        return NULL;
    }

    LOGERR(ctx, llly_errno, "Module \"%s\" parsing failed.", module->name);

    lllyp_check_circmod_pop(ctx);
    lllys_sub_module_remove_devs_augs(module);
    lllyp_del_includedup(module, 1);
    lllys_free(module, NULL, 0, 1);
    return NULL;
}

/* logs directly */
struct lllys_module *
yin_read_module(struct llly_ctx *ctx, const char *data, const char *revision, int implement)
{
    struct lllyxml_elem *yin;
    struct lllys_module *result;

    yin = lllyxml_parse_mem(ctx, data, LLLYXML_PARSE_NOMIXEDCONTENT);
    if (!yin) {
        LOGERR(ctx, llly_errno, "Module parsing failed.");
        return NULL;
    }

    result = yin_read_module_(ctx, yin, revision, implement);

    lllyxml_free(ctx, yin);

    return result;
}

static int
yin_parse_extcomplex_bool(struct lllys_module *mod, struct lllyxml_elem *node,
                          struct lllys_ext_instance_complex *ext, LLLY_STMT stmt,
                          const char *true_val, const char *false_val, struct unres_schema *unres)
{
    uint8_t *val;
    const char *str;
    struct lllyext_substmt *info;

    val = lllys_ext_complex_get_substmt(stmt, ext, &info);
    if (!val) {
        LOGVAL(mod->ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, node->name, node->parent->name);
        return EXIT_FAILURE;
    }
    if (*val) {
        LOGVAL(mod->ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, node->parent->name);
        return EXIT_FAILURE;
    }

    if (lllyp_yin_parse_subnode_ext(mod, ext, LLLYEXT_PAR_EXTINST, node, (LLLYEXT_SUBSTMT)stmt, 0, unres)) {
        return EXIT_FAILURE;
    }

    str = lllyxml_get_attr(node, "value", NULL);
    if (!str) {
        LOGVAL(mod->ctx, LLLYE_MISSARG, LLLY_VLOG_NONE, NULL, "value", node->name);
    } else if (true_val && !strcmp(true_val, str)) {
        /* true value */
        *val = 1;
    } else if (false_val && !strcmp(false_val, str)) {
        /* false value */
        *val = 2;
    } else {
        /* unknown value */
        LOGVAL(mod->ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, str, node->name);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/*
 * argelem - 1 if the value is stored in a standalone YIN element, 0 if stored as attribute
 * argname - name of the element/attribute where the value is stored
 */
static int
yin_parse_extcomplex_str(struct lllys_module *mod, struct lllyxml_elem *node,
                         struct lllys_ext_instance_complex *ext, LLLY_STMT stmt,
                         int argelem, const char *argname, struct unres_schema *unres)
{
    int c;
    const char **str, ***p = NULL, *value;
    void *reallocated;
    struct lllyext_substmt *info;

    str = lllys_ext_complex_get_substmt(stmt, ext, &info);
    if (!str) {
        LOGVAL(mod->ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, node->name, node->parent->name);
        return EXIT_FAILURE;
    }
    if (info->cardinality < LLLY_STMT_CARD_SOME && *str) {
        LOGVAL(mod->ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, node->parent->name);
        return EXIT_FAILURE;
    }

    c = 0;
    if (info->cardinality >= LLLY_STMT_CARD_SOME) {
        /* there can be multiple instances, str is actually const char *** */
        p = (const char ***)str;
        if (!p[0]) {
            /* allocate initial array */
            p[0] = malloc(2 * sizeof(const char *));
            LLLY_CHECK_ERR_RETURN(!p[0], LOGMEM(mod->ctx), EXIT_FAILURE);
            if (stmt == LLLY_STMT_BELONGSTO) {
                /* allocate another array for the belongs-to's prefixes */
                p[1] = malloc(2 * sizeof(const char *));
                LLLY_CHECK_ERR_RETURN(!p[1], LOGMEM(mod->ctx), EXIT_FAILURE);
            } else if (stmt == LLLY_STMT_ARGUMENT) {
                /* allocate another array for the yin element */
                ((uint8_t **)p)[1] = malloc(2 * sizeof(uint8_t));
                LLLY_CHECK_ERR_RETURN(!p[1], LOGMEM(mod->ctx), EXIT_FAILURE);
            }
        } else {
            /* get the index in the array to add new item */
            for (c = 0; p[0][c]; c++);
        }
        str = p[0];
    }
    if (lllyp_yin_parse_subnode_ext(mod, ext, LLLYEXT_PAR_EXTINST, node, (LLLYEXT_SUBSTMT)stmt, c, unres)) {
        return EXIT_FAILURE;
    }

    if (argelem) {
        str[c] = read_yin_subnode(mod->ctx, node, argname);
        if (!str[c]) {
            return EXIT_FAILURE;
        }
    } else {
        str[c] = lllyxml_get_attr(node, argname, NULL);
        if (!str[c]) {
            LOGVAL(mod->ctx, LLLYE_MISSARG, LLLY_VLOG_NONE, NULL, argname, node->name);
            return EXIT_FAILURE;
        } else {
            str[c] = lllydict_insert(mod->ctx, str[c], 0);
        }

        if (stmt == LLLY_STMT_BELONGSTO) {
            /* get the belongs-to's mandatory prefix substatement */
            if (!node->child) {
                LOGVAL(mod->ctx, LLLYE_MISSCHILDSTMT, LLLY_VLOG_NONE, NULL, "prefix", node->name);
                return EXIT_FAILURE;
            } else if (strcmp(node->child->name, "prefix")) {
                LOGVAL(mod->ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->child->name);
                return EXIT_FAILURE;
            } else if (node->child->next) {
                LOGVAL(mod->ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->child->next->name);
                return EXIT_FAILURE;
            }
            /* and now finally get the value */
            if (p) {
                str = p[1];
            } else {
                str++;
            }
            str[c] = lllyxml_get_attr(node->child, "value", ((void *)0));
            if (!str[c]) {
                LOGVAL(mod->ctx, LLLYE_MISSARG, LLLY_VLOG_NONE, NULL, "value", node->child->name);
                return EXIT_FAILURE;
            }
            str[c] = lllydict_insert(mod->ctx, str[c], 0);

            if (!str[c] || lllyp_yin_parse_subnode_ext(mod, ext, LLLYEXT_PAR_EXTINST, node->child, LLLYEXT_SUBSTMT_PREFIX, c, unres)) {
                return EXIT_FAILURE;
            }
        } else if (stmt == LLLY_STMT_ARGUMENT) {
            str = (p) ? p[1] : str + 1;
            if (!node->child) {
                /* default value of yin element */
                ((uint8_t *)str)[c] = 2;
            } else {
                /* get optional yin-element substatement */
                if (strcmp(node->child->name, "yin-element")) {
                    LOGVAL(mod->ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->child->name);
                    return EXIT_FAILURE;
                } else if (node->child->next) {
                    LOGVAL(mod->ctx, LLLYE_INSTMT, LLLY_VLOG_NONE, NULL, node->child->next->name);
                    return EXIT_FAILURE;
                } else {
                    /* and now finally get the value */
                    value = lllyxml_get_attr(node->child, "value", NULL);
                    if (!value) {
                        LOGVAL(mod->ctx, LLLYE_MISSARG, LLLY_VLOG_NONE, NULL, "value", node->child->name);
                        return EXIT_FAILURE;
                    }
                    if (llly_strequal(value, "true", 0)) {
                        ((uint8_t *)str)[c] = 1;
                    } else if (llly_strequal(value, "false", 0)) {
                        ((uint8_t *)str)[c] = 2;
                    } else {
                        LOGVAL(mod->ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, str, node->name);
                        return EXIT_FAILURE;
                    }

                    if (lllyp_yin_parse_subnode_ext(mod, ext, LLLYEXT_PAR_EXTINST, node->child, LLLYEXT_SUBSTMT_YINELEM, c, unres)) {
                        return EXIT_FAILURE;
                    }
                }
            }
        }
    }
    if (p) {
        /* enlarge the array(s) */
        reallocated = realloc(p[0], (c + 2) * sizeof(const char *));
        if (!reallocated) {
            LOGMEM(mod->ctx);
            lllydict_remove(mod->ctx, p[0][c]);
            p[0][c] = NULL;
            return EXIT_FAILURE;
        }
        p[0] = reallocated;
        p[0][c + 1] = NULL;

        if (stmt == LLLY_STMT_BELONGSTO) {
            /* enlarge the second belongs-to's array with prefixes */
            reallocated = realloc(p[1], (c + 2) * sizeof(const char *));
            if (!reallocated) {
                LOGMEM(mod->ctx);
                lllydict_remove(mod->ctx, p[1][c]);
                p[1][c] = NULL;
                return EXIT_FAILURE;
            }
            p[1] = reallocated;
            p[1][c + 1] = NULL;
        } else if (stmt == LLLY_STMT_ARGUMENT){
            /* enlarge the second argument's array with yin element */
            reallocated = realloc(p[1], (c + 2) * sizeof(uint8_t));
            if (!reallocated) {
                LOGMEM(mod->ctx);
                ((uint8_t *)p[1])[c] = 0;
                return EXIT_FAILURE;
            }
            p[1] = reallocated;
            ((uint8_t *)p[1])[c + 1] = 0;
        }
    }

    return EXIT_SUCCESS;
}

static void *
yin_getplace_for_extcomplex_flags(struct lllyxml_elem *node, struct lllys_ext_instance_complex *ext, LLLY_STMT stmt,
                                  uint16_t mask)
{
    void *data;
    struct lllyext_substmt *info;

    data = lllys_ext_complex_get_substmt(stmt, ext, &info);
    if (!data) {
        LOGVAL(ext->module->ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, node->name, node->parent->name);
        return NULL;
    }
    if (info->cardinality < LLLY_STMT_CARD_SOME && ((*(uint16_t *)data) & mask)) {
        LOGVAL(ext->module->ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, node->parent->name);
        return NULL;
    }

    return data;
}

static int
yin_parse_extcomplex_flag(struct lllys_module *mod, struct lllyxml_elem *node,
                          struct lllys_ext_instance_complex *ext, LLLY_STMT stmt,
                          const char *val1_str, const char *val2_str, uint16_t mask,
                          uint16_t val1, uint16_t val2, struct unres_schema *unres)
{
    uint16_t *val;
    const char *str;

    val = yin_getplace_for_extcomplex_flags(node, ext, stmt, mask);
    if (!val) {
        return EXIT_FAILURE;
    }

    str = lllyxml_get_attr(node, "value", NULL);
    if (!str) {
        LOGVAL(mod->ctx, LLLYE_MISSARG, LLLY_VLOG_NONE, NULL, "value", node->name);
    } else if (!strcmp(val1_str, str)) {
        *val = *val | val1;
    } else if (!strcmp(val2_str, str)) {
        *val = *val | val2;
    } else {
        /* unknown value */
        LOGVAL(mod->ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, str, node->name);
        return EXIT_FAILURE;
    }
    if (lllyp_yin_parse_subnode_ext(mod, ext, LLLYEXT_PAR_EXTINST, node, (LLLYEXT_SUBSTMT)stmt, 0, unres)) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static struct lllys_node **
yin_getplace_for_extcomplex_node(struct lllyxml_elem *node, struct lllys_ext_instance_complex *ext, LLLY_STMT stmt)
{
    struct lllyext_substmt *info;
    struct lllys_node **snode, *siter;

    snode = lllys_ext_complex_get_substmt(stmt, ext, &info);
    if (!snode) {
        LOGVAL(ext->module->ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, node->name, node->parent->name);
        return NULL;
    }
    if (info->cardinality < LLLY_STMT_CARD_SOME) {
        LLLY_TREE_FOR(*snode, siter) {
            if (stmt == lllys_snode2stmt(siter->nodetype)) {
                LOGVAL(ext->module->ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, node->parent->name);
                return NULL;
            }
        }
    }

    return snode;
}

static void **
yin_getplace_for_extcomplex_struct(struct lllyxml_elem *node, struct lllys_ext_instance_complex *ext, LLLY_STMT stmt)
{
    int c;
    void **data, ***p = NULL;
    void *reallocated;
    struct lllyext_substmt *info;

    data = lllys_ext_complex_get_substmt(stmt, ext, &info);
    if (!data) {
        LOGVAL(ext->module->ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, node->name, node->parent->name);
        return NULL;
    }
    if (info->cardinality < LLLY_STMT_CARD_SOME && *data) {
        LOGVAL(ext->module->ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, node->parent->name);
        return NULL;
    }

    c = 0;
    if (info->cardinality >= LLLY_STMT_CARD_SOME) {
        /* there can be multiple instances, so instead of pointer to array,
         * we have in data pointer to pointer to array */
        p = (void ***)data;
        data = *p;
        if (!data) {
            /* allocate initial array */
            *p = data = malloc(2 * sizeof(void *));
            LLLY_CHECK_ERR_RETURN(!data, LOGMEM(ext->module->ctx), NULL);
        } else {
            for (c = 0; *data; data++, c++);
        }
    }

    if (p) {
        /* enlarge the array */
        reallocated = realloc(*p, (c + 2) * sizeof(void *));
        LLLY_CHECK_ERR_RETURN(!reallocated, LOGMEM(ext->module->ctx), NULL);
        *p = reallocated;
        data = *p;
        data[c + 1] = NULL;
    }

    return &data[c];
}

int
lllyp_yin_parse_complex_ext(struct lllys_module *mod, struct lllys_ext_instance_complex *ext, struct lllyxml_elem *yin,
                          struct unres_schema *unres)
{
    struct lllyxml_elem *next, *node, *child;
    struct lllys_type **type;
    void **pp, *p, *reallocated;
    const char *value, *name;
    char *endptr, modifier;
    struct lllyext_substmt *info;
    long int v;
    long long int ll;
    unsigned long u;
    int i, j;

#define YIN_STORE_VALUE(TYPE, FROM, TO)           \
    *(TYPE **)TO = malloc(sizeof(TYPE));          \
    if (!*(TYPE **)TO) { LOGMEM(mod->ctx); goto error; }    \
    (**(TYPE **)TO) = (TYPE)FROM;

#define YIN_EXTCOMPLEX_GETPLACE(STMT, TYPE)                                          \
    p = lllys_ext_complex_get_substmt(STMT, ext, &info);                               \
    if (!p) {                                                                        \
        LOGVAL(mod->ctx, LLLYE_INCHILDSTMT, LLLY_VLOG_NONE, NULL, node->name, node->parent->name); \
        goto error;                                                                  \
    }                                                                                \
    if (info->cardinality < LLLY_STMT_CARD_SOME && (*(TYPE*)p)) {                      \
        LOGVAL(mod->ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, node->parent->name);     \
        goto error;                                                                  \
    }                                                                                \
    pp = NULL; i = 0;                                                                \
    if (info->cardinality >= LLLY_STMT_CARD_SOME) {                                    \
        /* there can be multiple instances */                                        \
        pp = p;                                                                      \
        if (!(*pp)) {                                                                \
            *pp = malloc(2 * sizeof(TYPE)); /* allocate initial array */             \
            LLLY_CHECK_ERR_GOTO(!*pp, LOGMEM(mod->ctx), error);                        \
        } else {                                                                     \
            for (i = 0; (*(TYPE**)pp)[i]; i++);                                      \
        }                                                                            \
        p = &(*(TYPE**)pp)[i];                                                       \
    }
#define YIN_EXTCOMPLEX_ENLARGE(TYPE)                         \
    if (pp) {                                                \
        /* enlarge the array */                              \
        reallocated = realloc(*pp, (i + 2) * sizeof(TYPE*)); \
        LLLY_CHECK_ERR_GOTO(!reallocated, LOGMEM(mod->ctx), error);      \
        *pp = reallocated;                                   \
        (*(TYPE**)pp)[i + 1] = 0;                            \
    }
#define YIN_EXTCOMPLEX_PARSE_SNODE(STMT, FUNC, ARGS...)                              \
    pp = (void**)yin_getplace_for_extcomplex_node(node, ext, STMT);                  \
    if (!pp) { goto error; }                                                         \
    if (!FUNC(mod, (struct lllys_node*)ext, node, ##ARGS, LLLYS_PARSE_OPT_CFG_NOINHERIT, unres)) { goto error; }
#define YIN_EXTCOMPLEX_PARSE_RESTR(STMT)                                             \
    YIN_EXTCOMPLEX_GETPLACE(STMT, struct lllys_restr*);                                \
    GETVAL(mod->ctx, value, node, "value");                                                    \
    *(struct lllys_restr **)p = calloc(1, sizeof(struct lllys_restr));                   \
    LLLY_CHECK_ERR_GOTO(!*(struct lllys_restr **)p, LOGMEM(mod->ctx), error);            \
    (*(struct lllys_restr **)p)->expr = lllydict_insert(mod->ctx, value, 0);             \
    if (read_restr_substmt(mod, *(struct lllys_restr **)p, node, unres)) {             \
        goto error;                                                                  \
    }                                                                                \
    YIN_EXTCOMPLEX_ENLARGE(struct lllys_restr*);

    LLLY_TREE_FOR_SAFE(yin->child, next, node) {
        if (!node->ns) {
            /* garbage */
        } else if (node->ns == yin->ns && (ext->flags & LLLYS_YINELEM) && llly_strequal(node->name, ext->def->argument, 1)) {
            /* we have the extension's argument */
            if (ext->arg_value) {
                LOGVAL(mod->ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, node->name, yin->name);
                goto error;
            }
            ext->arg_value = node->content;
            node->content = NULL;
        } else if (strcmp(node->ns->value, LLLY_NSYIN)) {
            /* extension */
            if (lllyp_yin_parse_subnode_ext(mod, ext, LLLYEXT_PAR_EXTINST, node, LLLYEXT_SUBSTMT_SELF, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "description")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_DESCRIPTION, 1, "text", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "reference")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_REFERENCE, 1, "text", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "units")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_UNITS, 0, "name", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "type")) {
            type = (struct lllys_type **)yin_getplace_for_extcomplex_struct(node, ext, LLLY_STMT_TYPE);
            if (!type) {
                goto error;
            }
            /* allocate type structure */
            (*type) = calloc(1, sizeof **type);
            LLLY_CHECK_ERR_GOTO(!*type, LOGMEM(mod->ctx), error);

            /* HACK for unres */
            lllyxml_unlink(mod->ctx, node);
            (*type)->der = (struct lllys_tpdf *)node;
            (*type)->parent = (struct lllys_tpdf *)ext;

            if (unres_schema_add_node(mod, unres, *type, UNRES_TYPE_DER_EXT, NULL) == -1) {
                (*type)->der = NULL;
                goto error;
            }
            continue; /* skip lllyxml_free() */
        } else if (!strcmp(node->name, "typedef")) {
            pp = yin_getplace_for_extcomplex_struct(node, ext, LLLY_STMT_TYPEDEF);
            if (!pp) {
                goto error;
            }
            /* allocate typedef structure */
            (*pp) = calloc(1, sizeof(struct lllys_tpdf));
            LLLY_CHECK_ERR_GOTO(!*pp, LOGMEM(mod->ctx), error);

            if (fill_yin_typedef(mod, (struct lllys_node *)ext, node, *((struct lllys_tpdf **)pp), unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "if-feature")) {
            pp = yin_getplace_for_extcomplex_struct(node, ext, LLLY_STMT_IFFEATURE);
            if (!pp) {
                goto error;
            }
            /* allocate iffeature structure */
            (*pp) = calloc(1, sizeof(struct lllys_iffeature));
            LLLY_CHECK_ERR_GOTO(!*pp, LOGMEM(mod->ctx), error);

            if (fill_yin_iffeature((struct lllys_node *)ext, 0, node, *((struct lllys_iffeature **)pp), unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "status")) {
            p = yin_getplace_for_extcomplex_flags(node, ext, LLLY_STMT_STATUS, LLLYS_STATUS_MASK);
            if (!p) {
                goto error;
            }

            GETVAL(mod->ctx, value, node, "value");
            if (!strcmp(value, "current")) {
                *(uint16_t*)p |= LLLYS_STATUS_CURR;
            } else if (!strcmp(value, "deprecated")) {
                *(uint16_t*)p |= LLLYS_STATUS_DEPRC;
            } else if (!strcmp(value, "obsolete")) {
                *(uint16_t*)p |= LLLYS_STATUS_OBSLT;
            } else {
                LOGVAL(mod->ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, node->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(mod, ext, LLLYEXT_PAR_EXTINST, node, LLLYEXT_SUBSTMT_STATUS, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "config")) {
            if (yin_parse_extcomplex_flag(mod, node, ext, LLLY_STMT_MANDATORY, "true", "false", LLLYS_CONFIG_MASK,
                                          LLLYS_CONFIG_W | LLLYS_CONFIG_SET, LLLYS_CONFIG_R | LLLYS_CONFIG_SET, unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "argument")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_ARGUMENT, 0, "name", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "default")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_DEFAULT, 0, "value", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "mandatory")) {
            if (yin_parse_extcomplex_flag(mod, node, ext, LLLY_STMT_MANDATORY,
                                         "true", "false", LLLYS_MAND_MASK, LLLYS_MAND_TRUE, LLLYS_MAND_FALSE, unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "error-app-tag")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_ERRTAG, 0, "value", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "error-message")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_ERRMSG, 1, "value", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "prefix")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_PREFIX, 0, "value", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "namespace")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_NAMESPACE, 0, "uri", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "presence")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_PRESENCE, 0, "value", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "revision-date")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_REVISIONDATE, 0, "date", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "key")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_KEY, 0, "value", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "base")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_BASE, 0, "name", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "ordered-by")) {
            if (yin_parse_extcomplex_flag(mod, node, ext, LLLY_STMT_ORDEREDBY,
                                          "user", "system", LLLYS_USERORDERED, LLLYS_USERORDERED, 0, unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "belongs-to")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_BELONGSTO, 0, "module", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "contact")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_CONTACT, 1, "text", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "organization")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_ORGANIZATION, 1, "text", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "path")) {
            if (yin_parse_extcomplex_str(mod, node, ext, LLLY_STMT_PATH, 0, "value", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "require-instance")) {
            if (yin_parse_extcomplex_bool(mod, node, ext, LLLY_STMT_REQINSTANCE, "true", "false", unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "modifier")) {
            if (yin_parse_extcomplex_bool(mod, node, ext, LLLY_STMT_MODIFIER, "invert-match", NULL, unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "fraction-digits")) {
            YIN_EXTCOMPLEX_GETPLACE(LLLY_STMT_DIGITS, uint8_t);

            GETVAL(mod->ctx, value, node, "value");
            v = strtol(value, NULL, 10);

            /* range check */
            if (v < 1 || v > 18) {
                LOGVAL(mod->ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, node->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(mod, ext, LLLYEXT_PAR_EXTINST, node, LLLYEXT_SUBSTMT_STATUS, i, unres)) {
                goto error;
            }

            /* store the value */
            (*(uint8_t *)p) = (uint8_t)v;

            YIN_EXTCOMPLEX_ENLARGE(uint8_t);
        } else if (!strcmp(node->name, "max-elements")) {
            YIN_EXTCOMPLEX_GETPLACE(LLLY_STMT_MAX, uint32_t*);

            GETVAL(mod->ctx, value, node, "value");
            while (isspace(value[0])) {
                value++;
            }

            if (!strcmp(value, "unbounded")) {
                u = 0;
            } else {
                /* convert it to uint32_t */
                errno = 0; endptr = NULL;
                u = strtoul(value, &endptr, 10);
                if (*endptr || value[0] == '-' || errno || u == 0 || u > UINT32_MAX) {
                    LOGVAL(mod->ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, node->name);
                    goto error;
                }
            }

            if (lllyp_yin_parse_subnode_ext(mod, ext, LLLYEXT_PAR_EXTINST, node, LLLYEXT_SUBSTMT_MAX, i, unres)) {
                goto error;
            }

            /* store the value */
            YIN_STORE_VALUE(uint32_t, u, p)

            YIN_EXTCOMPLEX_ENLARGE(uint32_t*);
        } else if (!strcmp(node->name, "min-elements")) {
            YIN_EXTCOMPLEX_GETPLACE(LLLY_STMT_MIN, uint32_t*);

            GETVAL(mod->ctx, value, node, "value");
            while (isspace(value[0])) {
                value++;
            }

            /* convert it to uint32_t */
            errno = 0;
            endptr = NULL;
            u = strtoul(value, &endptr, 10);
            if (*endptr || value[0] == '-' || errno || u > UINT32_MAX) {
                LOGVAL(mod->ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, node->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(mod, ext, LLLYEXT_PAR_EXTINST, node, LLLYEXT_SUBSTMT_MAX, i, unres)) {
                goto error;
            }

            /* store the value */
            YIN_STORE_VALUE(uint32_t, u, p)

            YIN_EXTCOMPLEX_ENLARGE(uint32_t*);
        } else if (!strcmp(node->name, "value")) {
            YIN_EXTCOMPLEX_GETPLACE(LLLY_STMT_VALUE, int32_t*);

            GETVAL(mod->ctx, value, node, "value");
            while (isspace(value[0])) {
                value++;
            }

            /* convert it to int32_t */
            ll = strtoll(value, NULL, 10);

            /* range check */
            if (ll < INT32_MIN || ll > INT32_MAX) {
                LOGVAL(mod->ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, node->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(mod, ext, LLLYEXT_PAR_EXTINST, node, LLLYEXT_SUBSTMT_VALUE, i, unres)) {
                goto error;
            }

            /* store the value */
            YIN_STORE_VALUE(int32_t, ll, p)

            YIN_EXTCOMPLEX_ENLARGE(int32_t*);
        } else if (!strcmp(node->name, "position")) {
            YIN_EXTCOMPLEX_GETPLACE(LLLY_STMT_POSITION, uint32_t*);

            GETVAL(mod->ctx, value, node, "value");
            ll = strtoll(value, NULL, 10);

            /* range check */
            if (ll < 0 || ll > UINT32_MAX) {
                LOGVAL(mod->ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, value, node->name);
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(mod, ext, LLLYEXT_PAR_EXTINST, node, LLLYEXT_SUBSTMT_POSITION, i, unres)) {
                goto error;
            }

            /* store the value */
            YIN_STORE_VALUE(uint32_t, ll, p)

            YIN_EXTCOMPLEX_ENLARGE(uint32_t*);
        } else if (!strcmp(node->name, "module")) {
            pp = yin_getplace_for_extcomplex_struct(node, ext, LLLY_STMT_MODULE);
            if (!pp) {
                goto error;
            }

            *(struct lllys_module **)pp = yin_read_module_(mod->ctx, node, NULL, mod->implemented);
            if (!(*pp)) {
                goto error;
            }
        } else if (!strcmp(node->name, "when")) {
            YIN_EXTCOMPLEX_GETPLACE(LLLY_STMT_WHEN, struct lllys_when*);

            *(struct lllys_when**)p = read_yin_when(mod, node, unres);
            if (!*(struct lllys_when**)p) {
                goto error;
            }

            YIN_EXTCOMPLEX_ENLARGE(struct lllys_when*);
        } else if (!strcmp(node->name, "revision")) {
            YIN_EXTCOMPLEX_GETPLACE(LLLY_STMT_REVISION, struct lllys_revision*);

            *(struct lllys_revision**)p = calloc(1, sizeof(struct lllys_revision));
            LLLY_CHECK_ERR_GOTO(!*(struct lllys_revision**)p, LOGMEM(mod->ctx), error);
            if (fill_yin_revision(mod, node, *(struct lllys_revision**)p, unres)) {
                goto error;
            }

            /* check uniqueness of the revision dates - not required by RFC */
            if (pp) {
                for (j = 0; j < i; j++) {
                    if (!strcmp((*(struct lllys_revision***)pp)[j]->date, (*(struct lllys_revision**)p)->date)) {
                        LOGWRN(mod->ctx, "Module's revisions are not unique (%s).", (*(struct lllys_revision**)p)->date);
                    }
                }
            }

            YIN_EXTCOMPLEX_ENLARGE(struct lllys_revision*);
        } else if (!strcmp(node->name, "unique")) {
            YIN_EXTCOMPLEX_GETPLACE(LLLY_STMT_UNIQUE, struct lllys_unique*);

            *(struct lllys_unique**)p = calloc(1, sizeof(struct lllys_unique));
            LLLY_CHECK_ERR_GOTO(!*(struct lllys_unique**)p, LOGMEM(mod->ctx), error);
            if (fill_yin_unique(mod, (struct lllys_node*)ext, node, *(struct lllys_unique**)p, unres)) {
                goto error;
            }

            if (lllyp_yin_parse_subnode_ext(mod, ext, LLLYEXT_PAR_EXTINST, node, LLLYEXT_SUBSTMT_UNIQUE, i, unres)) {
                goto error;
            }
            YIN_EXTCOMPLEX_ENLARGE(struct lllys_unique*);
        } else if (!strcmp(node->name, "action")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_ACTION, read_yin_rpc_action);
        } else if (!strcmp(node->name, "anydata")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_ANYDATA, read_yin_anydata, LLLYS_ANYDATA);
        } else if (!strcmp(node->name, "anyxml")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_ANYXML, read_yin_anydata, LLLYS_ANYXML);
        } else if (!strcmp(node->name, "case")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_CASE, read_yin_case);
        } else if (!strcmp(node->name, "choice")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_CHOICE, read_yin_choice);
        } else if (!strcmp(node->name, "container")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_CONTAINER, read_yin_container);
        } else if (!strcmp(node->name, "grouping")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_GROUPING, read_yin_grouping);
        } else if (!strcmp(node->name, "output")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_OUTPUT, read_yin_input_output);
        } else if (!strcmp(node->name, "input")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_INPUT, read_yin_input_output);
        } else if (!strcmp(node->name, "leaf")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_LEAF, read_yin_leaf);
        } else if (!strcmp(node->name, "leaf-list")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_LEAFLIST, read_yin_leaflist);
        } else if (!strcmp(node->name, "list")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_LIST, read_yin_list);
        } else if (!strcmp(node->name, "notification")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_NOTIFICATION, read_yin_notif);
        } else if (!strcmp(node->name, "uses")) {
            YIN_EXTCOMPLEX_PARSE_SNODE(LLLY_STMT_USES, read_yin_uses);
        } else if (!strcmp(node->name, "length")) {
            YIN_EXTCOMPLEX_PARSE_RESTR(LLLY_STMT_LENGTH);
        } else if (!strcmp(node->name, "must")) {
            pp = yin_getplace_for_extcomplex_struct(node, ext, LLLY_STMT_MUST);
            if (!pp) {
                goto error;
            }
            /* allocate structure for must */
            (*pp) = calloc(1, sizeof(struct lllys_restr));
            LLLY_CHECK_ERR_GOTO(!*pp, LOGMEM(mod->ctx), error);

            if (fill_yin_must(mod, node, *((struct lllys_restr **)pp), unres)) {
                goto error;
            }
        } else if (!strcmp(node->name, "pattern")) {
            YIN_EXTCOMPLEX_GETPLACE(LLLY_STMT_PATTERN, struct lllys_restr*);
            GETVAL(mod->ctx, value, node, "value");
            if (lllyp_check_pattern(mod->ctx, value, NULL)) {
                goto error;
            }

            *(struct lllys_restr **)p = calloc(1, sizeof(struct lllys_restr));
            LLLY_CHECK_ERR_GOTO(!*(struct lllys_restr **)p, LOGMEM(mod->ctx), error);

            modifier = 0x06; /* ACK */
            if (mod->version >= 2) {
                name = NULL;
                LLLY_TREE_FOR(node->child, child) {
                    if (child->ns && !strcmp(child->ns->value, LLLY_NSYIN) && !strcmp(child->name, "modifier")) {
                        if (name) {
                            LOGVAL(mod->ctx, LLLYE_TOOMANY, LLLY_VLOG_NONE, NULL, "modifier", node->name);
                            goto error;
                        }

                        GETVAL(mod->ctx, name, child, "value");
                        if (!strcmp(name, "invert-match")) {
                            modifier = 0x15; /* NACK */
                        } else {
                            LOGVAL(mod->ctx, LLLYE_INARG, LLLY_VLOG_NONE, NULL, name, "modifier");
                            goto error;
                        }
                        /* get extensions of the modifier */
                        if (lllyp_yin_parse_subnode_ext(mod, *(struct lllys_restr **)p, LLLYEXT_PAR_RESTR, child,
                                                      LLLYEXT_SUBSTMT_MODIFIER, 0, unres)) {
                            goto error;
                        }
                    }
                }
            }

            /* store the value: modifier byte + value + terminating NULL byte */
            (*(struct lllys_restr **)p)->expr = malloc((strlen(value) + 2) * sizeof(char));
            LLLY_CHECK_ERR_GOTO(!(*(struct lllys_restr **)p)->expr, LOGMEM(mod->ctx), error);
            ((char *)(*(struct lllys_restr **)p)->expr)[0] = modifier;
            strcpy(&((char *)(*(struct lllys_restr **)p)->expr)[1], value);
            lllydict_insert_zc(mod->ctx, (char *)(*(struct lllys_restr **)p)->expr);

            /* get possible sub-statements */
            if (read_restr_substmt(mod, *(struct lllys_restr **)p, node, unres)) {
                goto error;
            }

            YIN_EXTCOMPLEX_ENLARGE(struct lllys_restr*);
        } else if (!strcmp(node->name, "range")) {
            YIN_EXTCOMPLEX_PARSE_RESTR(LLLY_STMT_RANGE);
        } else {
            LOGERR(mod->ctx, llly_errno, "Extension's substatement \"%s\" not supported.", node->name);
        }
        lllyxml_free(mod->ctx, node);
    }

    if (ext->substmt && lllyp_mand_check_ext(ext, yin->name)) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;

error:
    return EXIT_FAILURE;
}
