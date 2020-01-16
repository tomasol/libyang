/**
 * @file printer/tree.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief TREE printer for libyang data model structure
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "common.h"
#include "printer.h"
#include "tree_schema.h"

/* module: <name>
 * <X>+--rw <node-name> */
#define LLLY_TREE_MOD_DATA_INDENT 2

/* <^>rpcs:
 * <X>+---x <rpc-name> */
#define LLLY_TREE_OP_DATA_INDENT 4

/* +--rw leaf<X>string */
#define LLLY_TREE_TYPE_INDENT 3

/* +--rw leaf
 * |     <X>string */
#define LLLY_TREE_WRAP_INDENT 2

/* these options are mostly inherited in recursive print, non-recursive options are parameters */
typedef struct {
    const struct lllys_module *module; /**< (sub)module we are printing from */
    uint8_t base_indent;             /**< base indent size of all the printed text */
    uint64_t indent;                 /**< bit-field of sibling (1)/ no sibling(0) on corresponding depths */
    uint16_t line_length;            /**< maximum desired line length */
    int spec_config;                 /**< special config flags - 0 (no special config status),
                                          1 (read-only - rpc output, notification), 2 (write-only - rpc input) */
    int options;                     /**< user-specified tree printer options */
} tp_opts;

static void tree_print_snode(struct lllyout *out, int level, uint16_t max_name_len, const struct lllys_node *node, int mask,
                             const struct lllys_node *aug_parent, int subtree, tp_opts *opts);

static int
tree_print_indent(struct lllyout *out, int level, tp_opts *opts)
{
    int i, ret = 0;

    if (opts->base_indent) {
        ret += llly_print(out, "%*s", opts->base_indent, " ");
    }
    for (i = 0; i < level; ++i) {
        if (opts->indent & (1 << i)) {
            ret += llly_print(out, "|  ");
        } else {
            ret += llly_print(out, "   ");
        }
    }

    return ret;
}

static int
tree_sibling_is_valid_child(const struct lllys_node *node, int including, const struct lllys_module *module,
                            const struct lllys_node *aug_parent, LLLYS_NODE nodetype)
{
    struct lllys_node *cur, *cur2;

    assert(!aug_parent || (aug_parent->nodetype == LLLYS_AUGMENT));

    if (!node) {
        return 0;
    } else if (!lllys_parent(node) && !strcmp(node->name, "config") && !strcmp(node->module->name, "ietf-netconf")) {
        /* node added by libyang, not actually in the model */
        return 0;
    }

    /* has a following printed child */
    LLLY_TREE_FOR((struct lllys_node *)(including ? node : node->next), cur) {
        if (aug_parent && (cur->parent != aug_parent)) {
            /* we are done traversing this augment, the nodes are all direct siblings */
            return 0;
        }

        if (module->type && (lllys_main_module(module) != lllys_node_module(cur))) {
            continue;
        }

        if (!lllys_is_disabled(cur, 0)) {
            if ((cur->nodetype == LLLYS_USES) || ((cur->nodetype == LLLYS_CASE) && (cur->flags & LLLYS_IMPLICIT))) {
                if (tree_sibling_is_valid_child(cur->child, 1, module, NULL, nodetype)) {
                    return 1;
                }
            } else {
                switch (nodetype) {
                case LLLYS_GROUPING:
                    /* we are printing groupings, they are printed separately */
                    if (cur->nodetype == LLLYS_GROUPING) {
                        return 0;
                    }
                    break;
                case LLLYS_RPC:
                    if (cur->nodetype == LLLYS_RPC) {
                        return 1;
                    }
                    break;
                case LLLYS_NOTIF:
                    if (cur->nodetype == LLLYS_NOTIF) {
                        return 1;
                    }
                    break;
                default:
                    if (cur->nodetype & (LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA | LLLYS_CHOICE
                            | LLLYS_CASE | LLLYS_ACTION)) {
                        return 1;
                    }
                    if ((cur->nodetype & (LLLYS_INPUT | LLLYS_OUTPUT)) && cur->child) {
                        return 1;
                    }
                    /* only nested notifications count here (not top-level) */
                    if (cur->nodetype == LLLYS_NOTIF) {
                        for (cur2 = lllys_parent(cur); cur2 && (cur2->nodetype == LLLYS_USES); cur2 = lllys_parent(cur2));
                        if (cur2) {
                            return 1;
                        }
                    }
                    break;
                }
            }
        }
    }

    /* if in uses, the following printed child can actually be in the parent node :-/ */
    if (lllys_parent(node) && (lllys_parent(node)->nodetype == LLLYS_USES)) {
        return tree_sibling_is_valid_child(lllys_parent(node), 0, module, NULL, nodetype);
    }

    return 0;
}

static void
tree_next_indent(int level, const struct lllys_node *node, const struct lllys_node *aug_parent, tp_opts *opts)
{
    int next_is_case = 0, has_next = 0;

    if (level > 64) {
        LOGINT(node->module->ctx);
        return;
    }

    /* clear level indent (it may have been set for some line wrapping) */
    opts->indent &= ~(uint64_t)(1ULL << (level - 1));

    /* this is the direct child of a case */
    if ((node->nodetype != LLLYS_CASE) && lllys_parent(node) && (lllys_parent(node)->nodetype & (LLLYS_CASE | LLLYS_CHOICE))) {
        /* it is not the only child */
        if (node->next && lllys_parent(node->next) && (lllys_parent(node->next)->nodetype == LLLYS_CHOICE)) {
            next_is_case = 1;
        }
    }

    /* next is a node that will actually be printed */
    has_next = tree_sibling_is_valid_child(node, 0, opts->module, aug_parent, node->nodetype);

    /* set level indent */
    if (has_next && !next_is_case) {
        opts->indent |= (uint64_t)1ULL << (level - 1);
    }
}

static uint16_t
tree_get_max_name_len(const struct lllys_node *sibling, const struct lllys_node *aug_parent, int type_mask,
                      tp_opts *opts)
{
    const struct lllys_node *sub;
    struct lllys_module *nodemod;
    unsigned int max_name_len = 0, name_len;

    LLLY_TREE_FOR(sibling, sub) {
        if (opts->module->type && (sub->module != opts->module)) {
            /* when printing submodule, we are only concerned with its own data (they are in the module data) */
            continue;
        }
        if (aug_parent && (sub->parent != aug_parent)) {
            /* when printing augment children, skip other target children */
            continue;
        }
        if (!(sub->nodetype & type_mask)) {
            /* this sibling will not be printed */
            continue;
        }

        if ((sub->nodetype == LLLYS_USES) && !(opts->options & LLLYS_OUTOPT_TREE_USES)) {
            name_len = tree_get_max_name_len(sub->child, NULL, type_mask, opts);
        } else {
            nodemod = lllys_node_module(sub);
            name_len = strlen(sub->name);
            if (lllys_main_module(opts->module) != nodemod) {
                /* ":" */
                ++name_len;
                if (opts->options & LLLYS_OUTOPT_TREE_RFC) {
                    name_len += strlen(nodemod->prefix);
                } else {
                    name_len += strlen(nodemod->name);
                }
            }

            /* add characters for optional opts */
            switch (sub->nodetype) {
            case LLLYS_LEAF:
            case LLLYS_LEAFLIST:
            case LLLYS_LIST:
            case LLLYS_ANYDATA:
            case LLLYS_ANYXML:
            case LLLYS_CONTAINER:
            case LLLYS_CASE:
                ++name_len;
                break;
            case LLLYS_CHOICE:
                /* choice is longer :-/ */
                name_len += 2;
                if (!(sub->flags & LLLYS_MAND_TRUE)) {
                    ++name_len;
                }
                break;
            default:
                break;
            }
        }

        if (name_len > max_name_len) {
            max_name_len = name_len;
        }
    }

    return max_name_len;
}

static int
tree_leaf_is_mandatory(const struct lllys_node *node)
{
    const struct lllys_node *parent;
    struct lllys_node_list *list;
    uint16_t i;

    for (parent = lllys_parent(node); parent && parent->nodetype == LLLYS_USES; parent = lllys_parent(parent));
    if (parent && parent->nodetype == LLLYS_LIST) {
        list = (struct lllys_node_list *)parent;
        for (i = 0; i < list->keys_size; i++) {
            if (list->keys[i] == (struct lllys_node_leaf *)node) {
                return 1;
            }
        }
    }

    return 0;
}

static int
tree_print_wrap(struct lllyout *out, int level, int line_printed, uint8_t indent, uint16_t len, tp_opts *opts)
{
    if (opts->line_length && (line_printed + indent + len > opts->line_length)) {
        llly_print(out, "\n");
        line_printed = tree_print_indent(out, level, opts);
        /* 3 for config + space */
        line_printed += llly_print(out, "%*s", 3 + LLLY_TREE_WRAP_INDENT, "");
    } else {
        line_printed += llly_print(out, "%*s", indent, "");
    }

    return line_printed;
}

static int
tree_print_prefix(struct lllyout *out, const struct lllys_node *node, tp_opts *opts)
{
    uint16_t ret = 0;
    const struct lllys_module *nodemod;

    nodemod = lllys_node_module(node);
    if (lllys_main_module(opts->module) != nodemod) {
        if (opts->options & LLLYS_OUTOPT_TREE_RFC) {
            ret = llly_print(out, "%s:", nodemod->prefix);
        } else {
            ret = llly_print(out, "%s:", nodemod->name);
        }
    }

    return ret;
}

static int
tree_print_type(struct lllyout *out, const struct lllys_type *type, int options, const char **out_str)
{
    struct lllys_module *type_mod = ((struct lllys_tpdf *)type->parent)->module;
    const char *str;
    char *tmp;
    int printed;

    if ((type->base == LLLY_TYPE_LEAFREF) && !type->der->module) {
        if (options & LLLYS_OUTOPT_TREE_NO_LEAFREF) {
            if (out_str) {
                printed = 7;
                *out_str = lllydict_insert(type_mod->ctx, "leafref", printed);
            } else {
                printed = llly_print(out, "leafref");
            }
        } else {
            if (options & LLLYS_OUTOPT_TREE_RFC) {
                str = transform_json2schema(type_mod, type->info.lref.path);
                if (out_str) {
                    printed = 3 + strlen(str);
                    tmp = malloc(printed + 1);
                    LLLY_CHECK_ERR_RETURN(!tmp, LOGMEM(type_mod->ctx), 0);
                    sprintf(tmp, "-> %s", str);
                    *out_str = lllydict_insert_zc(type_mod->ctx, tmp);
                } else {
                    printed = llly_print(out, "-> %s", str);
                }
                lllydict_remove(type_mod->ctx, str);
            } else {
                if (out_str) {
                    printed = 3 + strlen(type->info.lref.path);
                    tmp = malloc(printed + 1);
                    LLLY_CHECK_ERR_RETURN(!tmp, LOGMEM(type_mod->ctx), 0);
                    sprintf(tmp, "-> %s", type->info.lref.path);
                    *out_str = lllydict_insert_zc(type_mod->ctx, tmp);
                } else {
                    printed = llly_print(out, "-> %s", type->info.lref.path);
                }
            }
        }
    } else if (!lllys_type_is_local(type)) {
        if (options & LLLYS_OUTOPT_TREE_RFC) {
            str = transform_module_name2import_prefix(type_mod, type->der->module->name);
            if (out_str) {
                printed = strlen(str) + 1 + strlen(type->der->name);
                tmp = malloc(printed + 1);
                LLLY_CHECK_ERR_RETURN(!tmp, LOGMEM(type_mod->ctx), 0);
                sprintf(tmp, "%s:%s", str, type->der->name);
                *out_str = lllydict_insert_zc(type_mod->ctx, tmp);
            } else {
                printed = llly_print(out, "%s:%s", str, type->der->name);
            }
        } else {
            if (out_str) {
                printed = strlen(type->der->module->name) + 1 + strlen(type->der->name);
                tmp = malloc(printed + 1);
                LLLY_CHECK_ERR_RETURN(!tmp, LOGMEM(type_mod->ctx), 0);
                sprintf(tmp, "%s:%s", type->der->module->name, type->der->name);
                *out_str = lllydict_insert_zc(type_mod->ctx, tmp);
            } else {
                printed = llly_print(out, "%s:%s", type->der->module->name, type->der->name);
            }
        }
    } else {
        if (out_str) {
            printed = strlen(type->der->name);
            *out_str = lllydict_insert(type_mod->ctx, type->der->name, printed);
        } else {
            printed = llly_print(out, "%s", type->der->name);
        }
    }

    return printed;
}

static int
tree_print_config(struct lllyout *out, const struct lllys_node *node, int spec_config)
{
    int ret;

    switch (node->nodetype) {
    case LLLYS_RPC:
    case LLLYS_ACTION:
        return llly_print(out, "-x ");
    case LLLYS_NOTIF:
        return llly_print(out, "-n ");
    case LLLYS_USES:
        return llly_print(out, "-u ");
    case LLLYS_CASE:
        return llly_print(out, ":(");
    default:
        break;
    }

    if (spec_config == 1) {
        ret = llly_print(out, "-w ");
    } else if (spec_config == 2) {
        ret = llly_print(out, "ro ");
    } else {
        ret = llly_print(out, "%s ", (node->flags & LLLYS_CONFIG_W) ? "rw" : (node->flags & LLLYS_CONFIG_R) ? "ro" : "--");
    }

    if (node->nodetype == LLLYS_CHOICE) {
        ret += llly_print(out, "(");
    }
    return ret;
}

static int
tree_print_features(struct lllyout *out, struct lllys_iffeature *iff1, uint8_t iff1_size, struct lllys_iffeature *iff2,
                    uint8_t iff2_size, tp_opts *opts, const char **out_str)
{
    int i, printed;
    struct lllyout *o;

    if (!iff1_size && !iff2_size) {
        return 0;
    }

    if (out_str) {
        o = malloc(sizeof *o);
        LLLY_CHECK_ERR_RETURN(!o, LOGMEM(NULL), 0);
        o->type = LLLYOUT_MEMORY;
        o->method.mem.buf = NULL;
        o->method.mem.len = 0;
        o->method.mem.size = 0;
    } else {
        o = out;
    }

    printed = llly_print(o, "{");
    for (i = 0; i < iff1_size; i++) {
        if (i > 0) {
            printed += llly_print(o, ",");
        }
        printed += llly_print_iffeature(o, opts->module, &iff1[i], opts->options & LLLYS_OUTOPT_TREE_RFC ? 2 : 1);
    }
    for (i = 0; i < iff2_size; i++) {
        if (i > 0) {
            printed += llly_print(o, ",");
        }
        printed += llly_print_iffeature(o, opts->module, &iff2[i], opts->options & LLLYS_OUTOPT_TREE_RFC ? 2 : 1);
    }
    printed += llly_print(o, "}?");

    if (out_str) {
        *out_str = lllydict_insert_zc(opts->module->ctx, o->method.mem.buf);
        free(o);
    }

    return printed;
}

static int
tree_print_keys(struct lllyout *out, struct lllys_node_leaf **keys, uint8_t keys_size, tp_opts *opts, const char **out_str)
{
    int i, printed;
    struct lllyout *o;

    if (!keys_size) {
        return 0;
    }

    if (out_str) {
        o = malloc(sizeof *o);
        LLLY_CHECK_ERR_RETURN(!o, LOGMEM(NULL), 0);
        o->type = LLLYOUT_MEMORY;
        o->method.mem.buf = NULL;
        o->method.mem.len = 0;
        o->method.mem.size = 0;
    } else {
        o = out;
    }

    printed = llly_print(o, "[");
    for (i = 0; i < keys_size; i++) {
        printed += llly_print(o, "%s%s", keys[i]->name, i + 1 < keys_size ? " " : "]");
    }

    if (out_str) {
        *out_str = lllydict_insert_zc(opts->module->ctx, o->method.mem.buf);
        free(o);
    }

    return printed;
}

/**
 * @brief Print schema node in YANG tree diagram formatting.
 *
 * @param[in] out libyang output.
 * @param[in] level Current level of depth.
 * @param[in] max_name_len Maximal name length of all the siblings (relevant only for nodes with type).
 * @param[in] node Schema node to print.
 * @param[in] mask Type mask of children nodes to be printed.
 * @param[in] aug_parent Augment node parent in case we are printing its direct children.
 * @param[in] opts Tree printer options structure.
 */
static void
tree_print_snode(struct lllyout *out, int level, uint16_t max_name_len, const struct lllys_node *node, int mask,
                 const struct lllys_node *aug_parent, int subtree, tp_opts *opts)
{
    struct lllys_node *sub;
    int line_len, node_len, child_mask;
    uint8_t text_len, text_indent;
    uint16_t max_child_len;
    const char *text_str;

    /* disabled/not printed node */
    if (lllys_is_disabled(node, (node->parent && node->parent->nodetype == LLLYS_AUGMENT) ? 1 : 0) || !(node->nodetype & mask)) {
        return;
    }

    /* implicit input/output/case */
    if (((node->nodetype & mask) & (LLLYS_INPUT | LLLYS_OUTPUT | LLLYS_CASE)) && (node->flags & LLLYS_IMPLICIT)) {
        if ((node->nodetype != LLLYS_CASE) || lllys_is_disabled(node->child, 0)) {
            return;
        }
    }

    /* special uses and grouping handling */
    switch (node->nodetype & mask) {
    case LLLYS_USES:
        if (opts->options & LLLYS_OUTOPT_TREE_USES) {
            break;
        }
        /* fallthrough */
    case LLLYS_GROUPING:
        goto print_children;
    case LLLYS_ANYXML:
        if (!lllys_parent(node) && !strcmp(node->name, "config") && !strcmp(node->module->name, "ietf-netconf")) {
            /* node added by libyang, not actually in the model */
            return;
        }
        break;
    default:
        break;
    }

    /* print indent */
    line_len = tree_print_indent(out, level, opts);
    /* print status */
    line_len += llly_print(out, "%s--", (node->flags & LLLYS_STATUS_DEPRC ? "x" : (node->flags & LLLYS_STATUS_OBSLT ? "o" : "+")));
    /* print config flags (or special opening for case, choice) */
    line_len += tree_print_config(out, node, opts->spec_config);
    /* print optionally prefix */
    node_len = tree_print_prefix(out, node, opts);
    /* print name */
    node_len += llly_print(out, node->name);

    /* print one-character opts */
    switch (node->nodetype & mask) {
    case LLLYS_LEAF:
        if (!(node->flags & LLLYS_MAND_TRUE) && !tree_leaf_is_mandatory(node)) {
            node_len += llly_print(out, "?");
        }
        break;
    case LLLYS_ANYDATA:
    case LLLYS_ANYXML:
        if (!(node->flags & LLLYS_MAND_TRUE)) {
            node_len += llly_print(out, "?");
        }
        break;
    case LLLYS_CONTAINER:
        if (((struct lllys_node_container *)node)->presence) {
            node_len += llly_print(out, "!");
        }
        break;
    case LLLYS_LIST:
    case LLLYS_LEAFLIST:
        node_len += llly_print(out, "*");
        break;
    case LLLYS_CASE:
        /* kinda shady, but consistent in a way */
        node_len += llly_print(out, ")");
        break;
    case LLLYS_CHOICE:
        node_len += llly_print(out, ")");
        if (!(node->flags & LLLYS_MAND_TRUE)) {
            node_len += llly_print(out, "?");
        }
        break;
    default:
        break;
    }
    line_len += node_len;

    /**
     * wrapped print
     */

    /* learn next level indent (there is never a sibling for subtree) */
    ++level;
    if (!subtree) {
        tree_next_indent(level, node, aug_parent, opts);
    }

    /* print type/keys */
    switch (node->nodetype & mask) {
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
        assert(max_name_len);
        text_indent = LLLY_TREE_TYPE_INDENT + (uint8_t)(max_name_len - node_len);
        text_len = tree_print_type(out, &((struct lllys_node_leaf *)node)->type, opts->options, &text_str);
        line_len = tree_print_wrap(out, level, line_len, text_indent, text_len, opts);
        line_len += llly_print(out, text_str);
        lllydict_remove(opts->module->ctx, text_str);
        break;
    case LLLYS_ANYDATA:
        assert(max_name_len);
        text_indent = LLLY_TREE_TYPE_INDENT + (uint8_t)(max_name_len - node_len);
        line_len = tree_print_wrap(out, level, line_len, text_indent, 7, opts);
        line_len += llly_print(out, "anydata");
        break;
    case LLLYS_ANYXML:
        assert(max_name_len);
        text_indent = LLLY_TREE_TYPE_INDENT + (uint8_t)(max_name_len - node_len);
        line_len = tree_print_wrap(out, level, line_len, text_indent, 6, opts);
        line_len += llly_print(out, "anyxml");
        break;
    case LLLYS_LIST:
        text_len = tree_print_keys(out, ((struct lllys_node_list *)node)->keys, ((struct lllys_node_list *)node)->keys_size,
                                   opts, &text_str);
        if (text_len) {
            line_len = tree_print_wrap(out, level, line_len, 1, text_len, opts);
            line_len += llly_print(out, text_str);
            lllydict_remove(opts->module->ctx, text_str);
        }
        break;
    default:
        break;
    }

    /* print default */
    if (!(opts->options & LLLYS_OUTOPT_TREE_RFC)) {
        switch (node->nodetype & mask) {
        case LLLYS_LEAF:
            text_str = ((struct lllys_node_leaf *)node)->dflt;
            if (text_str) {
                line_len = tree_print_wrap(out, level, line_len, 1, 2 + strlen(text_str), opts);
                line_len += llly_print(out, "<%s>", text_str);
            }
            break;
        case LLLYS_CHOICE:
            sub = ((struct lllys_node_choice *)node)->dflt;
            if (sub) {
                line_len = tree_print_wrap(out, level, line_len, 1, 2 + strlen(sub->name), opts);
                line_len += llly_print(out, "<%s>", sub->name);
            }
            break;
        default:
            break;
        }
    }

    /* print if-features */
    switch (node->nodetype & mask) {
    case LLLYS_CONTAINER:
    case LLLYS_LIST:
    case LLLYS_CHOICE:
    case LLLYS_CASE:
    case LLLYS_ANYDATA:
    case LLLYS_ANYXML:
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
    case LLLYS_RPC:
    case LLLYS_ACTION:
    case LLLYS_NOTIF:
    case LLLYS_USES:
        if (node->parent && (node->parent->nodetype == LLLYS_AUGMENT)) {
            /* if-features from an augment are de facto inherited */
            text_len = tree_print_features(out, node->iffeature, node->iffeature_size,
                                           node->parent->iffeature, node->parent->iffeature_size, opts, &text_str);
        } else {
            text_len = tree_print_features(out, node->iffeature, node->iffeature_size, NULL, 0, opts, &text_str);
        }
        if (text_len) {
            line_len = tree_print_wrap(out, level, line_len, 1, text_len, opts);
            line_len += llly_print(out, text_str);
            lllydict_remove(opts->module->ctx, text_str);
        }
        break;
    default:
        /* only grouping */
        break;
    }

    /* this node is finished printing */
    llly_print(out, "\n");

    if ((subtree == 1) || ((node->nodetype & mask) == LLLYS_USES)) {
        /* we are printing subtree parents, finish here (or uses option) */
        return;
    }

    /* set special config flag */
    switch (node->nodetype & mask) {
    case LLLYS_INPUT:
        opts->spec_config = 1;
        break;
    case LLLYS_OUTPUT:
    case LLLYS_NOTIF:
        opts->spec_config = 2;
        break;
    default:
        break;
    }

print_children:
    /* set child mask and learn the longest child name (needed only if a child can have type) */
    switch (node->nodetype & mask) {
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
    case LLLYS_ANYDATA:
    case LLLYS_ANYXML:
        child_mask = 0;
        max_child_len = 0;
        break;
    case LLLYS_RPC:
    case LLLYS_ACTION:
        child_mask = LLLYS_INPUT | LLLYS_OUTPUT;
        max_child_len = 0;
        break;
    case LLLYS_CHOICE:
        child_mask = LLLYS_CASE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA;
        max_child_len = tree_get_max_name_len(node->child, NULL, child_mask, opts);
        break;
    case LLLYS_CASE:
    case LLLYS_NOTIF:
        child_mask = LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA | LLLYS_USES;
        max_child_len = tree_get_max_name_len(node->child, NULL, child_mask, opts);
        break;
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
        child_mask = LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA | LLLYS_USES;
        max_child_len = tree_get_max_name_len(node->child, NULL, child_mask, opts);
        break;
    case LLLYS_USES:
        child_mask = LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA | LLLYS_USES | LLLYS_ACTION | LLLYS_NOTIF;
        /* inherit the name length from the parent, it does not change */
        max_child_len = max_name_len;
        break;
    case LLLYS_CONTAINER:
    case LLLYS_LIST:
    case LLLYS_GROUPING:
        child_mask = LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA | LLLYS_USES | LLLYS_ACTION | LLLYS_NOTIF;
        max_child_len = tree_get_max_name_len(node->child, NULL, child_mask, opts);
        break;
    default:
        child_mask = 0;
        max_child_len = 0;
        LOGINT(node->module->ctx);
        break;
    }

    /* print descendants (children) */
    if (child_mask) {
        LLLY_TREE_FOR(node->child, sub) {
            /* submodule, foreign augments */
            if (opts->module->type && (sub->parent != node) && (sub->module != opts->module)) {
                continue;
            }
            tree_print_snode(out, level, max_child_len, sub, child_mask, NULL, 0, opts);
        }
    }

    /* reset special config flag */
    switch (node->nodetype & mask) {
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
    case LLLYS_NOTIF:
        opts->spec_config = 0;
        break;
    default:
        break;
    }
}

static void
tree_print_subtree(struct lllyout *out, const struct lllys_node *node, tp_opts *opts)
{
    unsigned int depth, i, j;
    int level = 0;
    uint16_t max_child_len;
    const struct lllys_node *parent;

    /* learn the depth of the node */
    depth = 0;
    parent = node;
    while (lllys_parent(parent)) {
        if (lllys_parent(parent)->nodetype != LLLYS_USES) {
            ++depth;
        }
        parent = lllys_parent(parent);
    }

    if (parent->nodetype == LLLYS_RPC) {
        llly_print(out, "\n%*srpcs:\n", LLLY_TREE_MOD_DATA_INDENT, "");
        opts->base_indent = LLLY_TREE_OP_DATA_INDENT;
    } else if (parent->nodetype == LLLYS_NOTIF) {
        llly_print(out, "\n%*snotifications:\n", LLLY_TREE_MOD_DATA_INDENT, "");
        opts->base_indent = LLLY_TREE_OP_DATA_INDENT;
    }

    /* print all the parents */
    if (depth) {
        i = depth;
        do {
            parent = node;
            for (j = 0; j < i; ++j) {
                do {
                    parent = lllys_parent(parent);
                } while (parent->nodetype == LLLYS_USES);
            }

            tree_print_snode(out, level, 0, parent, LLLYS_CONTAINER | LLLYS_LIST | LLLYS_NOTIF | LLLYS_RPC | LLLYS_ACTION
                                                    | LLLYS_INPUT | LLLYS_OUTPUT, NULL, 1, opts);

            ++level;
            --i;
        } while (i);
    }

    /* print the node and its descendants */
    max_child_len = tree_get_max_name_len(node, NULL, LLLYS_LEAF|LLLYS_LEAFLIST|LLLYS_ANYDATA, opts);
    tree_print_snode(out, level, max_child_len, node, LLLYS_ANY, NULL, 2, opts);
}

static int
tree_print_aug_target(struct lllyout *out, int line_printed, uint8_t indent, const char *path, tp_opts *opts)
{
    int printed, is_last, len;
    const char *cur, *next;

    printed = line_printed;
    cur = path;
    do {
        next = strchr(cur + 1, '/');
        if (!next) {
            len = strlen(cur) + 1;
            is_last = 1;
        } else {
            len = next - cur;
            is_last = 0;
        }

        if (opts->line_length && cur != path && (printed + len > opts->line_length)) {
            /* line_printed is treated as the base indent */
            printed = llly_print(out, "\n%*s", line_printed + indent, "");
            /* minus the newline */
            --printed;
        }
        printed += llly_print(out, "%.*s%s", len, cur, is_last ? ":" : "");

        cur = next;
    } while (!is_last);

    return printed;
}

int
tree_print_model(struct lllyout *out, const struct lllys_module *module, const char *target_schema_path,
                 int ll, int options)
{
    struct lllys_node *node = NULL, *data, *aug;
    struct llly_set *set;
    uint16_t max_child_len;
    int have_rpcs = 0, have_notifs = 0, have_grps = 0, have_augs = 0, printed;
    const char *str;
    int i, mask;
    tp_opts opts;

    memset(&opts, 0, sizeof opts);
    opts.module = module;
    opts.line_length = ll;
    opts.options = options;

    /* we are printing only a subtree */
    if (target_schema_path) {
        set = lllys_find_path(module, NULL, target_schema_path);
        if (!set) {
            return EXIT_FAILURE;
        } else if (set->number != 1) {
            LOGVAL(module->ctx, LLLYE_PATH_INNODE, LLLY_VLOG_NONE, NULL);
            if (set->number == 0) {
                LOGVAL(module->ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "Schema path \"%s\" did not match any nodes.", target_schema_path);
            } else {
                LOGVAL(module->ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "Schema path \"%s\" matched more nodes.", target_schema_path);
            }
            llly_set_free(set);
            return EXIT_FAILURE;
        }

        node = set->set.s[0];
        llly_set_free(set);
    }

    if (module->type) {
        llly_print(out, "submodule: %s", module->name);
        data = ((struct lllys_submodule *)module)->belongsto->data;
        if (options & LLLYS_OUTOPT_TREE_RFC) {
            llly_print(out, "\n");
        } else {
            llly_print(out, " (belongs-to %s)\n", ((struct lllys_submodule *)module)->belongsto->name);
        }
    } else {
        llly_print(out, "module: %s\n", module->name);
        data = module->data;
    }

    /* only subtree */
    if (target_schema_path) {
        opts.base_indent = LLLY_TREE_MOD_DATA_INDENT;
        tree_print_subtree(out, node, &opts);
        return EXIT_SUCCESS;
    }

    /* module */
    opts.base_indent = LLLY_TREE_MOD_DATA_INDENT;
    mask = LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA | LLLYS_USES;
    max_child_len = tree_get_max_name_len(data, NULL, mask, &opts);
    LLLY_TREE_FOR(data, node) {
        if (opts.module->type && (node->module != opts.module)) {
            /* we're printing the submodule only */
            continue;
        }

        switch (node->nodetype) {
        case LLLYS_RPC:
            if (!lllys_is_disabled(node, 0)) {
                have_rpcs++;
            }
            break;
        case LLLYS_NOTIF:
            if (!lllys_is_disabled(node, 0)) {
                have_notifs++;
            }
            break;
        case LLLYS_GROUPING:
            if ((options & LLLYS_OUTOPT_TREE_GROUPING) && !lllys_is_disabled(node, 0)) {
                have_grps++;
            }
            break;
        default:
            tree_print_snode(out, 0, max_child_len, node, mask, NULL, 0, &opts);
            break;
        }
    }

    /* all remaining nodes printed with operation indent */
    opts.base_indent = LLLY_TREE_OP_DATA_INDENT;

    /* augments */
    for (i = 0; i < module->augment_size; i++) {
        if ((module->type && (module->augment[i].target->module == module))
                || (!module->type && (lllys_node_module(module->augment[i].target) == module))
                || lllys_is_disabled((struct lllys_node *)&module->augment[i], 0)) {
            /* submodule, target is our submodule or module, target is in our module or any submodules */
            continue;
        }

        if (!have_augs) {
            llly_print(out, "\n");
            have_augs = 1;
        }

        printed = llly_print(out, "%*saugment ", LLLY_TREE_MOD_DATA_INDENT, "");
        if (options & LLLYS_OUTOPT_TREE_RFC) {
            str = transform_json2schema(module, module->augment[i].target_name);
            tree_print_aug_target(out, printed, LLLY_TREE_WRAP_INDENT, str, &opts);
            lllydict_remove(module->ctx, str);
        } else {
            tree_print_aug_target(out, printed, LLLY_TREE_WRAP_INDENT, module->augment[i].target_name, &opts);
        }
        llly_print(out, "\n");

        aug = (struct lllys_node *)&module->augment[i];
        mask = LLLYS_CHOICE | LLLYS_CASE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA | LLLYS_USES
               | LLLYS_ACTION | LLLYS_NOTIF;
        max_child_len = tree_get_max_name_len(aug->child, aug, mask, &opts);
        LLLY_TREE_FOR(aug->child, node) {
            /* submodule, foreign augments */
            if (node->parent != aug) {
                continue;
            }
            tree_print_snode(out, 0, max_child_len, node, mask, aug, 0, &opts);
        }
    }

    /* rpcs */
    if (have_rpcs) {
        llly_print(out, "\n%*srpcs:\n", LLLY_TREE_MOD_DATA_INDENT, "");

        LLLY_TREE_FOR(data, node) {
            tree_print_snode(out, 0, 0, node, LLLYS_RPC, NULL, 0, &opts);
        }
    }

    /* notifications */
    if (have_notifs) {
        llly_print(out, "\n%*snotifications:\n", LLLY_TREE_MOD_DATA_INDENT, "");

        LLLY_TREE_FOR(data, node) {
            tree_print_snode(out, 0, 0, node, LLLYS_NOTIF, NULL, 0, &opts);
        }
    }

    /* groupings */
    if ((options & LLLYS_OUTOPT_TREE_GROUPING) && have_grps) {
        llly_print(out, "\n");
        LLLY_TREE_FOR(data, node) {
            if (node->nodetype == LLLYS_GROUPING) {
                llly_print(out, "%*sgrouping %s:\n", LLLY_TREE_MOD_DATA_INDENT, "", node->name);

                tree_print_snode(out, 0, 0, node, LLLYS_GROUPING, NULL, 0, &opts);
            }
        }
    }

    llly_print_flush(out);

    return EXIT_SUCCESS;
}
