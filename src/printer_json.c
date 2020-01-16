/**
 * @file printer/json.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief JSON printer for libyang data structure
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

#include "common.h"
#include "printer.h"
#include "tree_data.h"
#include "resolve.h"
#include "tree_internal.h"

#define INDENT ""
#define LEVEL (level*2)

static int json_print_nodes(struct lllyout *out, int level, const struct lllyd_node *root, int withsiblings, int toplevel,
                            int options);

int
json_print_string(struct lllyout *out, const char *text)
{
    unsigned int i, n;

    if (!text) {
        return 0;
    }

    llly_write(out, "\"", 1);
    for (i = n = 0; text[i]; i++) {
        const unsigned char ascii = text[i];
        if (ascii < 0x20) {
            /* control character */
            n += llly_print(out, "\\u%.4X", ascii);
        } else {
            switch (ascii) {
            case '"':
                n += llly_print(out, "\\\"");
                break;
            case '\\':
                n += llly_print(out, "\\\\");
                break;
            default:
                llly_write(out, &text[i], 1);
                n++;
            }
        }
    }
    llly_write(out, "\"", 1);

    return n + 2;
}

static int
json_print_attrs(struct lllyout *out, int level, const struct lllyd_node *node, const struct lllys_module *wdmod)
{
    struct lllyd_attr *attr;
    size_t len;
    char *p;

    LLLY_PRINT_SET;

    if (wdmod) {
        llly_print(out, "%*s\"%s:default\":\"true\"", LEVEL, INDENT, wdmod->name);
        llly_print(out, "%s%s", node->attr ? "," : "", (level ? "\n" : ""));
    }
    for (attr = node->attr; attr; attr = attr->next) {
        if (!attr->annotation) {
            /* skip exception for the NETCONF's attribute since JSON is not defined for NETCONF */
            continue;
        }
        if (lllys_main_module(attr->annotation->module) != lllys_main_module(node->schema->module)) {
            llly_print(out, "%*s\"%s:%s\":", LEVEL, INDENT, attr->annotation->module->name, attr->name);
        } else {
            llly_print(out, "%*s\"%s\":", LEVEL, INDENT, attr->name);
        }
        /* leafref is not supported */
        switch (attr->value_type) {
        case LLLY_TYPE_BINARY:
        case LLLY_TYPE_STRING:
        case LLLY_TYPE_BITS:
        case LLLY_TYPE_ENUM:
        case LLLY_TYPE_INST:
        case LLLY_TYPE_INT64:
        case LLLY_TYPE_UINT64:
        case LLLY_TYPE_DEC64:
            json_print_string(out, attr->value_str);
            break;

        case LLLY_TYPE_INT8:
        case LLLY_TYPE_INT16:
        case LLLY_TYPE_INT32:
        case LLLY_TYPE_UINT8:
        case LLLY_TYPE_UINT16:
        case LLLY_TYPE_UINT32:
        case LLLY_TYPE_BOOL:
            llly_print(out, "%s", attr->value_str[0] ? attr->value_str : "null");
            break;

        case LLLY_TYPE_IDENT:
            p = strchr(attr->value_str, ':');
            assert(p);
            len = p - attr->value_str;
            if (!strncmp(attr->value_str, attr->annotation->module->name, len)
                    && !attr->annotation->module->name[len]) {
                /* do not print the prefix, it is the default prefix for this node */
                json_print_string(out, ++p);
            } else {
                json_print_string(out, attr->value_str);
            }
            break;

        case LLLY_TYPE_EMPTY:
            llly_print(out, "[null]");
            break;

        default:
            /* error */
            LOGINT(node->schema->module->ctx);
            return EXIT_FAILURE;
        }

        llly_print(out, "%s%s", attr->next ? "," : "", (level ? "\n" : ""));
    }

    LLLY_PRINT_RET(node->schema->module->ctx);
}

static int
json_print_leaf(struct lllyout *out, int level, const struct lllyd_node *node, int onlyvalue, int toplevel, int options)
{
    struct lllyd_node_leaf_list *leaf = (struct lllyd_node_leaf_list *)node, *iter;
    const struct lllys_type *type;
    const char *schema = NULL, *p, *mod_name;
    const struct lllys_module *wdmod = NULL;
    LLLY_DATA_TYPE datatype;
    size_t len;

    LLLY_PRINT_SET;

    if ((node->dflt && (options & (LLLYP_WD_ALL_TAG | LLLYP_WD_IMPL_TAG))) ||
            (!node->dflt && (options & LLLYP_WD_ALL_TAG) && lllyd_wd_default(leaf))) {
        /* we have implicit OR explicit default node */
        /* get with-defaults module */
        wdmod = llly_ctx_get_module(node->schema->module->ctx, "ietf-netconf-with-defaults", NULL, 1);
    }

    if (!onlyvalue) {
        if (toplevel || !node->parent || nscmp(node, node->parent)) {
            /* print "namespace" */
            schema = lllys_node_module(node->schema)->name;
            llly_print(out, "%*s\"%s:%s\":%s", LEVEL, INDENT, schema, node->schema->name, (level ? " " : ""));
        } else {
            llly_print(out, "%*s\"%s\":%s", LEVEL, INDENT, node->schema->name, (level ? " " : ""));
        }
    }

    datatype = leaf->value_type;
contentprint:
    switch (datatype) {
    case LLLY_TYPE_BINARY:
    case LLLY_TYPE_STRING:
    case LLLY_TYPE_BITS:
    case LLLY_TYPE_ENUM:
    case LLLY_TYPE_INST:
    case LLLY_TYPE_INT64:
    case LLLY_TYPE_UINT64:
    case LLLY_TYPE_UNION:
    case LLLY_TYPE_DEC64:
        json_print_string(out, leaf->value_str);
        break;

    case LLLY_TYPE_INT8:
    case LLLY_TYPE_INT16:
    case LLLY_TYPE_INT32:
    case LLLY_TYPE_UINT8:
    case LLLY_TYPE_UINT16:
    case LLLY_TYPE_UINT32:
    case LLLY_TYPE_BOOL:
        llly_print(out, "%s", leaf->value_str[0] ? leaf->value_str : "null");
        break;

    case LLLY_TYPE_IDENT:
        p = strchr(leaf->value_str, ':');
        assert(p);
        len = p - leaf->value_str;
        mod_name = leaf->schema->module->name;
        if (!strncmp(leaf->value_str, mod_name, len) && !mod_name[len]) {
            /* do not print the prefix, it is the default prefix for this node */
            json_print_string(out, ++p);
        } else {
            json_print_string(out, leaf->value_str);
        }
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
        goto contentprint;

    case LLLY_TYPE_EMPTY:
        llly_print(out, "[null]");
        break;

    default:
        /* error */
        LOGINT(node->schema->module->ctx);
        return EXIT_FAILURE;
    }

    /* print attributes as sibling leafs */
    if (!onlyvalue && (node->attr || wdmod)) {
        if (schema) {
            llly_print(out, ",%s%*s\"@%s:%s\":%s{%s", (level ? "\n" : ""), LEVEL, INDENT, schema, node->schema->name,
                     (level ? " " : ""), (level ? "\n" : ""));
        } else {
            llly_print(out, ",%s%*s\"@%s\":%s{%s", (level ? "\n" : ""), LEVEL, INDENT, node->schema->name,
                     (level ? " " : ""), (level ? "\n" : ""));
        }
        if (json_print_attrs(out, (level ? level + 1 : level), node, wdmod)) {
            return EXIT_FAILURE;
        }
        llly_print(out, "%*s}", LEVEL, INDENT);
    }

    LLLY_PRINT_RET(node->schema->module->ctx);
}

static int
json_print_container(struct lllyout *out, int level, const struct lllyd_node *node, int toplevel, int options)
{
    const char *schema;

    LLLY_PRINT_SET;

    if (toplevel || !node->parent || nscmp(node, node->parent)) {
        /* print "namespace" */
        schema = lllys_node_module(node->schema)->name;
        llly_print(out, "%*s\"%s:%s\":%s{%s", LEVEL, INDENT, schema, node->schema->name, (level ? " " : ""), (level ? "\n" : ""));
    } else {
        llly_print(out, "%*s\"%s\":%s{%s", LEVEL, INDENT, node->schema->name, (level ? " " : ""), (level ? "\n" : ""));
    }
    if (level) {
        level++;
    }
    if (node->attr) {
        llly_print(out, "%*s\"@\":%s{%s", LEVEL, INDENT, (level ? " " : ""), (level ? "\n" : ""));
        if (json_print_attrs(out, (level ? level + 1 : level), node, NULL)) {
            return EXIT_FAILURE;
        }
        llly_print(out, "%*s}", LEVEL, INDENT);
        if (node->child) {
            llly_print(out, ",%s", (level ? "\n" : ""));
        }
    }
    if (json_print_nodes(out, level, node->child, 1, 0, options)) {
        return EXIT_FAILURE;
    }
    if (level) {
        level--;
    }
    llly_print(out, "%*s}", LEVEL, INDENT);

    LLLY_PRINT_RET(node->schema->module->ctx);
}

static int
json_print_leaf_list(struct lllyout *out, int level, const struct lllyd_node *node, int is_list, int toplevel, int options)
{
    const char *schema = NULL;
    const struct lllyd_node *list = node;
    int flag_empty = 0, flag_attrs = 0;

    LLLY_PRINT_SET;

    if (is_list && !list->child) {
        /* empty, e.g. in case of filter */
        flag_empty = 1;
    }

    if (toplevel || !node->parent || nscmp(node, node->parent)) {
        /* print "namespace" */
        schema = lllys_node_module(node->schema)->name;
        llly_print(out, "%*s\"%s:%s\":", LEVEL, INDENT, schema, node->schema->name);
    } else {
        llly_print(out, "%*s\"%s\":", LEVEL, INDENT, node->schema->name);
    }

    if (flag_empty) {
        llly_print(out, "%snull", (level ? " " : ""));
        goto finish;
    }
    llly_print(out, "%s[%s", (level ? " " : ""), (level ? "\n" : ""));

    if (!is_list && level) {
        ++level;
    }

    while (list) {
        if (is_list) {
            /* list print */
            if (level) {
                ++level;
            }
            llly_print(out, "%*s{%s", LEVEL, INDENT, (level ? "\n" : ""));
            if (level) {
                ++level;
            }
            if (list->attr) {
                llly_print(out, "%*s\"@\":%s{%s", LEVEL, INDENT, (level ? " " : ""), (level ? "\n" : ""));
                if (json_print_attrs(out, (level ? level + 1 : level), list, NULL)) {
                    return EXIT_FAILURE;
                }
                if (list->child) {
                    llly_print(out, "%*s},%s", LEVEL, INDENT, (level ? "\n" : ""));
                } else {
                    llly_print(out, "%*s}", LEVEL, INDENT);
                }
            }
            if (json_print_nodes(out, level, list->child, 1, 0, options)) {
                return EXIT_FAILURE;
            }
            if (level) {
                --level;
            }
            llly_print(out, "%*s}", LEVEL, INDENT);
            if (level) {
                --level;
            }
        } else {
            /* leaf-list print */
            llly_print(out, "%*s", LEVEL, INDENT);
            if (json_print_leaf(out, level, list, 1, toplevel, options)) {
                return EXIT_FAILURE;
            }
            if (list->attr) {
                flag_attrs = 1;
            }
        }
        if (toplevel && !(options & LLLYP_WITHSIBLINGS)) {
            /* if initially called without LLLYP_WITHSIBLINGS do not print other list entries */
            break;
        }
        for (list = list->next; list && list->schema != node->schema; list = list->next);
        if (list) {
            llly_print(out, ",%s", (level ? "\n" : ""));
        }
    }

    if (!is_list && level) {
        --level;
    }

    llly_print(out, "%s%*s]", (level ? "\n" : ""), LEVEL, INDENT);

    /* attributes */
    if (!is_list && flag_attrs) {
        if (schema) {
            llly_print(out, ",%s%*s\"@%s:%s\":%s[%s", (level ? "\n" : ""), LEVEL, INDENT, schema, node->schema->name,
                     (level ? " " : ""), (level ? "\n" : ""));
        } else {
            llly_print(out, ",%s%*s\"@%s\":%s[%s", (level ? "\n" : ""), LEVEL, INDENT, node->schema->name,
                     (level ? " " : ""), (level ? "\n" : ""));
        }
        if (level) {
            level++;
        }
        for (list = node; list; ) {
            if (list->attr) {
                llly_print(out, "%*s{%s", LEVEL, INDENT, (level ? " " : ""));
                if (json_print_attrs(out, 0, list, NULL)) {
                    return EXIT_FAILURE;
                }
                llly_print(out, "%*s}", LEVEL, INDENT);
            } else {
                llly_print(out, "%*snull", LEVEL, INDENT);
            }


            for (list = list->next; list && list->schema != node->schema; list = list->next);
            if (list) {
                llly_print(out, ",%s", (level ? "\n" : ""));
            }
        }
        if (level) {
            level--;
        }
        llly_print(out, "%s%*s]", (level ? "\n" : ""), LEVEL, INDENT);
    }

finish:
    LLLY_PRINT_RET(node->schema->module->ctx);
}

static int
json_print_anydataxml(struct lllyout *out, int level, const struct lllyd_node *node, int toplevel, int options)
{
    struct lllyd_node_anydata *any = (struct lllyd_node_anydata *)node;
    int is_object = 0;
    char *buf;
    const char *schema = NULL;

    LLLY_PRINT_SET;

    if (toplevel || !node->parent || nscmp(node, node->parent)) {
        /* print "namespace" */
        schema = lllys_node_module(node->schema)->name;
        llly_print(out, "%*s\"%s:%s\":", LEVEL, INDENT, schema, node->schema->name);
    } else {
        llly_print(out, "%*s\"%s\":", LEVEL, INDENT, node->schema->name);
    }
    if (level) {
        level++;
    }

    switch (any->value_type) {
    case LLLYD_ANYDATA_DATATREE:
        is_object = 1;
        llly_print(out, "%s{%s", (level ? " " : ""), (level ? "\n" : ""));
        /* do not print any default values nor empty containers */
        if (json_print_nodes(out, level, any->value.tree, 1, 0,  LLLYP_WITHSIBLINGS | (options & ~LLLYP_NETCONF))) {
            return EXIT_FAILURE;
        }
        break;
    case LLLYD_ANYDATA_JSON:
        if (level) {
            llly_print(out, "\n");
        }
        if (any->value.str) {
            llly_print(out, "%s", any->value.str);
        }
        if (level && (!any->value.str || (any->value.str[strlen(any->value.str) - 1] != '\n'))) {
            /* do not print 2 newlines */
            llly_print(out, "\n");
        }
        break;
    case LLLYD_ANYDATA_XML:
        lllyxml_print_mem(&buf, any->value.xml, (level ? LLLYXML_PRINT_FORMAT | LLLYXML_PRINT_NO_LAST_NEWLINE : 0)
                                               | LLLYXML_PRINT_SIBLINGS);
        if (level) {
            llly_print(out, " ");
        }
        json_print_string(out, buf);
        free(buf);
        break;
    case LLLYD_ANYDATA_CONSTSTRING:
    case LLLYD_ANYDATA_SXML:
        if (level) {
            llly_print(out, " ");
        }
        if (any->value.str) {
            json_print_string(out, any->value.str);
        } else {
            llly_print(out, "\"\"");
        }
        break;
    case LLLYD_ANYDATA_STRING:
    case LLLYD_ANYDATA_SXMLD:
    case LLLYD_ANYDATA_JSOND:
    case LLLYD_ANYDATA_LYBD:
    case LLLYD_ANYDATA_LYB:
        /* other formats are not supported */
        LOGINT(node->schema->module->ctx);
        return EXIT_FAILURE;
    }

    /* print attributes as sibling leaf */
    if (node->attr) {
        if (schema) {
            llly_print(out, ",%s%*s\"@%s:%s\":%s{%s", (level ? "\n" : ""), LEVEL, INDENT, schema, node->schema->name,
                     (level ? " " : ""), (level ? "\n" : ""));
        } else {
            llly_print(out, ",%s%*s\"@%s\":%s{%s", (level ? "\n" : ""), LEVEL, INDENT, node->schema->name,
                     (level ? " " : ""), (level ? "\n" : ""));
        }
        if (json_print_attrs(out, (level ? level + 1 : level), node, NULL)) {
            return EXIT_FAILURE;
        }
        llly_print(out, "%*s}", LEVEL, INDENT);
    }

    if (level) {
        level--;
    }
    if (is_object) {
        llly_print(out, "%*s}", LEVEL, INDENT);
    }

    LLLY_PRINT_RET(node->schema->module->ctx);
}

static int
json_print_nodes(struct lllyout *out, int level, const struct lllyd_node *root, int withsiblings, int toplevel, int options)
{
    int comma_flag = 0;
    const struct lllyd_node *node, *iter;

    LLLY_PRINT_SET;

    LLLY_TREE_FOR(root, node) {
        if (!lllyd_toprint(node, options)) {
            /* wd says do not print */
            continue;
        }

        switch (node->schema->nodetype) {
        case LLLYS_RPC:
        case LLLYS_ACTION:
        case LLLYS_NOTIF:
        case LLLYS_CONTAINER:
            if (comma_flag) {
                /* print the previous comma */
                llly_print(out, ",%s", (level ? "\n" : ""));
            }
            if (json_print_container(out, level, node, toplevel, options)) {
                return EXIT_FAILURE;
            }
            break;
        case LLLYS_LEAF:
            if (comma_flag) {
                /* print the previous comma */
                llly_print(out, ",%s", (level ? "\n" : ""));
            }
            if (json_print_leaf(out, level, node, 0, toplevel, options)) {
                return EXIT_FAILURE;
            }
            break;
        case LLLYS_LEAFLIST:
        case LLLYS_LIST:
            /* is it already printed? (root node is not) */
            for (iter = node->prev; iter->next && node != root; iter = iter->prev) {
                if (iter == node) {
                    continue;
                }
                if (iter->schema == node->schema) {
                    /* the list has alread some previous instance and therefore it is already printed */
                    break;
                }
            }
            if (!iter->next || node == root) {
                if (comma_flag) {
                    /* print the previous comma */
                    llly_print(out, ",%s", (level ? "\n" : ""));
                }

                /* print the list/leaflist */
                if (json_print_leaf_list(out, level, node, node->schema->nodetype == LLLYS_LIST ? 1 : 0, toplevel, options)) {
                    return EXIT_FAILURE;
                }
            }
            break;
        case LLLYS_ANYXML:
        case LLLYS_ANYDATA:
            if (comma_flag) {
                /* print the previous comma */
                llly_print(out, ",%s", (level ? "\n" : ""));
            }
            if (json_print_anydataxml(out, level, node, toplevel, options)) {
                return EXIT_FAILURE;
            }
            break;
        default:
            LOGINT(node->schema->module->ctx);
            return EXIT_FAILURE;
        }

        if (!withsiblings) {
            break;
        }
        comma_flag = 1;
    }
    if (root && level) {
        llly_print(out, "\n");
    }

    LLLY_PRINT_RET(root ? root->schema->module->ctx : NULL);
}

int
json_print_data(struct lllyout *out, const struct lllyd_node *root, int options)
{
    const struct lllyd_node *node, *next;
    int level = 0, action_input = 0;

    LLLY_PRINT_SET;

    if (options & LLLYP_FORMAT) {
        ++level;
    }

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

        if (node && (node->schema->nodetype & (LLLYS_RPC | LLLYS_ACTION))) {
            if (node->child && (node->child->schema->parent->nodetype == LLLYS_OUTPUT)) {
                /* skip the container */
                root = node->child;
            } else if (node->schema->nodetype == LLLYS_ACTION) {
                action_input = 1;
            }
        }
    }

    /* start */
    llly_print(out, "{%s", (level ? "\n" : ""));

    if (action_input) {
        llly_print(out, "%*s\"yang:action\":%s{%s", LEVEL, INDENT, (level ? " " : ""), (level ? "\n" : ""));
        if (level) {
            ++level;
        }
    }

    /* content */
    if (json_print_nodes(out, level, root, options & LLLYP_WITHSIBLINGS, 1, options)) {
        return EXIT_FAILURE;
    }

    if (action_input) {
        if (level) {
            --level;
        }
        llly_print(out, "%*s}%s", LEVEL, INDENT, (level ? "\n" : ""));
    }

    /* end */
    llly_print(out, "}%s", (level ? "\n" : ""));

    llly_print_flush(out);
    LLLY_PRINT_RET(NULL);
}
