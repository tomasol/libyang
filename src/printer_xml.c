/**
 * @file printer_xml.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief XML printer for libyang data structure
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

#include "common.h"
#include "parser.h"
#include "printer.h"
#include "xml_internal.h"
#include "tree_data.h"
#include "tree_schema.h"
#include "resolve.h"
#include "tree_internal.h"

#define INDENT ""
#define LEVEL (level ? level*2-2 : 0)

struct mlist {
    struct mlist *next;
    struct lllys_module *module;
    uint8_t printed;
};

static int
modlist_add(struct mlist **mlist, const struct lllys_module *mod)
{
    struct mlist *iter;

    for (iter = *mlist; iter; iter = iter->next) {
        if (mod == iter->module) {
            break;
        }
    }

    if (!iter) {
        iter = malloc(sizeof *iter);
        LLLY_CHECK_ERR_RETURN(!iter, LOGMEM(mod->ctx), EXIT_FAILURE);
        iter->next = *mlist;
        iter->module = (struct lllys_module *)mod;
        iter->printed = 0;
        *mlist = iter;
    }

    return EXIT_SUCCESS;
}

static void
free_mlist(struct mlist **mlist) {
    struct mlist *miter;
    while (*mlist) {
        miter = *mlist;
        *mlist = miter->next;
        free(miter);
    }
}

static void
xml_print_ns(struct lllyout *out, const struct lllyd_node *node, struct mlist **mlist, int options)
{
    struct lllyd_node *next, *cur, *node2;
    struct lllyd_attr *attr;
    const struct lllys_module *wdmod = NULL;
    struct mlist *miter;
    int r;

    assert(out);
    assert(node);

    /* add node attribute modules */
    for (attr = node->attr; attr; attr = attr->next) {
        if (!strcmp(node->schema->name, "filter") &&
                (!strcmp(node->schema->module->name, "ietf-netconf") ||
                 !strcmp(node->schema->module->name, "notifications"))) {
            /* exception for NETCONF's filter attributes */
            continue;
        } else {
            r = modlist_add(mlist, lllys_main_module(attr->annotation->module));
        }
        if (r) {
            goto print;
        }
    }

    /* add node children nodes and attribute modules */
    switch (node->schema->nodetype) {
    case LLLYS_LEAFLIST:
    case LLLYS_LEAF:
        if (node->dflt && (options & (LLLYP_WD_ALL_TAG | LLLYP_WD_IMPL_TAG))) {
            /* get with-defaults module and print its namespace */
            wdmod = llly_ctx_get_module(node->schema->module->ctx, "ietf-netconf-with-defaults", NULL, 1);
            if (wdmod && modlist_add(mlist, wdmod)) {
                goto print;
            }
        }
        break;
    case LLLYS_CONTAINER:
    case LLLYS_LIST:
    case LLLYS_RPC:
    case LLLYS_ACTION:
    case LLLYS_NOTIF:
        if (options & (LLLYP_WD_ALL_TAG | LLLYP_WD_IMPL_TAG)) {
            /* get with-defaults module and print its namespace */
            wdmod = llly_ctx_get_module(node->schema->module->ctx, "ietf-netconf-with-defaults", NULL, 1);
            if (wdmod && modlist_add(mlist, wdmod)) {
                goto print;
            }
        }

        LLLY_TREE_FOR(node->child, node2) {
            LLLY_TREE_DFS_BEGIN(node2, next, cur) {
                for (attr = cur->attr; attr; attr = attr->next) {
                    if (!strcmp(cur->schema->name, "filter") &&
                            (!strcmp(cur->schema->module->name, "ietf-netconf") ||
                             !strcmp(cur->schema->module->name, "notifications"))) {
                        /* exception for NETCONF's filter attributes */
                        continue;
                    } else {
                        r = modlist_add(mlist, lllys_main_module(attr->annotation->module));
                    }
                    if (r) {
                        goto print;
                    }
                }
            LLLY_TREE_DFS_END(node2, next, cur)}
        }
        break;
    default:
        break;
    }

print:
    /* print used namespaces */
    miter = *mlist;
    while (miter) {

        if (!miter->printed) {
            llly_print(out, " xmlns:%s=\"%s\"", miter->module->prefix, miter->module->ns);
            miter->printed = 1;
        }
        miter = miter->next;
    }
}

static int
xml_print_attrs(struct lllyout *out, const struct lllyd_node *node, int options)
{
    struct lllyd_attr *attr;
    const char **prefs, **nss;
    const char *xml_expr = NULL, *mod_name;
    uint32_t ns_count, i;
    int rpc_filter = 0;
    const struct lllys_module *wdmod = NULL;
    char *p;
    size_t len;

    LLLY_PRINT_SET;

    /* with-defaults */
    if (node->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) {
        if ((node->dflt && (options & (LLLYP_WD_ALL_TAG | LLLYP_WD_IMPL_TAG))) ||
                (!node->dflt && (options & LLLYP_WD_ALL_TAG) && lllyd_wd_default((struct lllyd_node_leaf_list *)node))) {
            /* we have implicit OR explicit default node */
            /* get with-defaults module */
            wdmod = llly_ctx_get_module(node->schema->module->ctx, "ietf-netconf-with-defaults", NULL, 1);
            if (wdmod) {
                /* print attribute only if context include with-defaults schema */
                llly_print(out, " %s:default=\"true\"", wdmod->prefix);
            }
        }
    }
    /* technically, check for the extension get-filter-element-attributes from ietf-netconf */
    if (!strcmp(node->schema->name, "filter")
            && (!strcmp(node->schema->module->name, "ietf-netconf") || !strcmp(node->schema->module->name, "notifications"))) {
        rpc_filter = 1;
    }

    for (attr = node->attr; attr; attr = attr->next) {
        if (rpc_filter) {
            /* exception for NETCONF's filter's attributes */
            if (!strcmp(attr->name, "select")) {
                /* xpath content, we have to convert the JSON format into XML first */
                xml_expr = transform_json2xml(node->schema->module, attr->value_str, 0, &prefs, &nss, &ns_count);
                if (!xml_expr) {
                    /* error */
                    return EXIT_FAILURE;
                }

                for (i = 0; i < ns_count; ++i) {
                    llly_print(out, " xmlns:%s=\"%s\"", prefs[i], nss[i]);
                }
                free(prefs);
                free(nss);
            }
            llly_print(out, " %s=\"", attr->name);
        } else {
            llly_print(out, " %s:%s=\"", attr->annotation->module->prefix, attr->name);
        }

        switch (attr->value_type) {
        case LLLY_TYPE_BINARY:
        case LLLY_TYPE_STRING:
        case LLLY_TYPE_BITS:
        case LLLY_TYPE_ENUM:
        case LLLY_TYPE_BOOL:
        case LLLY_TYPE_DEC64:
        case LLLY_TYPE_INT8:
        case LLLY_TYPE_INT16:
        case LLLY_TYPE_INT32:
        case LLLY_TYPE_INT64:
        case LLLY_TYPE_UINT8:
        case LLLY_TYPE_UINT16:
        case LLLY_TYPE_UINT32:
        case LLLY_TYPE_UINT64:
            if (attr->value_str) {
                /* xml_expr can contain transformed xpath */
                lllyxml_dump_text(out, xml_expr ? xml_expr : attr->value_str, LLLYXML_DATA_ATTR);
            }
            break;

        case LLLY_TYPE_IDENT:
            if (!attr->value_str) {
                break;
            }
            p = strchr(attr->value_str, ':');
            assert(p);
            len = p - attr->value_str;
            mod_name = attr->annotation->module->name;
            if (!strncmp(attr->value_str, mod_name, len) && !mod_name[len]) {
                lllyxml_dump_text(out, ++p, LLLYXML_DATA_ATTR);
            } else {
                /* avoid code duplication - use instance-identifier printer which gets necessary namespaces to print */
                goto printinst;
            }
            break;
        case LLLY_TYPE_INST:
printinst:
            xml_expr = transform_json2xml(node->schema->module, ((struct lllyd_node_leaf_list *)node)->value_str, 1,
                                          &prefs, &nss, &ns_count);
            if (!xml_expr) {
                /* error */
                return EXIT_FAILURE;
            }

            for (i = 0; i < ns_count; ++i) {
                llly_print(out, " xmlns:%s=\"%s\"", prefs[i], nss[i]);
            }
            free(prefs);
            free(nss);

            lllyxml_dump_text(out, xml_expr, LLLYXML_DATA_ATTR);
            lllydict_remove(node->schema->module->ctx, xml_expr);
            break;

        /* LLLY_TYPE_LEAFREF not allowed */
        case LLLY_TYPE_EMPTY:
            break;

        default:
            /* error */
            LOGINT(node->schema->module->ctx);
            return EXIT_FAILURE;
        }

        llly_print(out, "\"");

        if (xml_expr) {
            lllydict_remove(node->schema->module->ctx, xml_expr);
        }
    }

    LLLY_PRINT_RET(node->schema->module->ctx);
}

static int
xml_print_leaf(struct lllyout *out, int level, const struct lllyd_node *node, int toplevel, int options)
{
    const struct lllyd_node_leaf_list *leaf = (struct lllyd_node_leaf_list *)node, *iter;
    const struct lllys_type *type;
    struct lllys_tpdf *tpdf;
    const char *ns, *mod_name;
    const char **prefs, **nss;
    const char *xml_expr;
    uint32_t ns_count, i;
    LLLY_DATA_TYPE datatype;
    char *p;
    size_t len;
    enum int_log_opts prev_ilo;
    struct mlist *mlist = NULL;

    LLLY_PRINT_SET;

    if (toplevel || !node->parent || nscmp(node, node->parent)) {
        /* print "namespace" */
        ns = lllyd_node_module(node)->ns;
        llly_print(out, "%*s<%s xmlns=\"%s\"", LEVEL, INDENT, node->schema->name, ns);
    } else {
        llly_print(out, "%*s<%s", LEVEL, INDENT, node->schema->name);
    }

    if (toplevel) {
        xml_print_ns(out, node, &mlist, options);
        free_mlist(&mlist);
    }

    if (xml_print_attrs(out, node, options)) {
        return EXIT_FAILURE;
    }
    datatype = leaf->value_type;

printvalue:
    switch (datatype) {
    case LLLY_TYPE_STRING:
        llly_ilo_change(NULL, ILO_IGNORE, &prev_ilo, NULL);
        type = lllyd_leaf_type((struct lllyd_node_leaf_list *)leaf);
        llly_ilo_restore(NULL, prev_ilo, NULL, 0);
        if (type) {
            for (tpdf = type->der;
                tpdf->module && (strcmp(tpdf->name, "xpath1.0") || strcmp(tpdf->module->name, "ietf-yang-types"));
                tpdf = tpdf->type.der);
            /* special handling of ietf-yang-types xpath1.0 */
            if (tpdf->module) {
                /* avoid code duplication - use instance-identifier printer which gets necessary namespaces to print */
                datatype = LLLY_TYPE_INST;
                goto printvalue;
            }
        }
        /* fallthrough */
    case LLLY_TYPE_BINARY:
    case LLLY_TYPE_BITS:
    case LLLY_TYPE_ENUM:
    case LLLY_TYPE_BOOL:
    case LLLY_TYPE_UNION:
    case LLLY_TYPE_DEC64:
    case LLLY_TYPE_INT8:
    case LLLY_TYPE_INT16:
    case LLLY_TYPE_INT32:
    case LLLY_TYPE_INT64:
    case LLLY_TYPE_UINT8:
    case LLLY_TYPE_UINT16:
    case LLLY_TYPE_UINT32:
    case LLLY_TYPE_UINT64:
        if (!leaf->value_str || !leaf->value_str[0]) {
            llly_print(out, "/>");
        } else {
            llly_print(out, ">");
            lllyxml_dump_text(out, leaf->value_str, LLLYXML_DATA_ELEM);
            llly_print(out, "</%s>", node->schema->name);
        }
        break;

    case LLLY_TYPE_IDENT:
        if (!leaf->value_str || !leaf->value_str[0]) {
            llly_print(out, "/>");
            break;
        }
        p = strchr(leaf->value_str, ':');
        assert(p);
        len = p - leaf->value_str;
        mod_name = leaf->schema->module->name;
        if (!strncmp(leaf->value_str, mod_name, len) && !mod_name[len]) {
            llly_print(out, ">");
            lllyxml_dump_text(out, ++p, LLLYXML_DATA_ELEM);
            llly_print(out, "</%s>", node->schema->name);
        } else {
            /* avoid code duplication - use instance-identifier printer which gets necessary namespaces to print */
            datatype = LLLY_TYPE_INST;
            goto printvalue;
        }
        break;
    case LLLY_TYPE_INST:
        xml_expr = transform_json2xml(node->schema->module, ((struct lllyd_node_leaf_list *)node)->value_str, 1,
                                      &prefs, &nss, &ns_count);
        if (!xml_expr) {
            /* error */
            return EXIT_FAILURE;
        }

        for (i = 0; i < ns_count; ++i) {
            llly_print(out, " xmlns:%s=\"%s\"", prefs[i], nss[i]);
        }
        free(prefs);
        free(nss);

        if (xml_expr[0]) {
            llly_print(out, ">");
            lllyxml_dump_text(out, xml_expr, LLLYXML_DATA_ELEM);
            llly_print(out, "</%s>", node->schema->name);
        } else {
            llly_print(out, "/>");
        }
        lllydict_remove(node->schema->module->ctx, xml_expr);
        break;

    case LLLY_TYPE_LEAFREF:
        iter = (struct lllyd_node_leaf_list *)leaf->value.leafref;
        while (iter && (iter->value_type == LLLY_TYPE_LEAFREF)) {
            iter = (struct lllyd_node_leaf_list *)iter->value.leafref;
        }
        if (!iter) {
            /* unresolved and invalid, but we can learn the correct type anyway */
            type = lllyd_leaf_type((struct lllyd_node_leaf_list *)leaf);
            if (!type) {
                /* error */
                return EXIT_FAILURE;
            }
            datatype = type->base;
        } else {
            datatype = iter->value_type;
        }
        goto printvalue;

    case LLLY_TYPE_EMPTY:
    case LLLY_TYPE_UNKNOWN:
        /* treat <edit-config> node without value as empty */
        llly_print(out, "/>");
        break;

    default:
        /* error */
        LOGINT(node->schema->module->ctx);
        return EXIT_FAILURE;
    }

    if (level) {
        llly_print(out, "\n");
    }

    LLLY_PRINT_RET(node->schema->module->ctx);
}

static int
xml_print_container(struct lllyout *out, int level, const struct lllyd_node *node, int toplevel, int options)
{
    struct lllyd_node *child;
    const char *ns;
    struct mlist *mlist = NULL;

    LLLY_PRINT_SET;

    if (toplevel || !node->parent || nscmp(node, node->parent)) {
        /* print "namespace" */
        ns = lllyd_node_module(node)->ns;
        llly_print(out, "%*s<%s xmlns=\"%s\"", LEVEL, INDENT, node->schema->name, ns);
    } else {
        llly_print(out, "%*s<%s", LEVEL, INDENT, node->schema->name);
    }

    if (toplevel) {
        xml_print_ns(out, node, &mlist, options);
        free_mlist(&mlist);
    }

    if (xml_print_attrs(out, node, options)) {
        return EXIT_FAILURE;
    }

    if (!node->child) {
        llly_print(out, "/>%s", level ? "\n" : "");
        goto finish;
    }
    llly_print(out, ">%s", level ? "\n" : "");

    LLLY_TREE_FOR(node->child, child) {
        if (xml_print_node(out, level ? level + 1 : 0, child, 0, options)) {
            return EXIT_FAILURE;
        }
    }

    llly_print(out, "%*s</%s>%s", LEVEL, INDENT, node->schema->name, level ? "\n" : "");

finish:
    LLLY_PRINT_RET(node->schema->module->ctx);
}

static int
xml_print_list(struct lllyout *out, int level, const struct lllyd_node *node, int is_list, int toplevel, int options)
{
    struct lllyd_node *child;
    const char *ns;
    struct mlist *mlist = NULL;

    LLLY_PRINT_SET;

    if (is_list) {
        /* list print */
        if (toplevel || !node->parent || nscmp(node, node->parent)) {
            /* print "namespace" */
            ns = lllyd_node_module(node)->ns;
            llly_print(out, "%*s<%s xmlns=\"%s\"", LEVEL, INDENT, node->schema->name, ns);
        } else {
            llly_print(out, "%*s<%s", LEVEL, INDENT, node->schema->name);
        }

        if (toplevel) {
            xml_print_ns(out, node, &mlist, options);
            free_mlist(&mlist);
        }
        if (xml_print_attrs(out, node, options)) {
            return EXIT_FAILURE;
        }

        if (!node->child) {
            llly_print(out, "/>%s", level ? "\n" : "");
            goto finish;
        }
        llly_print(out, ">%s", level ? "\n" : "");

        LLLY_TREE_FOR(node->child, child) {
            if (xml_print_node(out, level ? level + 1 : 0, child, 0, options)) {
                return EXIT_FAILURE;
            }
        }

        llly_print(out, "%*s</%s>%s", LEVEL, INDENT, node->schema->name, level ? "\n" : "");
    } else {
        /* leaf-list print */
        xml_print_leaf(out, level, node, toplevel, options);
    }

finish:
    LLLY_PRINT_RET(node->schema->module->ctx);
}

static int
xml_print_anydata(struct lllyout *out, int level, const struct lllyd_node *node, int toplevel, int options)
{
    char *buf;
    struct lllyd_node_anydata *any = (struct lllyd_node_anydata *)node;
    struct lllyd_node *iter;
    const char *ns;
    struct mlist *mlist = NULL;

    LLLY_PRINT_SET;

    if (toplevel || !node->parent || nscmp(node, node->parent)) {
        /* print "namespace" */
        ns = lllyd_node_module(node)->ns;
        llly_print(out, "%*s<%s xmlns=\"%s\"", LEVEL, INDENT, node->schema->name, ns);
    } else {
        llly_print(out, "%*s<%s", LEVEL, INDENT, node->schema->name);
    }

    if (toplevel) {
        xml_print_ns(out, node, &mlist, options);
    }
    if (xml_print_attrs(out, node, options)) {
        return EXIT_FAILURE;
    }
    if (!(void*)any->value.tree || (any->value_type == LLLYD_ANYDATA_CONSTSTRING && !any->value.str[0])) {
        /* no content */
        llly_print(out, "/>%s", level ? "\n" : "");
    } else {
        if (any->value_type == LLLYD_ANYDATA_LYB) {
            /* parse into a data tree */
            iter = lllyd_parse_mem(node->schema->module->ctx, any->value.mem, LLLYD_LYB, LLLYD_OPT_DATA | LLLYD_OPT_STRICT
                                 | LLLYD_OPT_TRUSTED);
            if (iter) {
                /* successfully parsed */
                free(any->value.mem);
                any->value_type = LLLYD_ANYDATA_DATATREE;
                any->value.tree = iter;
            }
        }
        if (any->value_type == LLLYD_ANYDATA_DATATREE) {
            /* print namespaces in the anydata data tree */
            LLLY_TREE_FOR(any->value.tree, iter) {
                xml_print_ns(out, iter, &mlist, options);
            }
        }
        /* close opening tag ... */
        llly_print(out, ">");
        free_mlist(&mlist);
        /* ... and print anydata content */
        switch (any->value_type) {
        case LLLYD_ANYDATA_CONSTSTRING:
            lllyxml_dump_text(out, any->value.str, LLLYXML_DATA_ELEM);
            break;
        case LLLYD_ANYDATA_DATATREE:
            if (any->value.tree) {
                if (level) {
                    llly_print(out, "\n");
                }
                LLLY_TREE_FOR(any->value.tree, iter) {
                    if (xml_print_node(out, level ? level + 1 : 0, iter, 0, (options & ~(LLLYP_WITHSIBLINGS | LLLYP_NETCONF)))) {
                        return EXIT_FAILURE;
                    }
                }
            }
            break;
        case LLLYD_ANYDATA_XML:
            lllyxml_print_mem(&buf, any->value.xml, (level ? LLLYXML_PRINT_FORMAT | LLLYXML_PRINT_NO_LAST_NEWLINE : 0)
                                                   | LLLYXML_PRINT_SIBLINGS);
            llly_print(out, "%s%s", level ? "\n" : "", buf);
            free(buf);
            break;
        case LLLYD_ANYDATA_SXML:
            /* print without escaping special characters */
            llly_print(out, "%s", any->value.str);
            break;
        case LLLYD_ANYDATA_JSON:
        case LLLYD_ANYDATA_LYB:
            /* JSON format is not supported (LLLYB failed to be converted) */
            LOGWRN(node->schema->module->ctx, "Unable to print anydata content (type %d) as XML.", any->value_type);
            break;
        case LLLYD_ANYDATA_STRING:
        case LLLYD_ANYDATA_SXMLD:
        case LLLYD_ANYDATA_JSOND:
        case LLLYD_ANYDATA_LYBD:
            /* dynamic strings are used only as input parameters */
            assert(0);
            break;
        }

        /* closing tag */
        llly_print(out, "</%s>%s", node->schema->name, level ? "\n" : "");
    }

    LLLY_PRINT_RET(node->schema->module->ctx);
}

int
xml_print_node(struct lllyout *out, int level, const struct lllyd_node *node, int toplevel, int options)
{
    int ret = EXIT_SUCCESS;

    if (!lllyd_toprint(node, options)) {
        /* wd says do not print */
        return EXIT_SUCCESS;
    }

    switch (node->schema->nodetype) {
    case LLLYS_NOTIF:
    case LLLYS_RPC:
    case LLLYS_ACTION:
    case LLLYS_CONTAINER:
        ret = xml_print_container(out, level, node, toplevel, options);
        break;
    case LLLYS_LEAF:
        ret = xml_print_leaf(out, level, node, toplevel, options);
        break;
    case LLLYS_LEAFLIST:
        ret = xml_print_list(out, level, node, 0, toplevel, options);
        break;
    case LLLYS_LIST:
        ret = xml_print_list(out, level, node, 1, toplevel, options);
        break;
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        ret = xml_print_anydata(out, level, node, toplevel, options);
        break;
    default:
        LOGINT(node->schema->module->ctx);
        ret = EXIT_FAILURE;
        break;
    }

    return ret;
}

int
xml_print_data(struct lllyout *out, const struct lllyd_node *root, int options)
{
    const struct lllyd_node *node, *next;
    struct lllys_node *parent = NULL;
    int level, action_input = 0;

    LLLY_PRINT_SET;

    if (!root) {
        if (out->type == LLLYOUT_MEMORY || out->type == LLLYOUT_CALLBACK) {
            llly_print(out, "");
        }
        goto finish;
    }

    level = (options & LLLYP_FORMAT ? 1 : 0);

    if (options & LLLYP_NETCONF) {
        if (root->schema->nodetype != LLLYS_RPC) {
            /* learn whether we are printing an action */
            LLLY_TREE_DFS_BEGIN(root, next, node) {
                if (node->schema->nodetype == LLLYS_ACTION) {
                    break;
                }
                LLLY_TREE_DFS_END(root, next, node);
            }
        } else {
            node = root;
        }

        if (node) {
            if ((node->schema->nodetype & (LLLYS_LIST | LLLYS_CONTAINER | LLLYS_RPC | LLLYS_NOTIF | LLLYS_ACTION)) && node->child) {
                for (parent = lllys_parent(node->child->schema); parent && (parent->nodetype == LLLYS_USES); parent = lllys_parent(parent));
            }
            if (parent && (parent->nodetype == LLLYS_OUTPUT)) {
                /* rpc/action output - skip the container */
                root = node->child;
            } else if (node->schema->nodetype == LLLYS_ACTION) {
                /* action input - print top-level action element */
                action_input = 1;
            }
        }
    }

    if (action_input) {
        llly_print(out, "%*s<action xmlns=\"%s\">%s", LEVEL, INDENT, LLLY_NSYANG, level ? "\n" : "");
        if (level) {
            ++level;
        }
    }

    /* content */
    LLLY_TREE_FOR(root, node) {
        if (xml_print_node(out, level, node, 1, options)) {
            return EXIT_FAILURE;
        }
        if (!(options & LLLYP_WITHSIBLINGS)) {
            break;
        }
    }

    if (action_input) {
        if (level) {
            --level;
        }
        llly_print(out, "%*s</action>%s", LEVEL, INDENT, level ? "\n" : "");
    }

finish:
    llly_print_flush(out);

    LLLY_PRINT_RET(NULL);
}

