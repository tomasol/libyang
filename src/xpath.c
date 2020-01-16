/**
 * @file xpath.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief YANG XPath evaluation functions
 *
 * Copyright (c) 2015 - 2017 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

/* needed by libmath functions isfinite(), isinf(), isnan(), signbit(), ... */
#define _ISOC99_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include <pcre.h>

#include "xpath.h"
#include "libyang.h"
#include "xml_internal.h"
#include "tree_schema.h"
#include "tree_data.h"
#include "context.h"
#include "tree_internal.h"
#include "common.h"
#include "resolve.h"
#include "printer.h"
#include "parser.h"
#include "hash_table.h"

static const struct lllyd_node *moveto_get_root(const struct lllyd_node *cur_node, int options,
                                              enum lllyxp_node_type *root_type);
static int reparse_or_expr(struct llly_ctx *ctx, struct lllyxp_expr *exp, uint16_t *exp_idx);
static int set_snode_insert_node(struct lllyxp_set *set, const struct lllys_node *node, enum lllyxp_node_type node_type);
static int eval_expr_select(struct lllyxp_expr *exp, uint16_t *exp_idx, enum lllyxp_expr_type etype, struct lllyd_node *cur_node,
                            struct lllys_module *local_mod, struct lllyxp_set *set, int options);

void
lllyxp_expr_free(struct lllyxp_expr *expr)
{
    uint16_t i;

    if (!expr) {
        return;
    }

    free(expr->expr);
    free(expr->tokens);
    free(expr->expr_pos);
    free(expr->tok_len);
    if (expr->repeat) {
        for (i = 0; i < expr->used; ++i) {
            free(expr->repeat[i]);
        }
    }
    free(expr->repeat);
    free(expr);
}

/**
 * @brief Print the type of an XPath \p set.
 *
 * @param[in] set Set to use.
 *
 * @return Set type string.
 */
static const char *
print_set_type(struct lllyxp_set *set)
{
    switch (set->type) {
    case LLLYXP_SET_EMPTY:
        return "empty";
    case LLLYXP_SET_NODE_SET:
        return "node set";
    case LLLYXP_SET_SNODE_SET:
        return "schema node set";
    case LLLYXP_SET_BOOLEAN:
        return "boolean";
    case LLLYXP_SET_NUMBER:
        return "number";
    case LLLYXP_SET_STRING:
        return "string";
    }

    return NULL;
}

/**
 * @brief Print an XPath token \p tok type.
 *
 * @param[in] tok Token to use.
 *
 * @return Token type string.
 */
static const char *
print_token(enum lllyxp_token tok)
{
    switch (tok) {
    case LLLYXP_TOKEN_PAR1:
        return "(";
    case LLLYXP_TOKEN_PAR2:
        return ")";
    case LLLYXP_TOKEN_BRACK1:
        return "[";
    case LLLYXP_TOKEN_BRACK2:
        return "]";
    case LLLYXP_TOKEN_DOT:
        return ".";
    case LLLYXP_TOKEN_DDOT:
        return "..";
    case LLLYXP_TOKEN_AT:
        return "@";
    case LLLYXP_TOKEN_COMMA:
        return ",";
    case LLLYXP_TOKEN_NAMETEST:
        return "NameTest";
    case LLLYXP_TOKEN_NODETYPE:
        return "NodeType";
    case LLLYXP_TOKEN_FUNCNAME:
        return "FunctionName";
    case LLLYXP_TOKEN_OPERATOR_LOG:
        return "Operator(Logic)";
    case LLLYXP_TOKEN_OPERATOR_COMP:
        return "Operator(Comparison)";
    case LLLYXP_TOKEN_OPERATOR_MATH:
        return "Operator(Math)";
    case LLLYXP_TOKEN_OPERATOR_UNI:
        return "Operator(Union)";
    case LLLYXP_TOKEN_OPERATOR_PATH:
        return "Operator(Path)";
    case LLLYXP_TOKEN_LITERAL:
        return "Literal";
    case LLLYXP_TOKEN_NUMBER:
        return "Number";
    default:
        LOGINT(NULL);
        return "";
    }
}

/**
 * @brief Print the whole expression \p exp to debug output.
 *
 * @param[in] exp Expression to use.
 */
static void
print_expr_struct_debug(struct lllyxp_expr *exp)
{
    uint16_t i, j;
    char tmp[128];

    if (!exp || (llly_log_level < LLLY_LLDBG)) {
        return;
    }

    LOGDBG(LLLY_LDGXPATH, "expression \"%s\":", exp->expr);
    for (i = 0; i < exp->used; ++i) {
        sprintf(tmp, "\ttoken %s, in expression \"%.*s\"", print_token(exp->tokens[i]), exp->tok_len[i],
               &exp->expr[exp->expr_pos[i]]);
        if (exp->repeat[i]) {
            sprintf(tmp + strlen(tmp), " (repeat %d", exp->repeat[i][0]);
            for (j = 1; exp->repeat[i][j]; ++j) {
                sprintf(tmp + strlen(tmp), ", %d", exp->repeat[i][j]);
            }
            strcat(tmp, ")");
        }
        LOGDBG(LLLY_LDGXPATH, tmp);
    }
}

#ifndef NDEBUG

/**
 * @brief Print XPath set content to debug output.
 *
 * @param[in] set Set to print.
 */
static void
print_set_debug(struct lllyxp_set *set)
{
    uint32_t i;
    char *str_num;
    struct lllyxp_set_node *item;
    struct lllyxp_set_snode *sitem;

    if (llly_log_level < LLLY_LLDBG) {
        return;
    }

    switch (set->type) {
    case LLLYXP_SET_NODE_SET:
        LOGDBG(LLLY_LDGXPATH, "set NODE SET:");
        for (i = 0; i < set->used; ++i) {
            item = &set->val.nodes[i];

            switch (item->type) {
            case LLLYXP_NODE_ROOT:
                LOGDBG(LLLY_LDGXPATH, "\t%d (pos %u): ROOT", i + 1, item->pos);
                break;
            case LLLYXP_NODE_ROOT_CONFIG:
                LOGDBG(LLLY_LDGXPATH, "\t%d (pos %u): ROOT CONFIG", i + 1, item->pos);
                break;
            case LLLYXP_NODE_ELEM:
                if ((item->node->schema->nodetype == LLLYS_LIST)
                        && (item->node->child->schema->nodetype == LLLYS_LEAF)) {
                    LOGDBG(LLLY_LDGXPATH, "\t%d (pos %u): ELEM %s (1st child val: %s)", i + 1, item->pos,
                           item->node->schema->name,
                           ((struct lllyd_node_leaf_list *)item->node->child)->value_str);
                } else if (item->node->schema->nodetype == LLLYS_LEAFLIST) {
                    LOGDBG(LLLY_LDGXPATH, "\t%d (pos %u): ELEM %s (val: %s)", i + 1, item->pos,
                           item->node->schema->name,
                           ((struct lllyd_node_leaf_list *)item->node)->value_str);
                } else {
                    LOGDBG(LLLY_LDGXPATH, "\t%d (pos %u): ELEM %s", i + 1, item->pos, item->node->schema->name);
                }
                break;
            case LLLYXP_NODE_TEXT:
                if (item->node->schema->nodetype & LLLYS_ANYDATA) {
                    LOGDBG(LLLY_LDGXPATH, "\t%d (pos %u): TEXT <%s>", i + 1, item->pos,
                           item->node->schema->nodetype == LLLYS_ANYXML ? "anyxml" : "anydata");
                } else {
                    LOGDBG(LLLY_LDGXPATH, "\t%d (pos %u): TEXT %s", i + 1, item->pos,
                           ((struct lllyd_node_leaf_list *)item->node)->value_str);
                }
                break;
            case LLLYXP_NODE_ATTR:
                LOGDBG(LLLY_LDGXPATH, "\t%d (pos %u): ATTR %s = %s", i + 1, item->pos, set->val.attrs[i].attr->name,
                       set->val.attrs[i].attr->value);
                break;
            default:
                LOGINT(NULL);
                break;
            }
        }
        break;

    case LLLYXP_SET_SNODE_SET:
        LOGDBG(LLLY_LDGXPATH, "set SNODE SET:");
        for (i = 0; i < set->used; ++i) {
            sitem = &set->val.snodes[i];

            switch (sitem->type) {
            case LLLYXP_NODE_ROOT:
                LOGDBG(LLLY_LDGXPATH, "\t%d (%u): ROOT", i + 1, sitem->in_ctx);
                break;
            case LLLYXP_NODE_ROOT_CONFIG:
                LOGDBG(LLLY_LDGXPATH, "\t%d (%u): ROOT CONFIG", i + 1, sitem->in_ctx);
                break;
            case LLLYXP_NODE_ELEM:
                LOGDBG(LLLY_LDGXPATH, "\t%d (%u): ELEM %s", i + 1, sitem->in_ctx, sitem->snode->name);
                break;
            default:
                LOGINT(NULL);
                break;
            }
        }
        break;

    case LLLYXP_SET_EMPTY:
        LOGDBG(LLLY_LDGXPATH, "set EMPTY");
        break;

    case LLLYXP_SET_BOOLEAN:
        LOGDBG(LLLY_LDGXPATH, "set BOOLEAN");
        LOGDBG(LLLY_LDGXPATH, "\t%s", (set->val.bool ? "true" : "false"));
        break;

    case LLLYXP_SET_STRING:
        LOGDBG(LLLY_LDGXPATH, "set STRING");
        LOGDBG(LLLY_LDGXPATH, "\t%s", set->val.str);
        break;

    case LLLYXP_SET_NUMBER:
        LOGDBG(LLLY_LDGXPATH, "set NUMBER");

        if (isnan(set->val.num)) {
            str_num = strdup("NaN");
        } else if ((set->val.num == 0) || (set->val.num == -0.0f)) {
            str_num = strdup("0");
        } else if (isinf(set->val.num) && !signbit(set->val.num)) {
            str_num = strdup("Infinity");
        } else if (isinf(set->val.num) && signbit(set->val.num)) {
            str_num = strdup("-Infinity");
        } else if ((long long)set->val.num == set->val.num) {
            if (asprintf(&str_num, "%lld", (long long)set->val.num) == -1) {
                str_num = NULL;
            }
        } else {
            if (asprintf(&str_num, "%03.1Lf", set->val.num) == -1) {
                str_num = NULL;
            }
        }
        LLLY_CHECK_ERR_RETURN(!str_num, LOGMEM(NULL), );

        LOGDBG(LLLY_LDGXPATH, "\t%s", str_num);
        free(str_num);
    }
}

#endif

/**
 * @brief Realloc the string \p str.
 *
 * @param[in] needed How much free space is required.
 * @param[in,out] str Pointer to the string to use.
 * @param[in,out] used Used bytes in \p str.
 * @param[in,out] size Allocated bytes in \p str.
 *
 * @return 0 on success, non-zero on error
 */
static int
cast_string_realloc(struct llly_ctx *ctx, uint16_t needed, char **str, uint16_t *used, uint16_t *size)
{
    if (*size - *used < needed) {
        do {
            if ((UINT16_MAX - *size) < LLLYXP_STRING_CAST_SIZE_STEP) {
                LOGERR(ctx, LLLY_EINVAL, "XPath string length limit (%u) reached.", UINT16_MAX);
                return -1;
            }
            *size += LLLYXP_STRING_CAST_SIZE_STEP;
        } while (*size - *used < needed);
        *str = llly_realloc(*str, *size * sizeof(char));
        LLLY_CHECK_ERR_RETURN(!(*str), LOGMEM(ctx), -1);
    }

    return 0;
}

/**
 * @brief Cast nodes recursively to one string \p str.
 *
 * @param[in] node Node to cast.
 * @param[in] fake_cont Whether to put the data into a "fake" container.
 * @param[in] root_type Type of the XPath root.
 * @param[in] indent Current indent.
 * @param[in,out] str Resulting string.
 * @param[in,out] used Used bytes in \p str.
 * @param[in,out] size Allocated bytes in \p str.
 */
static int
cast_string_recursive(struct lllyd_node *node, struct lllys_module *local_mod, int fake_cont, enum lllyxp_node_type root_type,
                      uint16_t indent, char **str, uint16_t *used, uint16_t *size)
{
    char *buf, *line, *ptr;
    const char *value_str;
    struct lllyd_node *child;
    struct lllyd_node_anydata *any;

    if ((root_type == LLLYXP_NODE_ROOT_CONFIG) && (node->schema->flags & LLLYS_CONFIG_R)) {
        return 0;
    }

    if (fake_cont) {
        if (cast_string_realloc(local_mod->ctx, 1, str, used, size)) {
            return -1;
        }
        strcpy(*str + (*used - 1), "\n");
        ++(*used);

        ++indent;
    }

    switch (node->schema->nodetype) {
    case LLLYS_CONTAINER:
    case LLLYS_LIST:
    case LLLYS_RPC:
    case LLLYS_NOTIF:
        if (cast_string_realloc(local_mod->ctx, 1, str, used, size)) {
            return -1;
        }
        strcpy(*str + (*used - 1), "\n");
        ++(*used);

        LLLY_TREE_FOR(node->child, child) {
            if (cast_string_recursive(child, local_mod, 0, root_type, indent + 1, str, used, size)) {
                return -1;
            }
        }

        break;

    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
        value_str = ((struct lllyd_node_leaf_list *)node)->value_str;
        if (!value_str) {
            value_str = "";
        }

        /* print indent */
        if (cast_string_realloc(local_mod->ctx, indent * 2 + strlen(value_str) + 1, str, used, size)) {
            return -1;
        }
        memset(*str + (*used - 1), ' ', indent * 2);
        *used += indent * 2;

        /* print value */
        if (*used == 1) {
            sprintf(*str + (*used - 1), "%s", value_str);
            *used += strlen(value_str);
        } else {
            sprintf(*str + (*used - 1), "%s\n", value_str);
            *used += strlen(value_str) + 1;
        }

        break;

    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        any = (struct lllyd_node_anydata *)node;
        if (!(void*)any->value.tree) {
            /* no content */
            buf = strdup("");
            LLLY_CHECK_ERR_RETURN(!buf, LOGMEM(local_mod->ctx), -1);
        } else {
            switch (any->value_type) {
            case LLLYD_ANYDATA_CONSTSTRING:
            case LLLYD_ANYDATA_SXML:
            case LLLYD_ANYDATA_JSON:
                buf = strdup(any->value.str);
                LLLY_CHECK_ERR_RETURN(!buf, LOGMEM(local_mod->ctx), -1);
                break;
            case LLLYD_ANYDATA_DATATREE:
                if (lllyd_print_mem(&buf, any->value.tree, LLLYD_XML, LLLYP_WITHSIBLINGS)) {
                    return -1;
                }
                break;
            case LLLYD_ANYDATA_XML:
                if (!lllyxml_print_mem(&buf, any->value.xml, LLLYXML_PRINT_SIBLINGS)) {
                    return -1;
                }
                break;
            case LLLYD_ANYDATA_LYB:
                LOGERR(local_mod->ctx, LLLY_EINVAL, "Cannot convert LLLYB anydata into string.");
                return -1;
            case LLLYD_ANYDATA_STRING:
            case LLLYD_ANYDATA_SXMLD:
            case LLLYD_ANYDATA_JSOND:
            case LLLYD_ANYDATA_LYBD:
                /* dynamic strings are used only as input parameters */
                LOGINT(local_mod->ctx);
                return -1;
            }
        }

        line = strtok_r(buf, "\n", &ptr);
        do {
            if (cast_string_realloc(local_mod->ctx, indent * 2 + strlen(line) + 1, str, used, size)) {
                free(buf);
                return -1;
            }
            memset(*str + (*used - 1), ' ', indent * 2);
            *used += indent * 2;

            strcpy(*str + (*used - 1), line);
            *used += strlen(line);

            strcpy(*str + (*used - 1), "\n");
            *used += 1;
        } while ((line = strtok_r(NULL, "\n", &ptr)));

        free(buf);
        break;

    default:
        LOGINT(local_mod->ctx);
        return -1;
    }

    if (fake_cont) {
        if (cast_string_realloc(local_mod->ctx, 1, str, used, size)) {
            return -1;
        }
        strcpy(*str + (*used - 1), "\n");
        ++(*used);

        --indent;
    }

    return 0;
}

/**
 * @brief Cast an element into a string.
 *
 * @param[in] node Node to cast.
 * @param[in] fake_cont Whether to put the data into a "fake" container.
 * @param[in] root_type Type of the XPath root.
 *
 * @return Element cast to dynamically-allocated string.
 */
static char *
cast_string_elem(struct lllyd_node *node, struct lllys_module *local_mod, int fake_cont, enum lllyxp_node_type root_type)
{
    char *str;
    uint16_t used, size;

    str = malloc(LLLYXP_STRING_CAST_SIZE_START * sizeof(char));
    LLLY_CHECK_ERR_RETURN(!str, LOGMEM(local_mod->ctx), NULL);
    str[0] = '\0';
    used = 1;
    size = LLLYXP_STRING_CAST_SIZE_START;

    if (cast_string_recursive(node, local_mod, fake_cont, root_type, 0, &str, &used, &size)) {
        free(str);
        return NULL;
    }

    if (size > used) {
        str = llly_realloc(str, used * sizeof(char));
        LLLY_CHECK_ERR_RETURN(!str, free(str); LOGMEM(local_mod->ctx), NULL);
    }
    return str;
}

/**
 * @brief Cast a LLLYXP_SET_NODE_SET set into a string.
 *        Context position aware.
 *
 * @param[in] set Set to cast.
 * @param[in] cur_node Original context node.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return Cast string in the dictionary.
 */
static char *
cast_node_set_to_string(struct lllyxp_set *set, struct lllyd_node *cur_node, struct lllys_module *local_mod, int options)
{
    enum lllyxp_node_type root_type;
    char *str;

    if ((set->val.nodes[0].type != LLLYXP_NODE_ATTR) && (set->val.nodes[0].node->validity & LLLYD_VAL_INUSE)) {
        LOGVAL(local_mod->ctx, LLLYE_XPATH_DUMMY, LLLY_VLOG_LYD, set->val.nodes[0].node, set->val.nodes[0].node->schema->name);
        return NULL;
    }

    moveto_get_root(cur_node, options, &root_type);

    switch (set->val.nodes[0].type) {
    case LLLYXP_NODE_ROOT:
    case LLLYXP_NODE_ROOT_CONFIG:
        return cast_string_elem(set->val.nodes[0].node, local_mod, 1, root_type);
    case LLLYXP_NODE_ELEM:
    case LLLYXP_NODE_TEXT:
        return cast_string_elem(set->val.nodes[0].node, local_mod, 0, root_type);
    case LLLYXP_NODE_ATTR:
        str = strdup(set->val.attrs[0].attr->value_str);
        if (!str) {
            LOGMEM(local_mod->ctx);
        }
        return str;
    default:
        break;
    }

    LOGINT(local_mod->ctx);
    return NULL;
}

/**
 * @brief Cast a string into an XPath number.
 *
 * @param[in] str String to use.
 *
 * @return Cast number.
 */
static long double
cast_string_to_number(const char *str)
{
    long double num;
    char *ptr;

    errno = 0;
    num = strtold(str, &ptr);
    if (errno || *ptr) {
        num = NAN;
    }
    return num;
}

/*
 * lllyxp_set manipulation functions
 */

#ifdef LLLY_ENABLED_CACHE

static int
set_values_equal_cb(void *val1_p, void *val2_p, int UNUSED(mod), void *UNUSED(cb_data))
{
    struct lllyxp_set_hash_node *val1, *val2;

    val1 = (struct lllyxp_set_hash_node *)val1_p;
    val2 = (struct lllyxp_set_hash_node *)val2_p;

    if ((val1->node == val2->node) && (val1->type == val2->type)) {
        return 1;
    }

    return 0;
}

static void
set_insert_node_hash(struct lllyxp_set *set, struct lllyd_node *node, enum lllyxp_node_type type)
{
    int r;
    uint32_t i, hash;
    struct lllyxp_set_hash_node hnode;

    if (!set->ht && (set->used >= LLLY_CACHE_HT_MIN_CHILDREN)) {
        /* create hash table and add all the nodes */
        set->ht = lllyht_new(1, sizeof(struct lllyxp_set_hash_node), set_values_equal_cb, NULL, 1);
        for (i = 0; i < set->used; ++i) {
            hnode.node = set->val.nodes[i].node;
            hnode.type = set->val.nodes[i].type;

            hash = dict_hash_multi(0, (const char *)&hnode.node, sizeof hnode.node);
            hash = dict_hash_multi(hash, (const char *)&hnode.type, sizeof hnode.type);
            hash = dict_hash_multi(hash, NULL, 0);

            r = lllyht_insert(set->ht, &hnode, hash, NULL);
            assert(!r);
            (void)r;

            if (hnode.node == node) {
                /* it was just added, do not add it twice */
                node = NULL;
            }
        }
    }

    if (set->ht && node) {
        /* add the new node into hash table */
        hnode.node = node;
        hnode.type = type;

        hash = dict_hash_multi(0, (const char *)&hnode.node, sizeof hnode.node);
        hash = dict_hash_multi(hash, (const char *)&hnode.type, sizeof hnode.type);
        hash = dict_hash_multi(hash, NULL, 0);

        r = lllyht_insert(set->ht, &hnode, hash, NULL);
        assert(!r);
        (void)r;
    }
}

static void
set_remove_node_hash(struct lllyxp_set *set, struct lllyd_node *node, enum lllyxp_node_type type)
{
    int r;
    struct lllyxp_set_hash_node hnode;
    uint32_t hash;

    if (set->ht) {
        hnode.node = node;
        hnode.type = type;

        hash = dict_hash_multi(0, (const char *)&hnode.node, sizeof hnode.node);
        hash = dict_hash_multi(hash, (const char *)&hnode.type, sizeof hnode.type);
        hash = dict_hash_multi(hash, NULL, 0);

        r = lllyht_remove(set->ht, &hnode, hash);
        assert(!r);
        (void)r;

        if (!set->ht->used) {
            lllyht_free(set->ht);
            set->ht = NULL;
        }
    }
}

static int
set_dup_node_hash_check(const struct lllyxp_set *set, struct lllyd_node *node, enum lllyxp_node_type type, int skip_idx)
{
    struct lllyxp_set_hash_node hnode, *match_p;
    uint32_t hash;

    hnode.node = node;
    hnode.type = type;

    hash = dict_hash_multi(0, (const char *)&hnode.node, sizeof hnode.node);
    hash = dict_hash_multi(hash, (const char *)&hnode.type, sizeof hnode.type);
    hash = dict_hash_multi(hash, NULL, 0);

    if (!lllyht_find(set->ht, &hnode, hash, (void **)&match_p)) {
        if ((skip_idx > -1) && (set->val.nodes[skip_idx].node == match_p->node) && (set->val.nodes[skip_idx].type == match_p->type)) {
            /* we found it on the index that should be skipped, find another */
            hnode = *match_p;
            if (lllyht_find_next(set->ht, &hnode, hash, (void **)&match_p)) {
                /* none other found */
                return 0;
            }
        }

        return 1;
    }

    /* not found */
    return 0;
}

#endif

static void
set_free_content(struct lllyxp_set *set)
{
    if (!set) {
        return;
    }

    if (set->type == LLLYXP_SET_NODE_SET) {
        free(set->val.nodes);
#ifdef LLLY_ENABLED_CACHE
        lllyht_free(set->ht);
        set->ht = NULL;
#endif
    } else if (set->type == LLLYXP_SET_SNODE_SET) {
        free(set->val.snodes);
    } else if (set->type == LLLYXP_SET_STRING) {
        free(set->val.str);
    }
    set->type = LLLYXP_SET_EMPTY;
}

void
lllyxp_set_free(struct lllyxp_set *set)
{
    if (!set) {
        return;
    }

    set_free_content(set);
    free(set);
}

/**
 * @brief Create a deep copy of a \p set.
 *
 * @param[in] set Set to copy.
 *
 * @return Copy of \p set.
 */
static struct lllyxp_set *
set_copy(struct lllyxp_set *set)
{
    struct lllyxp_set *ret;
    uint16_t i;

    if (!set) {
        return NULL;
    }

    ret = malloc(sizeof *ret);
    LLLY_CHECK_ERR_RETURN(!ret, LOGMEM(NULL), NULL);

    if (set->type == LLLYXP_SET_SNODE_SET) {
        memset(ret, 0, sizeof *ret);
        ret->type = set->type;

        for (i = 0; i < set->used; ++i) {
            if (set->val.snodes[i].in_ctx == 1) {
                if (set_snode_insert_node(ret, set->val.snodes[i].snode, set->val.snodes[i].type)) {
                    lllyxp_set_free(ret);
                    return NULL;
                }
            }
        }
    } else if (set->type == LLLYXP_SET_NODE_SET) {
        ret->type = set->type;
        ret->val.nodes = malloc(set->used * sizeof *ret->val.nodes);
        LLLY_CHECK_ERR_RETURN(!ret->val.nodes, LOGMEM(NULL); free(ret), NULL);
        memcpy(ret->val.nodes, set->val.nodes, set->used * sizeof *ret->val.nodes);

        ret->used = ret->size = set->used;
        ret->ctx_pos = set->ctx_pos;
        ret->ctx_size = set->ctx_size;

#ifdef LLLY_ENABLED_CACHE
        ret->ht = lllyht_dup(set->ht);
#endif
    } else {
       memcpy(ret, set, sizeof *ret);
       if (set->type == LLLYXP_SET_STRING) {
           ret->val.str = strdup(set->val.str);
           LLLY_CHECK_ERR_RETURN(!ret->val.str, LOGMEM(NULL); free(ret), NULL);
       }
    }

    return ret;
}

/**
 * @brief Fill XPath set with a string. Any current data are disposed of.
 *
 * @param[in] set Set to fill.
 * @param[in] string String to fill into \p set.
 * @param[in] str_len Length of \p string. 0 is a valid value!
 * @param[in] ctx libyang context to use.
 */
static void
set_fill_string(struct lllyxp_set *set, const char *string, uint16_t str_len)
{
    set_free_content(set);

    set->type = LLLYXP_SET_STRING;
    if ((str_len == 0) && (string[0] != '\0')) {
        string = "";
    }
    set->val.str = strndup(string, str_len);
}

/**
 * @brief Fill XPath set with a number. Any current data are disposed of.
 *
 * @param[in] set Set to fill.
 * @param[in] number Number to fill into \p set.
 */
static void
set_fill_number(struct lllyxp_set *set, long double number)
{
    set_free_content(set);

    set->type = LLLYXP_SET_NUMBER;
    set->val.num = number;
}

/**
 * @brief Fill XPath set with a boolean. Any current data are disposed of.
 *
 * @param[in] set Set to fill.
 * @param[in] boolean Boolean to fill into \p set.
 */
static void
set_fill_boolean(struct lllyxp_set *set, int boolean)
{
    set_free_content(set);

    set->type = LLLYXP_SET_BOOLEAN;
    set->val.bool = boolean;
}

/**
 * @brief Fill XPath set with the value from another set (deep assign).
 *        Any current data are disposed of.
 *
 * @param[in] trg Set to fill.
 * @param[in] src Source set to copy into \p trg.
 */
static void
set_fill_set(struct lllyxp_set *trg, struct lllyxp_set *src)
{
    if (!trg || !src) {
        return;
    }

    if (src->type == LLLYXP_SET_SNODE_SET) {
        trg->type = LLLYXP_SET_SNODE_SET;
        trg->used = src->used;
        trg->size = src->used;

        trg->val.snodes = llly_realloc(trg->val.snodes, trg->size * sizeof *trg->val.nodes);
        LLLY_CHECK_ERR_RETURN(!trg->val.nodes, LOGMEM(NULL); memset(trg, 0, sizeof *trg), );
        memcpy(trg->val.nodes, src->val.nodes, src->used * sizeof *src->val.nodes);
    } else if (src->type == LLLYXP_SET_BOOLEAN) {
        set_fill_boolean(trg, src->val.bool);
    } else if (src->type ==  LLLYXP_SET_NUMBER) {
        set_fill_number(trg, src->val.num);
    } else if (src->type == LLLYXP_SET_STRING) {
        set_fill_string(trg, src->val.str, strlen(src->val.str));
    } else {
        if (trg->type == LLLYXP_SET_NODE_SET) {
            free(trg->val.nodes);
        } else if (trg->type == LLLYXP_SET_STRING) {
            free(trg->val.str);
        }

        if (src->type == LLLYXP_SET_EMPTY) {
            trg->type = LLLYXP_SET_EMPTY;
        } else {
            assert(src->type == LLLYXP_SET_NODE_SET);

            trg->type = LLLYXP_SET_NODE_SET;
            trg->used = src->used;
            trg->size = src->used;
            trg->ctx_pos = src->ctx_pos;
            trg->ctx_size = src->ctx_size;

            trg->val.nodes = malloc(trg->used * sizeof *trg->val.nodes);
            LLLY_CHECK_ERR_RETURN(!trg->val.nodes, LOGMEM(NULL); memset(trg, 0, sizeof *trg), );
            memcpy(trg->val.nodes, src->val.nodes, src->used * sizeof *src->val.nodes);
#ifdef LLLY_ENABLED_CACHE
            trg->ht = lllyht_dup(src->ht);
#endif
        }
    }


}

static void
set_snode_clear_ctx(struct lllyxp_set *set)
{
    uint32_t i;

    for (i = 0; i < set->used; ++i) {
        if (set->val.snodes[i].in_ctx == 1) {
            set->val.snodes[i].in_ctx = 0;
        }
    }
}

/**
 * @brief Remove a node from a set. Removing last node changes
 *        \p set into LLLYXP_SET_EMPTY. Context position aware.
 *
 * @param[in] set Set to use.
 * @param[in] idx Index from \p set of the node to be removed.
 */
static void
set_remove_node(struct lllyxp_set *set, uint32_t idx)
{
    assert(set && (set->type == LLLYXP_SET_NODE_SET));
    assert(idx < set->used);

#ifdef LLLY_ENABLED_CACHE
    set_remove_node_hash(set, set->val.nodes[idx].node, set->val.nodes[idx].type);
#endif

    --set->used;
    if (set->used) {
        memmove(&set->val.nodes[idx], &set->val.nodes[idx + 1],
                (set->used - idx) * sizeof *set->val.nodes);
    } else {
        set_free_content(set);
        /* this changes it to LLLYXP_SET_EMPTY */
        memset(set, 0, sizeof *set);
    }
}

/**
 * @brief Remove all none node types from a set. Removing last node changes
 *        \p set into LLLYXP_SET_EMPTY. Hashes are expected to be already removed. Context position aware.
 *
 * @param[in] set Set to use.
 * @param[in] idx Index from \p set of the node to be removed.
 */
static void
set_remove_none_nodes(struct lllyxp_set *set)
{
    uint16_t i, orig_used, end;
    int32_t start;

    assert(set && (set->type == LLLYXP_SET_NODE_SET));

    orig_used = set->used;
    set->used = 0;
    for (i = 0; i < orig_used;) {
        start = -1;
        do {
            if ((set->val.nodes[i].type != LLLYXP_NODE_NONE) && (start == -1)) {
                start = i;
            } else if ((start > -1) && (set->val.nodes[i].type == LLLYXP_NODE_NONE)) {
                end = i;
                ++i;
                break;
            }

            ++i;
            if (i == orig_used) {
                end = i;
            }
        } while (i < orig_used);

        if (start > -1) {
            if (set->used != (unsigned)start) {
                memmove(&set->val.nodes[set->used], &set->val.nodes[start], (end - start) * sizeof *set->val.nodes);
            }
            set->used += end - start;
        }
    }

    if (!set->used) {
        set_free_content(set);
        /* this changes it to LLLYXP_SET_EMPTY */
        memset(set, 0, sizeof *set);
    }
}

/**
 * @brief Check for duplicates in a node set.
 *
 * @param[in] set Set to check.
 * @param[in] node Node to look for in \p set.
 * @param[in] node_type Type of \p node.
 * @param[in] skip_idx Index from \p set to skip.
 *
 * @return 0 on success, 1 on duplicate found.
 */
static int
set_dup_node_check(const struct lllyxp_set *set, const struct lllyd_node *node, enum lllyxp_node_type node_type, int skip_idx)
{
    uint32_t i;

#ifdef LLLY_ENABLED_CACHE
    if (set->ht) {
        return set_dup_node_hash_check(set, (struct lllyd_node *)node, node_type, skip_idx);
    }
#endif

    for (i = 0; i < set->used; ++i) {
        if ((skip_idx > -1) && (i == (unsigned)skip_idx)) {
            continue;
        }

        if ((set->val.nodes[i].node == node) && (set->val.nodes[i].type == node_type)) {
            return 1;
        }
    }

    return 0;
}

static int
set_snode_dup_node_check(struct lllyxp_set *set, const struct lllys_node *node, enum lllyxp_node_type node_type, int skip_idx)
{
    uint32_t i;

    for (i = 0; i < set->used; ++i) {
        if ((skip_idx > -1) && (i == (unsigned)skip_idx)) {
            continue;
        }

        if ((set->val.snodes[i].snode == node) && (set->val.snodes[i].type == node_type)) {
            return i;
        }
    }

    return -1;
}

static void
set_snode_merge(struct lllyxp_set *set1, struct lllyxp_set *set2)
{
    uint32_t orig_used, i, j;

    assert(((set1->type == LLLYXP_SET_SNODE_SET) || (set1->type == LLLYXP_SET_EMPTY))
        && ((set2->type == LLLYXP_SET_SNODE_SET) || (set2->type == LLLYXP_SET_EMPTY)));

    if (set2->type == LLLYXP_SET_EMPTY) {
        return;
    }

    if (set1->type == LLLYXP_SET_EMPTY) {
        memcpy(set1, set2, sizeof *set1);
        return;
    }

    if (set1->used + set2->used > set1->size) {
        set1->size = set1->used + set2->used;
        set1->val.snodes = llly_realloc(set1->val.snodes, set1->size * sizeof *set1->val.snodes);
        LLLY_CHECK_ERR_RETURN(!set1->val.snodes, LOGMEM(NULL), );
    }

    orig_used = set1->used;

    for (i = 0; i < set2->used; ++i) {
        for (j = 0; j < orig_used; ++j) {
            /* detect duplicities */
            if (set1->val.snodes[j].snode == set2->val.snodes[i].snode) {
                break;
            }
        }

        if (j == orig_used) {
            memcpy(&set1->val.snodes[set1->used], &set2->val.snodes[i], sizeof *set2->val.snodes);
            ++set1->used;
        }
    }

    free(set2->val.snodes);
    memset(set2, 0, sizeof *set2);
}

/**
 * @brief Insert a node into a set. Context position aware.
 *
 * @param[in] set Set to use.
 * @param[in] node Node to insert to \p set.
 * @param[in] pos Sort position of \p node. If left 0, it is filled just before sorting.
 * @param[in] node_type Node type of \p node.
 * @param[in] idx Index in \p set to insert into.
 */
static void
set_insert_node(struct lllyxp_set *set, const struct lllyd_node *node, uint32_t pos, enum lllyxp_node_type node_type, uint32_t idx)
{
    assert(set && ((set->type == LLLYXP_SET_NODE_SET) || (set->type == LLLYXP_SET_EMPTY)));

    if (set->type == LLLYXP_SET_EMPTY) {
        /* first item */
        if (idx) {
            /* no real harm done, but it is a bug */
            LOGINT(NULL);
            idx = 0;
        }
        set->val.nodes = malloc(LLLYXP_SET_SIZE_START * sizeof *set->val.nodes);
        LLLY_CHECK_ERR_RETURN(!set->val.nodes, LOGMEM(NULL), );
        set->type = LLLYXP_SET_NODE_SET;
        set->used = 0;
        set->size = LLLYXP_SET_SIZE_START;
        set->ctx_pos = 1;
        set->ctx_size = 1;
#ifdef LLLY_ENABLED_CACHE
        set->ht = NULL;
#endif
    } else {
        /* not an empty set */
        if (set->used == set->size) {

            /* set is full */
            set->val.nodes = llly_realloc(set->val.nodes, (set->size + LLLYXP_SET_SIZE_STEP) * sizeof *set->val.nodes);
            LLLY_CHECK_ERR_RETURN(!set->val.nodes, LOGMEM(NULL), );
            set->size += LLLYXP_SET_SIZE_STEP;
        }

        if (idx > set->used) {
            LOGINT(NULL);
            idx = set->used;
        }

        /* make space for the new node */
        if (idx < set->used) {
            memmove(&set->val.nodes[idx + 1], &set->val.nodes[idx], (set->used - idx) * sizeof *set->val.nodes);
        }
    }

    /* finally assign the value */
    set->val.nodes[idx].node = (struct lllyd_node *)node;
    set->val.nodes[idx].type = node_type;
    set->val.nodes[idx].pos = pos;
    ++set->used;

#ifdef LLLY_ENABLED_CACHE
    set_insert_node_hash(set, (struct lllyd_node *)node, node_type);
#endif
}

static int
set_snode_insert_node(struct lllyxp_set *set, const struct lllys_node *node, enum lllyxp_node_type node_type)
{
    int ret;

    assert(set->type == LLLYXP_SET_SNODE_SET);

    ret = set_snode_dup_node_check(set, node, node_type, -1);
    if (ret > -1) {
        set->val.snodes[ret].in_ctx = 1;
    } else {
        if (set->used == set->size) {
            set->val.snodes = llly_realloc(set->val.snodes, (set->size + LLLYXP_SET_SIZE_STEP) * sizeof *set->val.snodes);
            LLLY_CHECK_ERR_RETURN(!set->val.snodes, LOGMEM(node->module->ctx), -1);
            set->size += LLLYXP_SET_SIZE_STEP;
        }

        ret = set->used;
        set->val.snodes[ret].snode = (struct lllys_node *)node;
        set->val.snodes[ret].type = node_type;
        set->val.snodes[ret].in_ctx = 1;
        ++set->used;
    }

    return ret;
}

/**
 * @brief Replace a node in a set with another. Context position aware.
 *
 * @param[in] set Set to use.
 * @param[in] node Node to insert to \p set.
 * @param[in] pos Sort position of \p node. If left 0, it is filled just before sorting.
 * @param[in] node_type Node type of \p node.
 * @param[in] idx Index in \p set of the node to replace.
 */
static void
set_replace_node(struct lllyxp_set *set, const struct lllyd_node *node, uint32_t pos, enum lllyxp_node_type node_type, uint32_t idx)
{
    assert(set && (idx < set->used));

#ifdef LLLY_ENABLED_CACHE
    set_remove_node_hash(set, set->val.nodes[idx].node, set->val.nodes[idx].type);
#endif
    set->val.nodes[idx].node = (struct lllyd_node *)node;
    set->val.nodes[idx].type = node_type;
    set->val.nodes[idx].pos = pos;
#ifdef LLLY_ENABLED_CACHE
    set_insert_node_hash(set, set->val.nodes[idx].node, set->val.nodes[idx].type);
#endif
}

static uint32_t
set_snode_new_in_ctx(struct lllyxp_set *set)
{
    uint32_t ret_ctx, i;

    assert(set->type == LLLYXP_SET_SNODE_SET);

    ret_ctx = 3;
retry:
    for (i = 0; i < set->used; ++i) {
        if (set->val.snodes[i].in_ctx >= ret_ctx) {
            ret_ctx = set->val.snodes[i].in_ctx + 1;
            goto retry;
        }
    }
    for (i = 0; i < set->used; ++i) {
        if (set->val.snodes[i].in_ctx == 1) {
            set->val.snodes[i].in_ctx = ret_ctx;
        }
    }

    return ret_ctx;
}

/**
 * @brief Get unique \p node position in the data.
 *
 * @param[in] node Node to find.
 * @param[in] node_type Node type of \p node.
 * @param[in] root Root node.
 * @param[in] root_type Type of the XPath \p root node.
 * @param[in] prev Node that we think is before \p node in DFS from \p root. Can optionally
 * be used to increase efficiency and start the DFS from this node.
 * @param[in] prev_pos Node \p prev position. Optional, but must be set if \p prev is set.
 *
 * @return Node position.
 */
static uint32_t
get_node_pos(const struct lllyd_node *node, enum lllyxp_node_type node_type, const struct lllyd_node *root,
             enum lllyxp_node_type root_type, const struct lllyd_node **prev, uint32_t *prev_pos)
{
    const struct lllyd_node *next, *elem, *top_sibling;
    uint32_t pos = 1;

    assert(prev && prev_pos && !root->prev->next);

    if ((node_type == LLLYXP_NODE_ROOT) || (node_type == LLLYXP_NODE_ROOT_CONFIG)) {
        return 0;
    }

    if (*prev) {
        /* start from the previous element instead from the root */
        elem = next = *prev;
        pos = *prev_pos;
        for (top_sibling = elem; top_sibling->parent; top_sibling = top_sibling->parent);
        goto dfs_search;
    }

    LLLY_TREE_FOR(root, top_sibling) {
        /* TREE DFS */
        LLLY_TREE_DFS_BEGIN(top_sibling, next, elem) {
dfs_search:
            if ((root_type == LLLYXP_NODE_ROOT_CONFIG) && (elem->schema->flags & LLLYS_CONFIG_R)) {
                goto skip_children;
            }

            if (elem == node) {
                break;
            }
            ++pos;

            /* TREE DFS END */
            /* select element for the next run - children first,
             * child exception for lllyd_node_leaf and lllyd_node_leaflist, but not the root */
            if (elem->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA)) {
                next = NULL;
            } else {
                next = elem->child;
            }
            if (!next) {
skip_children:
                /* no children */
                if (elem == top_sibling) {
                    /* we are done, root has no children */
                    elem = NULL;
                    break;
                }
                /* try siblings */
                next = elem->next;
            }
            while (!next) {
                /* no siblings, go back through parents */
                if (elem->parent == top_sibling->parent) {
                    /* we are done, no next element to process */
                    elem = NULL;
                    break;
                }
                /* parent is already processed, go to its sibling */
                elem = elem->parent;
                next = elem->next;
            }
        }

        /* node found */
        if (elem) {
            break;
        }
    }

    if (!elem) {
        if (!(*prev)) {
            /* we went from root and failed to find it, cannot be */
            LOGINT(node->schema->module->ctx);
            return 0;
        } else {
            /* node is before prev, we assumed otherwise :( */
            //LOGDBG(LLLY_LDGXPATH, "get_node_pos optimalization fail.");

            *prev = NULL;
            *prev_pos = 0;

            elem = next = top_sibling = root;
            pos = 1;
            goto dfs_search;
        }
    }

    /*if (*prev) {
        LOGDBG(LLLY_LDGXPATH, "get_node_pos optimalization success.");
    }*/

    /* remember the last found node for next time */
    *prev = node;
    *prev_pos = pos;

    return pos;
}

/**
 * @brief Assign (fill) missing node positions.
 *
 * @param[in] set Set to fill positions in.
 * @param[in] root Context root node.
 * @param[in] root_type Context root type.
 *
 * @return 0 on success, -1 on error.
 */
static int
set_assign_pos(struct lllyxp_set *set, const struct lllyd_node *root, enum lllyxp_node_type root_type)
{
    const struct lllyd_node *prev = NULL, *tmp_node;
    uint32_t i, tmp_pos = 0;

    for (i = 0; i < set->used; ++i) {
        if (!set->val.nodes[i].pos) {
            tmp_node = NULL;
            switch (set->val.nodes[i].type) {
            case LLLYXP_NODE_ATTR:
                tmp_node = lllyd_attr_parent(root, set->val.attrs[i].attr);
                if (!tmp_node) {
                    LOGINT(root->schema->module->ctx);
                    return -1;
                }
                /* fallthrough */
            case LLLYXP_NODE_ELEM:
            case LLLYXP_NODE_TEXT:
                if (!tmp_node) {
                    tmp_node = set->val.nodes[i].node;
                }
                set->val.nodes[i].pos = get_node_pos(tmp_node, set->val.nodes[i].type, root, root_type, &prev, &tmp_pos);
                break;
            default:
                /* all roots have position 0 */
                break;
            }
        }
    }

    return 0;
}

/**
 * @brief Get unique \p attr position in the parent attributes.
 *
 * @param[in] attr Attr to use.
 * @param[in] parent Parent of \p attr.
 *
 * @return Attribute position.
 */
static uint16_t
get_attr_pos(struct lllyd_attr *attr, const struct lllyd_node *parent)
{
    uint16_t pos = 0;
    struct lllyd_attr *attr2;

    for (attr2 = parent->attr; attr2 && (attr2 != attr); attr2 = attr2->next) {
        ++pos;
    }

    assert(attr2);
    return pos;
}

/**
 * @brief Compare 2 nodes in respect to XPath document order.
 *
 * @param[in] idx1 Index of the 1st node in \p set1.
 * @param[in] set1 Set with the 1st node on index \p idx1.
 * @param[in] idx2 Index of the 2nd node in \p set2.
 * @param[in] set2 Set with the 2nd node on index \p idx2.
 * @param[in] root Context root node.
 *
 * @return If 1st > 2nd returns 1, 1st == 2nd returns 0, and 1st < 2nd returns -1.
 */
static int
set_sort_compare(struct lllyxp_set_node *item1, struct lllyxp_set_node *item2,
                 const struct lllyd_node *root)
{
    const struct lllyd_node *tmp_node;
    uint32_t attr_pos1 = 0, attr_pos2 = 0;

    if (item1->pos < item2->pos) {
        return -1;
    }

    if (item1->pos > item2->pos) {
        return 1;
    }

    /* node positions are equal, the fun case */

    /* 1st ELEM - == - 2nd TEXT, 1st TEXT - == - 2nd ELEM */
    /* special case since text nodes are actually saved as their parents */
    if ((item1->node == item2->node) && (item1->type != item2->type)) {
        if (item1->type == LLLYXP_NODE_ELEM) {
            assert(item2->type == LLLYXP_NODE_TEXT);
            return -1;
        } else {
            assert((item1->type == LLLYXP_NODE_TEXT) && (item2->type == LLLYXP_NODE_ELEM));
            return 1;
        }
    }

    /* we need attr positions now */
    if (item1->type == LLLYXP_NODE_ATTR) {
        tmp_node = lllyd_attr_parent(root, (struct lllyd_attr *)item1->node);
        if (!tmp_node) {
            LOGINT(root->schema->module->ctx);
            return -1;
        }
        attr_pos1 = get_attr_pos((struct lllyd_attr *)item1->node, tmp_node);
    }
    if (item2->type == LLLYXP_NODE_ATTR) {
        tmp_node = lllyd_attr_parent(root, (struct lllyd_attr *)item2->node);
        if (!tmp_node) {
            LOGINT(root->schema->module->ctx);
            return -1;
        }
        attr_pos2 = get_attr_pos((struct lllyd_attr *)item2->node, tmp_node);
    }

    /* 1st ROOT - 2nd ROOT, 1st ELEM - 2nd ELEM, 1st TEXT - 2nd TEXT, 1st ATTR - =pos= - 2nd ATTR */
    /* check for duplicates */
    if (item1->node == item2->node) {
        assert((item1->type == item2->type) && ((item1->type != LLLYXP_NODE_ATTR) || (attr_pos1 == attr_pos2)));
        return 0;
    }

    /* 1st ELEM - 2nd TEXT, 1st ELEM - any pos - 2nd ATTR */
    /* elem is always first, 2nd node is after it */
    if (item1->type == LLLYXP_NODE_ELEM) {
        assert(item2->type != LLLYXP_NODE_ELEM);
        return -1;
    }

    /* 1st TEXT - 2nd ELEM, 1st TEXT - any pos - 2nd ATTR, 1st ATTR - any pos - 2nd ELEM, 1st ATTR - >pos> - 2nd ATTR */
    /* 2nd is before 1st */
    if (((item1->type == LLLYXP_NODE_TEXT)
            && ((item2->type == LLLYXP_NODE_ELEM) || (item2->type == LLLYXP_NODE_ATTR)))
            || ((item1->type == LLLYXP_NODE_ATTR) && (item2->type == LLLYXP_NODE_ELEM))
            || (((item1->type == LLLYXP_NODE_ATTR) && (item2->type == LLLYXP_NODE_ATTR))
            && (attr_pos1 > attr_pos2))) {
        return 1;
    }

    /* 1st ATTR - any pos - 2nd TEXT, 1st ATTR <pos< - 2nd ATTR */
    /* 2nd is after 1st */
    return -1;
}

static int
set_comp_cast(struct lllyxp_set *trg, struct lllyxp_set *src, enum lllyxp_set_type type, const struct lllyd_node *cur_node,
              const struct lllys_module *local_mod, uint32_t src_idx, int options)
{
    assert(src->type == LLLYXP_SET_NODE_SET);

    memset(trg, 0, sizeof *trg);

    /* insert node into target set */
    set_insert_node(trg, src->val.nodes[src_idx].node, src->val.nodes[src_idx].pos, src->val.nodes[src_idx].type, 0);

    /* cast target set appropriately */
    if (lllyxp_set_cast(trg, type, cur_node, local_mod, options)) {
        set_free_content(trg);
        return -1;
    }

    return EXIT_SUCCESS;
}

#ifndef NDEBUG

/**
 * @brief Bubble sort \p set into XPath document order.
 *        Context position aware. Unused in the 'Release' build target.
 *
 * @param[in] set Set to sort.
 * @param[in] cur_node Original context node.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return How many times the whole set was traversed - 1 (if set was sorted, returns 0).
 */
static int
set_sort(struct lllyxp_set *set, const struct lllyd_node *cur_node, int options)
{
    uint32_t i, j;
    int ret = 0, cmp, inverted, change;
    const struct lllyd_node *root;
    enum lllyxp_node_type root_type;
    struct lllyxp_set_node item;

    if ((set->type != LLLYXP_SET_NODE_SET) || (set->used == 1)) {
        return 0;
    }

    /* get root */
    root = moveto_get_root(cur_node, options, &root_type);

    /* fill positions */
    if (set_assign_pos(set, root, root_type)) {
        return -1;
    }

    LOGDBG(LLLY_LDGXPATH, "SORT BEGIN");
    print_set_debug(set);

    for (i = 0; i < set->used; ++i) {
        inverted = 0;
        change = 0;

        for (j = 1; j < set->used - i; ++j) {
            /* compare node positions */
            if (inverted) {
                cmp = set_sort_compare(&set->val.nodes[j], &set->val.nodes[j - 1], root);
            } else {
                cmp = set_sort_compare(&set->val.nodes[j - 1], &set->val.nodes[j], root);
            }

            /* swap if needed */
            if ((inverted && (cmp < 0)) || (!inverted && (cmp > 0))) {
                change = 1;

                item = set->val.nodes[j - 1];
                set->val.nodes[j - 1] = set->val.nodes[j];
                set->val.nodes[j] = item;
            } else {
                /* whether node_pos1 should be smaller than node_pos2 or the other way around */
                inverted = !inverted;
            }
        }

        ++ret;

        if (!change) {
            break;
        }
    }

    LOGDBG(LLLY_LDGXPATH, "SORT END %d", ret);
    print_set_debug(set);

#ifdef LLLY_ENABLED_CACHE
    struct lllyxp_set_hash_node hnode;
    uint64_t hash;

    /* check node hashes */
    if (set->used >= LLLY_CACHE_HT_MIN_CHILDREN) {
        assert(set->ht);
        for (i = 0; i < set->used; ++i) {
            hnode.node = set->val.nodes[i].node;
            hnode.type = set->val.nodes[i].type;

            hash = dict_hash_multi(0, (const char *)&hnode.node, sizeof hnode.node);
            hash = dict_hash_multi(hash, (const char *)&hnode.type, sizeof hnode.type);
            hash = dict_hash_multi(hash, NULL, 0);

            assert(!lllyht_find(set->ht, &hnode, hash, NULL));
        }
    }
#endif

    return ret - 1;
}

/**
 * @brief Remove duplicate entries in a sorted node set.
 *
 * @param[in] set Sorted set to check.
 *
 * @return EXIT_SUCCESS if no duplicates were found,
 *         EXIT_FAILURE otherwise.
 */
static int
set_sorted_dup_node_clean(struct lllyxp_set *set)
{
    uint32_t i = 0;
    int ret = EXIT_SUCCESS;

    if (set->used > 1) {
        while (i < set->used - 1) {
            if ((set->val.nodes[i].node == set->val.nodes[i + 1].node)
                    && (set->val.nodes[i].type == set->val.nodes[i + 1].type)) {
                set_remove_node(set, i + 1);
            ret = EXIT_FAILURE;
            } else {
                ++i;
            }
        }
    }

    return ret;
}

#endif

/**
 * @brief Merge 2 sorted sets into one.
 *
 * @param[in,out] trg Set to merge into. Duplicates are removed.
 * @param[in] src Set to be merged into \p trg. It is cast to #LLLYXP_SET_EMPTY on success.
 * @param[in] cur_node Original context node.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return 0 on success, -1 on error.
 */
static int
set_sorted_merge(struct lllyxp_set *trg, struct lllyxp_set *src, struct lllyd_node *cur_node, int options)
{
    uint32_t i, j, count, dup_count;
    int cmp;
    const struct lllyd_node *root;
    enum lllyxp_node_type root_type;

    if (((trg->type != LLLYXP_SET_NODE_SET) && (trg->type != LLLYXP_SET_EMPTY))
            || ((src->type != LLLYXP_SET_NODE_SET) && (src->type != LLLYXP_SET_EMPTY))) {
        return -1;
    }

    if (src->type == LLLYXP_SET_EMPTY) {
        return 0;
    } else if (trg->type == LLLYXP_SET_EMPTY) {
        set_fill_set(trg, src);
        lllyxp_set_cast(src, LLLYXP_SET_EMPTY, cur_node, NULL, options);
        return 0;
    }

    /* get root */
    root = moveto_get_root(cur_node, options, &root_type);

    /* fill positions */
    if (set_assign_pos(trg, root, root_type) || set_assign_pos(src, root, root_type)) {
        return -1;
    }

#ifndef NDEBUG
    LOGDBG(LLLY_LDGXPATH, "MERGE target");
    print_set_debug(trg);
    LOGDBG(LLLY_LDGXPATH, "MERGE source");
    print_set_debug(src);
#endif

    /* make memory for the merge (duplicates are not detected yet, so space
     * will likely be wasted on them, too bad) */
    if (trg->size - trg->used < src->used) {
        trg->size = trg->used + src->used;

        trg->val.nodes = llly_realloc(trg->val.nodes, trg->size * sizeof *trg->val.nodes);
        LLLY_CHECK_ERR_RETURN(!trg->val.nodes, LOGMEM(cur_node->schema->module->ctx), -1);
    }

    i = 0;
    j = 0;
    count = 0;
    dup_count = 0;
    do {
        cmp = set_sort_compare(&src->val.nodes[i], &trg->val.nodes[j], root);
        if (!cmp) {
            if (!count) {
                /* duplicate, just skip it */
                ++i;
                ++j;
            } else {
                /* we are copying something already, so let's copy the duplicate too,
                 * we are hoping that afterwards there are some more nodes to
                 * copy and this way we can copy them all together */
                ++count;
                ++dup_count;
                ++i;
                ++j;
            }
        } else if (cmp < 0) {
            /* inserting src node into trg, just remember it for now */
            ++count;
            ++i;

#ifdef LLLY_ENABLED_CACHE
            /* insert the hash now */
            set_insert_node_hash(trg, src->val.nodes[i - 1].node, src->val.nodes[i - 1].type);
#endif
        } else if (count) {
copy_nodes:
            /* time to actually copy the nodes, we have found the largest block of nodes */
            memmove(&trg->val.nodes[j + (count - dup_count)],
                    &trg->val.nodes[j],
                    (trg->used - j) * sizeof *trg->val.nodes);
            memcpy(&trg->val.nodes[j - dup_count], &src->val.nodes[i - count], count * sizeof *src->val.nodes);

            trg->used += count - dup_count;
            /* do not change i, except the copying above, we are basically doing exactly what is in the else branch below */
            j += count - dup_count;

            count = 0;
            dup_count = 0;
        } else {
            ++j;
        }
    } while ((i < src->used) && (j < trg->used));

    if ((i < src->used) || count) {
#ifdef LLLY_ENABLED_CACHE
        uint32_t k;

        /* insert all the hashes first */
        for (k = i; k < src->used; ++k) {
            set_insert_node_hash(trg, src->val.nodes[k].node, src->val.nodes[k].type);
        }
#endif
        /* loop ended, but we need to copy something at trg end */
        count += src->used - i;
        i = src->used;
        goto copy_nodes;
    }

#ifdef LLLY_ENABLED_CACHE
    /* we are inserting hashes before the actual node insert, which causes
     * situations when there were initially not enough items for a hash table,
     * but even after some were inserted, hash table was not created (during
     * insertion the number of items is not updated yet) */
    if (!trg->ht && (trg->used >= LLLY_CACHE_HT_MIN_CHILDREN)) {
        set_insert_node_hash(trg, NULL, 0);
    }
#endif

#ifndef NDEBUG
    LOGDBG(LLLY_LDGXPATH, "MERGE result");
    print_set_debug(trg);
#endif

    lllyxp_set_cast(src, LLLYXP_SET_EMPTY, cur_node, NULL, options);
    return 0;
}

/**
 * @brief Canonize value in the set (can be string or number).
 *
 * @param[in] set Set to canonize.
 * @param[in] set2 Set to canonize for.
 * @param[in] schema Schema node to read YANG canonization rules from.
 *
 * @return 0 on succes, -1 on error.
 */
static int
set_canonize(struct lllyxp_set *set, const struct lllyxp_set *set2)
{
    char *num_str, *val_can, *ptr;
    struct lllys_node *schema;
    enum int_log_opts prev_ilo;

    assert(set2->type == LLLYXP_SET_NODE_SET);

    if ((set2->val.nodes[0].type == LLLYXP_NODE_ELEM) && (set2->val.nodes[0].node->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
        schema = set2->val.nodes[0].node->schema;
    } else {
        /* nothing to canonize/not supported */
        return 0;
    }

    switch (set->type) {
    case LLLYXP_SET_NUMBER:
        /* canonize number */
        if (asprintf(&num_str, "%Lf", set->val.num) == -1) {
            LOGMEM(schema->module->ctx);
            return -1;
        }

        /* ignore errors, the value may not satisfy schema constraints */
        llly_ilo_change(NULL, ILO_IGNORE, &prev_ilo, NULL);
        val_can = lllyd_make_canonical(schema, num_str, strlen(num_str));
        llly_ilo_restore(NULL, prev_ilo, NULL, 0);

        free(num_str);
        if (!val_can) {
            break;
        }
        set->val.num = strtold(val_can, &ptr);
        if (ptr[0]) {
            free(val_can);
            LOGINT(schema->module->ctx);
            return -1;
        }
        free(val_can);
        break;
    case LLLYXP_SET_STRING:
        /* canonize string */
        llly_ilo_change(NULL, ILO_IGNORE, &prev_ilo, NULL);
        val_can = lllyd_make_canonical(schema, set->val.str, strlen(set->val.str));
        llly_ilo_restore(NULL, prev_ilo, NULL, 0);
        if (!val_can) {
            break;
        }
        free(set->val.str);
        set->val.str = val_can;
        break;
    case LLLYXP_SET_BOOLEAN:
        /* always canonical */
        break;
    default:
        LOGINT(schema->module->ctx);
        return -1;
    }

    return 0;
}

/*
 * (re)parse functions
 *
 * Parse functions parse the expression into
 * tokens (syntactic analysis).
 *
 * Reparse functions perform semantic analysis
 * (do not save the result, just a check) of
 * the expression and fill repeat indices.
 */

/**
 * @brief Add \p token into the expression \p exp.
 *
 * @param[in] exp Expression to use.
 * @param[in] token Token to add.
 * @param[in] expr_pos Token position in the XPath expression.
 * @param[in] tok_len Token length in the XPath expression.
 * @return 0 on success, -1 on error.
 */
static int
exp_add_token(struct lllyxp_expr *exp, enum lllyxp_token token, uint16_t expr_pos, uint16_t tok_len)
{
    uint32_t prev;

    if (exp->used == exp->size) {
        prev = exp->size;
        exp->size += LLLYXP_EXPR_SIZE_STEP;
        if (prev > exp->size) {
            LOGINT(NULL);
            return -1;
        }

        exp->tokens = llly_realloc(exp->tokens, exp->size * sizeof *exp->tokens);
        LLLY_CHECK_ERR_RETURN(!exp->tokens, LOGMEM(NULL), -1);
        exp->expr_pos = llly_realloc(exp->expr_pos, exp->size * sizeof *exp->expr_pos);
        LLLY_CHECK_ERR_RETURN(!exp->expr_pos, LOGMEM(NULL), -1);
        exp->tok_len = llly_realloc(exp->tok_len, exp->size * sizeof *exp->tok_len);
        LLLY_CHECK_ERR_RETURN(!exp->tok_len, LOGMEM(NULL), -1);
    }

    exp->tokens[exp->used] = token;
    exp->expr_pos[exp->used] = expr_pos;
    exp->tok_len[exp->used] = tok_len;
    ++exp->used;
    return 0;
}

/**
 * @brief Look at the next token and check its kind.
 *
 * @param[in] exp Expression to use.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] want_tok Expected token.
 * @param[in] strict Whether the token is strictly required (print error if
 * not the next one) or we simply want to check whether it is the next or not.
 *
 * @return EXIT_SUCCESS if the current token matches the expected one,
 *         -1 otherwise.
 */
static int
exp_check_token(struct llly_ctx *ctx, struct lllyxp_expr *exp, uint16_t exp_idx, enum lllyxp_token want_tok, int strict)
{
    if (exp->used == exp_idx) {
        if (strict) {
            LOGVAL(ctx, LLLYE_XPATH_EOF, LLLY_VLOG_NONE, NULL);
        }
        return -1;
    }

    if (want_tok && (exp->tokens[exp_idx] != want_tok)) {
        if (strict) {
            LOGVAL(ctx, LLLYE_XPATH_INTOK, LLLY_VLOG_NONE, NULL,
                   print_token(exp->tokens[exp_idx]), &exp->expr[exp->expr_pos[exp_idx]]);
        }
        return -1;
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Stack operation push on the repeat array.
 *
 * @param[in] exp Expression to use.
 * @param[in] exp_idx Position in the expresion \p exp.
 * @param[in] repeat_op_idx Index from \p exp of the operator token. This value is pushed.
 */
static void
exp_repeat_push(struct lllyxp_expr *exp, uint16_t exp_idx, uint16_t repeat_op_idx)
{
    uint16_t i;

    if (exp->repeat[exp_idx]) {
        for (i = 0; exp->repeat[exp_idx][i]; ++i);
        exp->repeat[exp_idx] = realloc(exp->repeat[exp_idx], (i + 2) * sizeof *exp->repeat[exp_idx]);
        LLLY_CHECK_ERR_RETURN(!exp->repeat[exp_idx], LOGMEM(NULL), );
        exp->repeat[exp_idx][i] = repeat_op_idx;
        exp->repeat[exp_idx][i + 1] = 0;
    } else {
        exp->repeat[exp_idx] = calloc(2, sizeof *exp->repeat[exp_idx]);
        LLLY_CHECK_ERR_RETURN(!exp->repeat[exp_idx], LOGMEM(NULL), );
        exp->repeat[exp_idx][0] = repeat_op_idx;
    }
}

/**
 * @brief Reparse Predicate. Logs directly on error.
 *
 * [7] Predicate ::= '[' Expr ']'
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
reparse_predicate(struct llly_ctx *ctx, struct lllyxp_expr *exp, uint16_t *exp_idx)
{
    if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_BRACK1, 1)) {
        return -1;
    }
    ++(*exp_idx);

    if (reparse_or_expr(ctx, exp, exp_idx)) {
        return -1;
    }

    if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_BRACK2, 1)) {
        return -1;
    }
    ++(*exp_idx);

    return EXIT_SUCCESS;
}

/**
 * @brief Reparse RelativeLocationPath. Logs directly on error.
 *
 * [4] RelativeLocationPath ::= Step | RelativeLocationPath '/' Step | RelativeLocationPath '//' Step
 * [5] Step ::= '@'? NodeTest Predicate* | '.' | '..'
 * [6] NodeTest ::= NameTest | NodeType '(' ')'
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on forward reference, -1 on error.
 */
static int
reparse_relative_location_path(struct llly_ctx *ctx, struct lllyxp_expr *exp, uint16_t *exp_idx)
{
    if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_NONE, 1)) {
        return -1;
    }

    goto step;
    do {
        /* '/' or '//' */
        ++(*exp_idx);

        if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_NONE, 1)) {
            return -1;
        }
step:
        /* Step */
        switch (exp->tokens[*exp_idx]) {
        case LLLYXP_TOKEN_DOT:
            ++(*exp_idx);
            break;

        case LLLYXP_TOKEN_DDOT:
            ++(*exp_idx);
            break;

        case LLLYXP_TOKEN_AT:
            ++(*exp_idx);

            if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_NONE, 1)) {
                return -1;
            }
            if ((exp->tokens[*exp_idx] != LLLYXP_TOKEN_NAMETEST) && (exp->tokens[*exp_idx] != LLLYXP_TOKEN_NODETYPE)) {
                LOGVAL(ctx, LLLYE_XPATH_INTOK, LLLY_VLOG_NONE, NULL,
                       print_token(exp->tokens[*exp_idx]), &exp->expr[exp->expr_pos[*exp_idx]]);
                return -1;
            }
            /* fall through */
        case LLLYXP_TOKEN_NAMETEST:
            ++(*exp_idx);
            goto reparse_predicate;
            break;

        case LLLYXP_TOKEN_NODETYPE:
            ++(*exp_idx);

            /* '(' */
            if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_PAR1, 1)) {
                return -1;
            }
            ++(*exp_idx);

            /* ')' */
            if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_PAR2, 1)) {
                return -1;
            }
            ++(*exp_idx);

reparse_predicate:
            /* Predicate* */
            while ((exp->used > *exp_idx) && (exp->tokens[*exp_idx] == LLLYXP_TOKEN_BRACK1)) {
                if (reparse_predicate(ctx, exp, exp_idx)) {
                    return -1;
                }
            }
            break;
        default:
            LOGVAL(ctx, LLLYE_XPATH_INTOK, LLLY_VLOG_NONE, NULL,
                   print_token(exp->tokens[*exp_idx]), &exp->expr[exp->expr_pos[*exp_idx]]);
            return -1;
        }
    } while ((exp->used > *exp_idx) && (exp->tokens[*exp_idx] == LLLYXP_TOKEN_OPERATOR_PATH));

    return EXIT_SUCCESS;
}

/**
 * @brief Reparse AbsoluteLocationPath. Logs directly on error.
 *
 * [3] AbsoluteLocationPath ::= '/' RelativeLocationPath? | '//' RelativeLocationPath
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
reparse_absolute_location_path(struct llly_ctx *ctx, struct lllyxp_expr *exp, uint16_t *exp_idx)
{
    if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_OPERATOR_PATH, 1)) {
        return -1;
    }

    /* '/' RelativeLocationPath? */
    if (exp->tok_len[*exp_idx] == 1) {
        /* '/' */
        ++(*exp_idx);

        if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_NONE, 0)) {
            return EXIT_SUCCESS;
        }
        switch (exp->tokens[*exp_idx]) {
        case LLLYXP_TOKEN_DOT:
        case LLLYXP_TOKEN_DDOT:
        case LLLYXP_TOKEN_AT:
        case LLLYXP_TOKEN_NAMETEST:
        case LLLYXP_TOKEN_NODETYPE:
            if (reparse_relative_location_path(ctx, exp, exp_idx)) {
                return -1;
            }
            /* fall through */
        default:
            break;
        }

    /* '//' RelativeLocationPath */
    } else {
        /* '//' */
        ++(*exp_idx);

        if (reparse_relative_location_path(ctx, exp, exp_idx)) {
            return -1;
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Reparse FunctionCall. Logs directly on error.
 *
 * [9] FunctionCall ::= FunctionName '(' ( Expr ( ',' Expr )* )? ')'
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
reparse_function_call(struct llly_ctx *ctx, struct lllyxp_expr *exp, uint16_t *exp_idx)
{
    int min_arg_count = -1, max_arg_count, arg_count;
    uint16_t func_exp_idx;

    if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_FUNCNAME, 1)) {
        return -1;
    }
    func_exp_idx = *exp_idx;
    switch (exp->tok_len[*exp_idx]) {
    case 3:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "not", 3)) {
            min_arg_count = 1;
            max_arg_count = 1;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "sum", 3)) {
            min_arg_count = 1;
            max_arg_count = 1;
        }
        break;
    case 4:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "lang", 4)) {
            min_arg_count = 1;
            max_arg_count = 1;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "last", 4)) {
            min_arg_count = 0;
            max_arg_count = 0;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "name", 4)) {
            min_arg_count = 0;
            max_arg_count = 1;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "true", 4)) {
            min_arg_count = 0;
            max_arg_count = 0;
        }
        break;
    case 5:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "count", 5)) {
            min_arg_count = 1;
            max_arg_count = 1;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "false", 5)) {
            min_arg_count = 0;
            max_arg_count = 0;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "floor", 5)) {
            min_arg_count = 1;
            max_arg_count = 1;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "round", 5)) {
            min_arg_count = 1;
            max_arg_count = 1;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "deref", 5)) {
            min_arg_count = 1;
            max_arg_count = 1;
        }
        break;
    case 6:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "concat", 6)) {
            min_arg_count = 2;
            max_arg_count = INT_MAX;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "number", 6)) {
            min_arg_count = 0;
            max_arg_count = 1;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "string", 6)) {
            min_arg_count = 0;
            max_arg_count = 1;
        }
        break;
    case 7:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "boolean", 7)) {
            min_arg_count = 1;
            max_arg_count = 1;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "ceiling", 7)) {
            min_arg_count = 1;
            max_arg_count = 1;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "current", 7)) {
            min_arg_count = 0;
            max_arg_count = 0;
        }
        break;
    case 8:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "contains", 8)) {
            min_arg_count = 2;
            max_arg_count = 2;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "position", 8)) {
            min_arg_count = 0;
            max_arg_count = 0;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "re-match", 8)) {
            min_arg_count = 2;
            max_arg_count = 2;
        }
        break;
    case 9:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "substring", 9)) {
            min_arg_count = 2;
            max_arg_count = 3;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "translate", 9)) {
            min_arg_count = 3;
            max_arg_count = 3;
        }
        break;
    case 10:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "local-name", 10)) {
            min_arg_count = 0;
            max_arg_count = 1;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "enum-value", 10)) {
            min_arg_count = 1;
            max_arg_count = 1;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "bit-is-set", 10)) {
            min_arg_count = 2;
            max_arg_count = 2;
        }
        break;
    case 11:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "starts-with", 11)) {
            min_arg_count = 2;
            max_arg_count = 2;
        }
        break;
    case 12:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "derived-from", 12)) {
            min_arg_count = 2;
            max_arg_count = 2;
        }
        break;
    case 13:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "namespace-uri", 13)) {
            min_arg_count = 0;
            max_arg_count = 1;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "string-length", 13)) {
            min_arg_count = 0;
            max_arg_count = 1;
        }
        break;
    case 15:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "normalize-space", 15)) {
            min_arg_count = 0;
            max_arg_count = 1;
        } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "substring-after", 15)) {
            min_arg_count = 2;
            max_arg_count = 2;
        }
        break;
    case 16:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "substring-before", 16)) {
            min_arg_count = 2;
            max_arg_count = 2;
        }
        break;
    case 20:
        if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "derived-from-or-self", 20)) {
            min_arg_count = 2;
            max_arg_count = 2;
        }
        break;
    }
    if (min_arg_count == -1) {
        LOGVAL(ctx, LLLYE_XPATH_INFUNC, LLLY_VLOG_NONE, NULL, exp->tok_len[*exp_idx], &exp->expr[exp->expr_pos[*exp_idx]]);
        return -1;
    }
    ++(*exp_idx);

    /* '(' */
    if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_PAR1, 1)) {
        return -1;
    }
    ++(*exp_idx);

    /* ( Expr ( ',' Expr )* )? */
    arg_count = 0;
    if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_NONE, 1)) {
        return -1;
    }
    if (exp->tokens[*exp_idx] != LLLYXP_TOKEN_PAR2) {
        ++arg_count;
        if (reparse_or_expr(ctx, exp, exp_idx)) {
            return -1;
        }
    }
    while ((exp->used > *exp_idx) && (exp->tokens[*exp_idx] == LLLYXP_TOKEN_COMMA)) {
        ++(*exp_idx);

        ++arg_count;
        if (reparse_or_expr(ctx, exp, exp_idx)) {
            return -1;
        }
    }

    /* ')' */
    if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_PAR2, 1)) {
        return -1;
    }
    ++(*exp_idx);

    if ((arg_count < min_arg_count) || (arg_count > max_arg_count)) {
        LOGVAL(ctx, LLLYE_XPATH_INARGCOUNT, LLLY_VLOG_NONE, NULL, arg_count, exp->tok_len[func_exp_idx],
               &exp->expr[exp->expr_pos[func_exp_idx]]);
        return -1;
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Reparse PathExpr. Logs directly on error.
 *
 * [10] PathExpr ::= LocationPath | PrimaryExpr Predicate*
 *                 | PrimaryExpr Predicate* '/' RelativeLocationPath
 *                 | PrimaryExpr Predicate* '//' RelativeLocationPath
 * [2] LocationPath ::= RelativeLocationPath | AbsoluteLocationPath
 * [8] PrimaryExpr ::= '(' Expr ')' | Literal | Number | FunctionCall
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
reparse_path_expr(struct llly_ctx *ctx, struct lllyxp_expr *exp, uint16_t *exp_idx)
{
    if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_NONE, 1)) {
        return -1;
    }

    switch (exp->tokens[*exp_idx]) {
    case LLLYXP_TOKEN_PAR1:
        /* '(' Expr ')' Predicate* */
        ++(*exp_idx);

        if (reparse_or_expr(ctx, exp, exp_idx)) {
            return -1;
        }

        if (exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_PAR2, 1)) {
            return -1;
        }
        ++(*exp_idx);
        goto predicate;
        break;
    case LLLYXP_TOKEN_DOT:
    case LLLYXP_TOKEN_DDOT:
    case LLLYXP_TOKEN_AT:
    case LLLYXP_TOKEN_NAMETEST:
    case LLLYXP_TOKEN_NODETYPE:
        /* RelativeLocationPath */
        if (reparse_relative_location_path(ctx, exp, exp_idx)) {
            return -1;
        }
        break;
    case LLLYXP_TOKEN_FUNCNAME:
        /* FunctionCall */
        if (reparse_function_call(ctx, exp, exp_idx)) {
            return -1;
        }
        goto predicate;
        break;
    case LLLYXP_TOKEN_OPERATOR_PATH:
        /* AbsoluteLocationPath */
        if (reparse_absolute_location_path(ctx, exp, exp_idx)) {
            return -1;
        }
        break;
    case LLLYXP_TOKEN_LITERAL:
        /* Literal */
        ++(*exp_idx);
        goto predicate;
        break;
    case LLLYXP_TOKEN_NUMBER:
        /* Number */
        ++(*exp_idx);
        goto predicate;
        break;
    default:
        LOGVAL(ctx, LLLYE_XPATH_INTOK, LLLY_VLOG_NONE, NULL,
               print_token(exp->tokens[*exp_idx]), &exp->expr[exp->expr_pos[*exp_idx]]);
        return -1;
    }

    return EXIT_SUCCESS;

predicate:
    /* Predicate* */
    while ((exp->used > *exp_idx) && (exp->tokens[*exp_idx] == LLLYXP_TOKEN_BRACK1)) {
        if (reparse_predicate(ctx, exp, exp_idx)) {
            return -1;
        }
    }

    /* ('/' or '//') RelativeLocationPath */
    if ((exp->used > *exp_idx) && (exp->tokens[*exp_idx] == LLLYXP_TOKEN_OPERATOR_PATH)) {

        /* '/' or '//' */
        ++(*exp_idx);

        if (reparse_relative_location_path(ctx, exp, exp_idx)) {
            return -1;
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Reparse UnaryExpr. Logs directly on error.
 *
 * [17] UnaryExpr ::= UnionExpr | '-' UnaryExpr
 * [18] UnionExpr ::= PathExpr | UnionExpr '|' PathExpr
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
reparse_unary_expr(struct llly_ctx *ctx, struct lllyxp_expr *exp, uint16_t *exp_idx)
{
    uint16_t prev_exp;

    /* ('-')* */
    prev_exp = *exp_idx;
    while (!exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_OPERATOR_MATH, 0)
            && (exp->expr[exp->expr_pos[*exp_idx]] == '-')) {
        exp_repeat_push(exp, prev_exp, LLLYXP_EXPR_UNARY);
        ++(*exp_idx);
    }

    /* PathExpr */
    prev_exp = *exp_idx;
    if (reparse_path_expr(ctx, exp, exp_idx)) {
        return -1;
    }

    /* ('|' PathExpr)* */
    while (!exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_OPERATOR_UNI, 0)) {
        exp_repeat_push(exp, prev_exp, LLLYXP_EXPR_UNION);
        ++(*exp_idx);

        if (reparse_path_expr(ctx, exp, exp_idx)) {
            return -1;
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Reparse AdditiveExpr. Logs directly on error.
 *
 * [15] AdditiveExpr ::= MultiplicativeExpr
 *                     | AdditiveExpr '+' MultiplicativeExpr
 *                     | AdditiveExpr '-' MultiplicativeExpr
 * [16] MultiplicativeExpr ::= UnaryExpr
 *                     | MultiplicativeExpr '*' UnaryExpr
 *                     | MultiplicativeExpr 'div' UnaryExpr
 *                     | MultiplicativeExpr 'mod' UnaryExpr
 *
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
reparse_additive_expr(struct llly_ctx *ctx, struct lllyxp_expr *exp, uint16_t *exp_idx)
{
    uint16_t prev_add_exp, prev_mul_exp;

    prev_add_exp = *exp_idx;
    goto reparse_multiplicative_expr;

    /* ('+' / '-' MultiplicativeExpr)* */
    while (!exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_OPERATOR_MATH, 0)
            && ((exp->expr[exp->expr_pos[*exp_idx]] == '+') || (exp->expr[exp->expr_pos[*exp_idx]] == '-'))) {
        exp_repeat_push(exp, prev_add_exp, LLLYXP_EXPR_ADDITIVE);
        ++(*exp_idx);

reparse_multiplicative_expr:
        /* UnaryExpr */
        prev_mul_exp = *exp_idx;
        if (reparse_unary_expr(ctx, exp, exp_idx)) {
            return -1;
        }

        /* ('*' / 'div' / 'mod' UnaryExpr)* */
        while (!exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_OPERATOR_MATH, 0)
                && ((exp->expr[exp->expr_pos[*exp_idx]] == '*') || (exp->tok_len[*exp_idx] == 3))) {
            exp_repeat_push(exp, prev_mul_exp, LLLYXP_EXPR_MULTIPLICATIVE);
            ++(*exp_idx);

            if (reparse_unary_expr(ctx, exp, exp_idx)) {
                return -1;
            }
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Reparse EqualityExpr. Logs directly on error.
 *
 * [13] EqualityExpr ::= RelationalExpr | EqualityExpr '=' RelationalExpr
 *                     | EqualityExpr '!=' RelationalExpr
 * [14] RelationalExpr ::= AdditiveExpr
 *                       | RelationalExpr '<' AdditiveExpr
 *                       | RelationalExpr '>' AdditiveExpr
 *                       | RelationalExpr '<=' AdditiveExpr
 *                       | RelationalExpr '>=' AdditiveExpr
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
reparse_equality_expr(struct llly_ctx *ctx, struct lllyxp_expr *exp, uint16_t *exp_idx)
{
    uint16_t prev_eq_exp, prev_rel_exp;

    prev_eq_exp = *exp_idx;
    goto reparse_additive_expr;

    /* ('=' / '!=' RelationalExpr)* */
    while (!exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_OPERATOR_COMP, 0)
            && ((exp->expr[exp->expr_pos[*exp_idx]] == '=') || (exp->expr[exp->expr_pos[*exp_idx]] == '!'))) {
        exp_repeat_push(exp, prev_eq_exp, LLLYXP_EXPR_EQUALITY);
        ++(*exp_idx);

reparse_additive_expr:
        /* AdditiveExpr */
        prev_rel_exp = *exp_idx;
        if (reparse_additive_expr(ctx, exp, exp_idx)) {
            return -1;
        }

        /* ('<' / '>' / '<=' / '>=' AdditiveExpr)* */
        while (!exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_OPERATOR_COMP, 0)
                && ((exp->expr[exp->expr_pos[*exp_idx]] == '<') || (exp->expr[exp->expr_pos[*exp_idx]] == '>'))) {
            exp_repeat_push(exp, prev_rel_exp, LLLYXP_EXPR_RELATIONAL);
            ++(*exp_idx);

            if (reparse_additive_expr(ctx, exp, exp_idx)) {
                return -1;
            }
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Reparse OrExpr. Logs directly on error.
 *
 * [11] OrExpr ::= AndExpr | OrExpr 'or' AndExpr
 * [12] AndExpr ::= EqualityExpr | AndExpr 'and' EqualityExpr
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
reparse_or_expr(struct llly_ctx *ctx, struct lllyxp_expr *exp, uint16_t *exp_idx)
{
    uint16_t prev_or_exp, prev_and_exp;

    prev_or_exp = *exp_idx;
    goto reparse_equality_expr;

    /* ('or' AndExpr)* */
    while (!exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_OPERATOR_LOG, 0) && (exp->tok_len[*exp_idx] == 2)) {
        exp_repeat_push(exp, prev_or_exp, LLLYXP_EXPR_OR);
        ++(*exp_idx);

reparse_equality_expr:
        /* EqualityExpr */
        prev_and_exp = *exp_idx;
        if (reparse_equality_expr(ctx, exp, exp_idx)) {
            return -1;
        }

        /* ('and' EqualityExpr)* */
        while (!exp_check_token(ctx, exp, *exp_idx, LLLYXP_TOKEN_OPERATOR_LOG, 0) && (exp->tok_len[*exp_idx] == 3)) {
            exp_repeat_push(exp, prev_and_exp, LLLYXP_EXPR_AND);
            ++(*exp_idx);

            if (reparse_equality_expr(ctx, exp, exp_idx)) {
                return -1;
            }
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Parse NCName.
 *
 * @param[in] ncname Name to parse.
 *
 * @return Length of \p ncname valid characters.
 */
static uint16_t
parse_ncname(struct llly_ctx *ctx, const char *ncname)
{
    uint16_t parsed = 0;
    int uc;
    unsigned int size;

    uc = lllyxml_getutf8(ctx, &ncname[parsed], &size);
    if (!is_xmlnamestartchar(uc) || (uc == ':')) {
       return parsed;
    }

    do {
        parsed += size;
        if (!ncname[parsed]) {
            break;
        }
        uc = lllyxml_getutf8(ctx, &ncname[parsed], &size);
    } while (is_xmlnamechar(uc) && (uc != ':'));

    return parsed;
}

struct lllyxp_expr *
lllyxp_parse_expr(struct llly_ctx *ctx, const char *expr)
{
    struct lllyxp_expr *ret;
    uint16_t parsed = 0, tok_len, ncname_len;
    enum lllyxp_token tok_type;
    int prev_function_check = 0;

    if (strlen(expr) > UINT16_MAX) {
        LOGERR(ctx, LLLY_EINVAL, "XPath expression cannot be longer than %ud characters.", UINT16_MAX);
        return NULL;
    }

    /* init lllyxp_expr structure */
    ret = calloc(1, sizeof *ret);
    LLLY_CHECK_ERR_GOTO(!ret, LOGMEM(ctx), error);
    ret->expr = strdup(expr);
    LLLY_CHECK_ERR_GOTO(!ret->expr, LOGMEM(ctx), error);
    ret->used = 0;
    ret->size = LLLYXP_EXPR_SIZE_START;
    ret->tokens = malloc(ret->size * sizeof *ret->tokens);
    LLLY_CHECK_ERR_GOTO(!ret->tokens, LOGMEM(ctx), error);

    ret->expr_pos = malloc(ret->size * sizeof *ret->expr_pos);
    LLLY_CHECK_ERR_GOTO(!ret->expr_pos, LOGMEM(ctx), error);

    ret->tok_len = malloc(ret->size * sizeof *ret->tok_len);
    LLLY_CHECK_ERR_GOTO(!ret->tok_len, LOGMEM(ctx), error);

    while (is_xmlws(expr[parsed])) {
        ++parsed;
    }

    do {
        if (expr[parsed] == '(') {

            /* '(' */
            tok_len = 1;
            tok_type = LLLYXP_TOKEN_PAR1;

            if (prev_function_check && ret->used && (ret->tokens[ret->used - 1] == LLLYXP_TOKEN_NAMETEST)) {
                /* it is a NodeType/FunctionName after all */
                if (((ret->tok_len[ret->used - 1] == 4)
                        && (!strncmp(&expr[ret->expr_pos[ret->used - 1]], "node", 4)
                        || !strncmp(&expr[ret->expr_pos[ret->used - 1]], "text", 4))) ||
                        ((ret->tok_len[ret->used - 1] == 7)
                        && !strncmp(&expr[ret->expr_pos[ret->used - 1]], "comment", 7))) {
                    ret->tokens[ret->used - 1] = LLLYXP_TOKEN_NODETYPE;
                } else {
                    ret->tokens[ret->used - 1] = LLLYXP_TOKEN_FUNCNAME;
                }
                prev_function_check = 0;
            }

        } else if (expr[parsed] == ')') {

            /* ')' */
            tok_len = 1;
            tok_type = LLLYXP_TOKEN_PAR2;

        } else if (expr[parsed] == '[') {

            /* '[' */
            tok_len = 1;
            tok_type = LLLYXP_TOKEN_BRACK1;

        } else if (expr[parsed] == ']') {

            /* ']' */
            tok_len = 1;
            tok_type = LLLYXP_TOKEN_BRACK2;

        } else if (!strncmp(&expr[parsed], "..", 2)) {

            /* '..' */
            tok_len = 2;
            tok_type = LLLYXP_TOKEN_DDOT;

        } else if ((expr[parsed] == '.') && (!isdigit(expr[parsed + 1]))) {

            /* '.' */
            tok_len = 1;
            tok_type = LLLYXP_TOKEN_DOT;

        } else if (expr[parsed] == '@') {

            /* '@' */
            tok_len = 1;
            tok_type = LLLYXP_TOKEN_AT;

        } else if (expr[parsed] == ',') {

            /* ',' */
            tok_len = 1;
            tok_type = LLLYXP_TOKEN_COMMA;

        } else if (expr[parsed] == '\'') {

            /* Literal with ' */
            for (tok_len = 1; (expr[parsed + tok_len] != '\0') && (expr[parsed + tok_len] != '\''); ++tok_len);
            if (expr[parsed + tok_len] == '\0') {
                LOGVAL(ctx, LLLYE_XPATH_NOEND, LLLY_VLOG_NONE, NULL, expr[parsed], &expr[parsed]);
                goto error;
            }
            ++tok_len;
            tok_type = LLLYXP_TOKEN_LITERAL;

        } else if (expr[parsed] == '\"') {

            /* Literal with " */
            for (tok_len = 1; (expr[parsed + tok_len] != '\0') && (expr[parsed + tok_len] != '\"'); ++tok_len);
            if (expr[parsed + tok_len] == '\0') {
                LOGVAL(ctx, LLLYE_XPATH_NOEND, LLLY_VLOG_NONE, NULL, expr[parsed], &expr[parsed]);
                goto error;
            }
            ++tok_len;
            tok_type = LLLYXP_TOKEN_LITERAL;

        } else if ((expr[parsed] == '.') || (isdigit(expr[parsed]))) {

            /* Number */
            for (tok_len = 0; isdigit(expr[parsed + tok_len]); ++tok_len);
            if (expr[parsed + tok_len] == '.') {
                ++tok_len;
                for (; isdigit(expr[parsed + tok_len]); ++tok_len);
            }
            tok_type = LLLYXP_TOKEN_NUMBER;

        } else if (expr[parsed] == '/') {

            /* Operator '/', '//' */
            if (!strncmp(&expr[parsed], "//", 2)) {
                tok_len = 2;
            } else {
                tok_len = 1;
            }
            tok_type = LLLYXP_TOKEN_OPERATOR_PATH;

        } else if  (!strncmp(&expr[parsed], "!=", 2) || !strncmp(&expr[parsed], "<=", 2)
                || !strncmp(&expr[parsed], ">=", 2)) {

            /* Operator '!=', '<=', '>=' */
            tok_len = 2;
            tok_type = LLLYXP_TOKEN_OPERATOR_COMP;

        } else if (expr[parsed] == '|') {

            /* Operator '|' */
            tok_len = 1;
            tok_type = LLLYXP_TOKEN_OPERATOR_UNI;

        } else if ((expr[parsed] == '+') || (expr[parsed] == '-')) {

            /* Operator '+', '-' */
            tok_len = 1;
            tok_type = LLLYXP_TOKEN_OPERATOR_MATH;

        } else if ((expr[parsed] == '=') || (expr[parsed] == '<') || (expr[parsed] == '>')) {

            /* Operator '=', '<', '>' */
            tok_len = 1;
            tok_type = LLLYXP_TOKEN_OPERATOR_COMP;

        } else if (ret->used && (ret->tokens[ret->used - 1] != LLLYXP_TOKEN_AT)
                && (ret->tokens[ret->used - 1] != LLLYXP_TOKEN_PAR1)
                && (ret->tokens[ret->used - 1] != LLLYXP_TOKEN_BRACK1)
                && (ret->tokens[ret->used - 1] != LLLYXP_TOKEN_COMMA)
                && (ret->tokens[ret->used - 1] != LLLYXP_TOKEN_OPERATOR_LOG)
                && (ret->tokens[ret->used - 1] != LLLYXP_TOKEN_OPERATOR_COMP)
                && (ret->tokens[ret->used - 1] != LLLYXP_TOKEN_OPERATOR_MATH)
                && (ret->tokens[ret->used - 1] != LLLYXP_TOKEN_OPERATOR_UNI)
                && (ret->tokens[ret->used - 1] != LLLYXP_TOKEN_OPERATOR_PATH)) {

            /* Operator '*', 'or', 'and', 'mod', or 'div' */
            if (expr[parsed] == '*') {
                tok_len = 1;
                tok_type = LLLYXP_TOKEN_OPERATOR_MATH;

            } else if (!strncmp(&expr[parsed], "or", 2)) {
                tok_len = 2;
                tok_type = LLLYXP_TOKEN_OPERATOR_LOG;

            } else if (!strncmp(&expr[parsed], "and", 3)) {
                tok_len = 3;
                tok_type = LLLYXP_TOKEN_OPERATOR_LOG;

            } else if (!strncmp(&expr[parsed], "mod", 3) || !strncmp(&expr[parsed], "div", 3)) {
                tok_len = 3;
                tok_type = LLLYXP_TOKEN_OPERATOR_MATH;

            } else {
                LOGVAL(ctx, LLLYE_INCHAR, LLLY_VLOG_NONE, NULL, expr[parsed], &expr[parsed]);
                if (prev_function_check) {
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "Perhaps \"%.*s\" is supposed to be a function call.",
                           ret->tok_len[ret->used - 1], &ret->expr[ret->expr_pos[ret->used - 1]]);
                }
                goto error;
            }
        } else if (expr[parsed] == '*') {

            /* NameTest '*' */
            tok_len = 1;
            tok_type = LLLYXP_TOKEN_NAMETEST;

        } else {

            /* NameTest (NCName ':' '*' | QName) or NodeType/FunctionName */
            ncname_len = parse_ncname(ctx, &expr[parsed]);
            if (!ncname_len) {
                LOGVAL(ctx, LLLYE_INCHAR, LLLY_VLOG_NONE, NULL, expr[parsed], &expr[parsed]);
                goto error;
            }
            tok_len = ncname_len;

            if (expr[parsed + tok_len] == ':') {
                ++tok_len;
                if (expr[parsed + tok_len] == '*') {
                    ++tok_len;
                } else {
                    ncname_len = parse_ncname(ctx, &expr[parsed + tok_len]);
                    if (!ncname_len) {
                        LOGVAL(ctx, LLLYE_INCHAR, LLLY_VLOG_NONE, NULL, expr[parsed], &expr[parsed]);
                        goto error;
                    }
                    tok_len += ncname_len;
                }
                /* remove old flag to prevent ambiguities */
                prev_function_check = 0;
                tok_type = LLLYXP_TOKEN_NAMETEST;
            } else {
                /* there is no prefix so it can still be NodeType/FunctionName, we can't finally decide now */
                prev_function_check = 1;
                tok_type = LLLYXP_TOKEN_NAMETEST;
            }
        }

        /* store the token, move on to the next one */
        if (exp_add_token(ret, tok_type, parsed, tok_len)) {
            goto error;
        }
        parsed += tok_len;
        while (is_xmlws(expr[parsed])) {
            ++parsed;
        }

    } while (expr[parsed]);

    /* prealloc repeat */
    ret->repeat = calloc(ret->size, sizeof *ret->repeat);
    LLLY_CHECK_ERR_GOTO(!ret->repeat, LOGMEM(ctx), error);

    return ret;

error:
    lllyxp_expr_free(ret);
    return NULL;
}

/*
 * warn functions
 *
 * Warn functions check specific reasonable conditions for schema XPath
 * and print a warning if they are not satisfied.
 */

/**
 * @brief Get the last-added schema node that is currently in the context.
 *
 * @param[in] set Set to search in.
 *
 * @return Last-added schema context node, NULL if no node is in context.
 */
static struct lllys_node *
warn_get_snode_in_ctx(struct lllyxp_set *set)
{
    uint32_t i;

    if (!set || (set->type != LLLYXP_SET_SNODE_SET)) {
        return NULL;
    }

    i = set->used;
    do {
        --i;
        if (set->val.snodes[i].in_ctx == 1) {
            /* if there are more, simply return the first found (last added) */
            return set->val.snodes[i].snode;
        }
    } while (i);

    return NULL;
}

/**
 * @brief Test whether a type is numeric - integer type or decimal64.
 *
 * @return 1 if numeric, 0 otherwise.
 */
static int
warn_is_numeric_type(struct lllys_type *type)
{
    struct lllys_node *node;
    struct lllys_type *t = NULL;
    int found = 0, ret;

    switch (type->base) {
    case LLLY_TYPE_DEC64:
    case LLLY_TYPE_INT8:
    case LLLY_TYPE_UINT8:
    case LLLY_TYPE_INT16:
    case LLLY_TYPE_UINT16:
    case LLLY_TYPE_INT32:
    case LLLY_TYPE_UINT32:
    case LLLY_TYPE_INT64:
    case LLLY_TYPE_UINT64:
        return 1;
    case LLLY_TYPE_UNION:
        while ((t = lllyp_get_next_union_type(type, t, &found))) {
            found = 0;
            ret = warn_is_numeric_type(t);
            if (ret) {
                /* found a suitable type */
                return 1;
            }
        }
        /* did not find any suitable type */
        return 0;
    case LLLY_TYPE_LEAFREF:
        if (!type->info.lref.target) {
            /* we may be in a grouping (and not directly in a typedef) */
            assert(&((struct lllys_node_leaf *)type->parent)->type == type);
            for (node = ((struct lllys_node *)type->parent); node && (node->nodetype != LLLYS_GROUPING); node = node->parent);
            if (!node) {
                LOGINT(((struct lllys_node *)type->parent)->module->ctx);
            }
            return 0;
        }
        return warn_is_numeric_type(&type->info.lref.target->type);
    default:
        return 0;
    }
}

/**
 * @brief Test whether a type is string-like - no integers, decimal64 or binary.
 *
 * @return 1 if string, 0 otherwise.
 */
static int
warn_is_string_type(struct lllys_type *type)
{
    struct lllys_type *t = NULL;
    int found = 0, ret;

    switch (type->base) {
    case LLLY_TYPE_BITS:
    case LLLY_TYPE_ENUM:
    case LLLY_TYPE_IDENT:
    case LLLY_TYPE_INST:
    case LLLY_TYPE_STRING:
        return 1;
    case LLLY_TYPE_UNION:
        while ((t = lllyp_get_next_union_type(type, t, &found))) {
            found = 0;
            ret = warn_is_string_type(t);
            if (ret) {
                /* found a suitable type */
                return 1;
            }
        }
        /* did not find any suitable type */
        return 0;
    case LLLY_TYPE_LEAFREF:
        if (!type->info.lref.target) {
            /* we are in a grouping */
            return 0;
        }
        return warn_is_string_type(&type->info.lref.target->type);
    default:
        return 0;
    }
}

/**
 * @brief Test whether a type is one specific type.
 *
 * @return 1 if it is, 0 otherwise.
 */
static int
warn_is_specific_type(struct lllys_type *type, LLLY_DATA_TYPE base)
{
    struct lllys_type *t = NULL;
    int found = 0, ret;

    if (type->base == base) {
        return 1;
    } else if (type->base == LLLY_TYPE_UNION) {
        while ((t = lllyp_get_next_union_type(type, t, &found))) {
            found = 0;
            ret = warn_is_specific_type(t, base);
            if (ret) {
                /* found a suitable type */
                return 1;
            }
        }
        /* did not find any suitable type */
        return 0;
    } else if (type->base == LLLY_TYPE_LEAFREF) {
        if (!type->info.lref.target) {
            /* we are in a grouping */
            return 1;
        }
        return warn_is_specific_type(&type->info.lref.target->type, base);
    }

    return 0;
}

static struct lllys_type *
warn_is_equal_type_next_type(struct lllys_type *type, struct lllys_type *prev_type)
{
    int found = 0;

    switch (type->base) {
    case LLLY_TYPE_UNION:
        /* this can, unfortunately, return leafref */
        return lllyp_get_next_union_type(type, prev_type, &found);
    case LLLY_TYPE_LEAFREF:
        if (!type->info.lref.target) {
            /* we are in a grouping */
            return type;
        }
        return warn_is_equal_type_next_type(&type->info.lref.target->type, prev_type);
    default:
        if (prev_type) {
            assert(type == prev_type);
            return NULL;
        } else {
            return type;
        }
    }
}

/**
 * @brief Test whether 2 types have a common type.
 *
 * @return 1 if they do, 0 otherwise.
 */
static int
warn_is_equal_type(struct lllys_type *type1, struct lllys_type *type2)
{
    struct lllys_type *t1, *t2;

    t1 = NULL;
    while ((t1 = warn_is_equal_type_next_type(type1, t1))) {
        if (t1->base == LLLY_TYPE_LEAFREF) {
            /* we do not check unions with leafrefs, that is just too much... */
            return 1;
        }

        t2 = NULL;
        while ((t2 = warn_is_equal_type_next_type(type2, t2))) {
            if (t2->base == LLLY_TYPE_LEAFREF) {
                return 1;
            }

            if (t2->base == t1->base) {
                /* match found */
                return 1;
            }
        }
    }

    return 0;
}

/**
 * @brief Check both operands of comparison operators.
 *
 * @param[in] ctx Context for errors.
 * @param[in] set1 First operand set.
 * @param[in] set2 Second operand set.
 * @param[in] numbers_only Whether accept only numbers or other types are fine too (for '=' and '!=').
 * @param[in] expr Start of the expression to print with the warning.
 */
static void
warn_operands(struct llly_ctx *ctx, struct lllyxp_set *set1, struct lllyxp_set *set2, int numbers_only, const char *expr, uint16_t expr_pos)
{
    struct lllys_node_leaf *node1, *node2;
    int leaves = 1, warning = 0;

    node1 = (struct lllys_node_leaf *)warn_get_snode_in_ctx(set1);
    node2 = (struct lllys_node_leaf *)warn_get_snode_in_ctx(set2);

    if (!node1 && !node2) {
        /* no node-sets involved, nothing to do */
        return;
    }

    if (node1) {
        if (!(node1->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
            LOGWRN(ctx, "Node type %s \"%s\" used as operand.", strnodetype(node1->nodetype), node1->name);
            warning = 1;
            leaves = 0;
        } else if (numbers_only && !warn_is_numeric_type(&node1->type)) {
            LOGWRN(ctx, "Node \"%s\" is not of a numeric type, but used where it was expected.", node1->name);
            warning = 1;
        }
    }

    if (node2) {
        if (!(node2->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
            LOGWRN(ctx, "Node type %s \"%s\" used as operand.", strnodetype(node2->nodetype), node2->name);
            warning = 1;
            leaves = 0;
        } else if (numbers_only && !warn_is_numeric_type(&node2->type)) {
            LOGWRN(ctx, "Node \"%s\" is not of a numeric type, but used where it was expected.", node2->name);
            warning = 1;
        }
    }

    if (node1 && node2 && leaves && !numbers_only) {
        if ((warn_is_numeric_type(&node1->type) && !warn_is_numeric_type(&node2->type))
                || (!warn_is_numeric_type(&node1->type) && warn_is_numeric_type(&node2->type))
                || (!warn_is_numeric_type(&node1->type) && !warn_is_numeric_type(&node2->type)
                && !warn_is_equal_type(&node1->type, &node2->type))) {
            LOGWRN(ctx, "Incompatible types of operands \"%s\" and \"%s\" for comparison.", node1->name, node2->name);
            warning = 1;
        }
    }

    if (warning) {
        LOGWRN(ctx, "Previous warning generated by XPath subexpression[%u] \"%.20s\".", expr_pos, expr + expr_pos);
    }
}

/**
 * @brief Check that a value is valid for a leaf. If not applicable, does nothing.
 *
 * @param[in] ctx Context for errors.
 * @param[in] exp Parsed XPath expression.
 * @param[in] set Set with the leaf/leaf-list.
 * @param[in] val_exp Index of the value (literal/number) in \p exp.
 * @param[in] equal_exp Index of the start of the equality expression in \p exp.
 * @param[in] last_equal_exp Index of the end of the equality expression in \p exp.
 */
static void
warn_equality_value(struct llly_ctx *ctx, struct lllyxp_expr *exp, struct lllyxp_set *set, uint16_t val_exp, uint16_t equal_exp,
                    uint16_t last_equal_exp)
{
    struct lllys_node *snode;
    char *value;
    int ret;
    enum int_log_opts prev_ilo;

    if ((snode = warn_get_snode_in_ctx(set)) && (snode->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))
            && ((exp->tokens[val_exp] == LLLYXP_TOKEN_LITERAL) || (exp->tokens[val_exp] == LLLYXP_TOKEN_NUMBER))) {
        /* check that the node can have the specified value */
        if (exp->tokens[val_exp] == LLLYXP_TOKEN_LITERAL) {
            value = strndup(exp->expr + exp->expr_pos[val_exp] + 1, exp->tok_len[val_exp] - 2);
        } else {
            value = strndup(exp->expr + exp->expr_pos[val_exp], exp->tok_len[val_exp]);
        }
        if (!value) {
            LOGMEM(ctx);
            return;
        }

        if ((((struct lllys_node_leaf *)snode)->type.base == LLLY_TYPE_IDENT) && !strchr(value, ':')) {
            LOGWRN(ctx, "Identityref \"%s\" comparison with identity \"%s\" without prefix, consider adding"
                   " a prefix or best using \"derived-from(-or-self)()\" functions.", snode->name, value);
            LOGWRN(ctx, "Previous warning generated by XPath subexpression[%u] \"%.*s\".", exp->expr_pos[equal_exp],
                   (exp->expr_pos[last_equal_exp] - exp->expr_pos[equal_exp]) + exp->tok_len[last_equal_exp],
                   exp->expr + exp->expr_pos[equal_exp]);
        }

        /* we are unable to check identityref validity if this module (and any required imports) are not implemented */
        if ((((struct lllys_node_leaf *)snode)->type.base != LLLY_TYPE_IDENT) || lllys_node_module(snode)->implemented) {
            /* we want to print our message and more importantly a warning, not an error */
            llly_ilo_change(NULL, ILO_ERR2WRN, &prev_ilo, NULL);
            ret = lllyd_validate_value(snode, value);
            llly_ilo_restore(NULL, prev_ilo, NULL, 0);
            if (ret) {
                LOGWRN(ctx, "Previous warning generated by XPath subexpression[%u] \"%.*s\".", exp->expr_pos[equal_exp],
                    (exp->expr_pos[last_equal_exp] - exp->expr_pos[equal_exp]) + exp->tok_len[last_equal_exp],
                    exp->expr + exp->expr_pos[equal_exp]);
            }
        }
        free(value);
    }
}

/*
 * XPath functions
 */

/**
 * @brief Execute the YANG 1.1 bit-is-set(node-set, string) function. Returns LLLYXP_SET_BOOLEAN
 *        depending on whether the first node bit value from the second argument is set.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_bit_is_set(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node, struct lllys_module *local_mod,
                 struct lllyxp_set *set, int options)
{
    struct lllyd_node_leaf_list *leaf;
    struct lllys_node_leaf *sleaf;
    int i, bits_count, ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type != LLLYXP_SET_SNODE_SET) || !(sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s not a node-set as expected.", __func__);
            ret = EXIT_FAILURE;
        } else if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
            ret = EXIT_FAILURE;
        } else if (!warn_is_specific_type(&sleaf->type, LLLY_TYPE_BITS)) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of type \"bits\".", __func__, sleaf->name);
            ret = EXIT_FAILURE;
        }

        if ((args[1]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[1]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if ((args[0]->type != LLLYXP_SET_NODE_SET) && (args[0]->type != LLLYXP_SET_EMPTY)) {
        LOGVAL(local_mod->ctx, LLLYE_XPATH_INARGTYPE, LLLY_VLOG_NONE, NULL, 1, print_set_type(args[0]), "bit-is-set(node-set, string)");
        return -1;
    }
    if (lllyxp_set_cast(args[1], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }

    set_fill_boolean(set, 0);
    if (args[0]->type == LLLYXP_SET_NODE_SET) {
        leaf = (struct lllyd_node_leaf_list *)args[0]->val.nodes[0].node;
        if ((leaf->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))
                && (((struct lllys_node_leaf *)leaf->schema)->type.base == LLLY_TYPE_BITS)) {
            bits_count = ((struct lllys_node_leaf *)leaf->schema)->type.info.bits.count;
            for (i = 0; i < bits_count; ++i) {
                if (leaf->value.bit[i] && llly_strequal(leaf->value.bit[i]->name, args[1]->val.str, 0)) {
                    set_fill_boolean(set, 1);
                    break;
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath boolean(object) function. Returns LLLYXP_SET_BOOLEAN
 *        with the argument converted to boolean.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_boolean(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node, struct lllys_module *local_mod,
              struct lllyxp_set *set, int options)
{
    if (options & LLLYXP_SNODE_ALL) {
        set_snode_clear_ctx(set);
        return EXIT_SUCCESS;
    }

    lllyxp_set_cast(args[0], LLLYXP_SET_BOOLEAN, cur_node, local_mod, options);
    set_fill_set(set, args[0]);

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath ceiling(number) function. Returns LLLYXP_SET_NUMBER
 *        with the first argument rounded up to the nearest integer.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_ceiling(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node, struct lllys_module *local_mod,
              struct lllyxp_set *set, int options)
{
    struct lllys_node_leaf *sleaf;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type != LLLYXP_SET_SNODE_SET) || !(sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s not a node-set as expected.", __func__);
            ret = EXIT_FAILURE;
        } else if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
            ret = EXIT_FAILURE;
        } else if (!warn_is_specific_type(&sleaf->type, LLLY_TYPE_DEC64)) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of type \"decimal64\".", __func__, sleaf->name);
            ret = EXIT_FAILURE;
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if (lllyxp_set_cast(args[0], LLLYXP_SET_NUMBER, cur_node, local_mod, options)) {
        return -1;
    }
    if ((long long)args[0]->val.num != args[0]->val.num) {
        set_fill_number(set, ((long long)args[0]->val.num) + 1);
    } else {
        set_fill_number(set, args[0]->val.num);
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath concat(string, string, string*) function.
 *        Returns LLLYXP_SET_STRING with the concatenation of all the arguments.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_concat(struct lllyxp_set **args, uint16_t arg_count, struct lllyd_node *cur_node, struct lllys_module *local_mod,
             struct lllyxp_set *set, int options)
{
    uint16_t i;
    char *str = NULL;
    size_t used = 1;
    int ret = EXIT_SUCCESS;
    struct lllys_node_leaf *sleaf;

    if (options & LLLYXP_SNODE_ALL) {
        for (i = 0; i < arg_count; ++i) {
            if ((args[i]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[i]))) {
                if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                    LOGWRN(local_mod->ctx, "Argument #%u of %s is a %s node \"%s\".",
                           i + 1, __func__, strnodetype(sleaf->nodetype), sleaf->name);
                    ret = EXIT_FAILURE;
                } else if (!warn_is_string_type(&sleaf->type)) {
                    LOGWRN(local_mod->ctx, "Argument #%u of %s is node \"%s\", not of string-type.", i + 1, __func__, sleaf->name);
                    ret = EXIT_FAILURE;
                }
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    for (i = 0; i < arg_count; ++i) {
        if (lllyxp_set_cast(args[i], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
            free(str);
            return -1;
        }

        str = llly_realloc(str, (used + strlen(args[i]->val.str)) * sizeof(char));
        LLLY_CHECK_ERR_RETURN(!str, LOGMEM(local_mod->ctx), -1);
        strcpy(str + used - 1, args[i]->val.str);
        used += strlen(args[i]->val.str);
    }

    /* free, kind of */
    lllyxp_set_cast(set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    set->type = LLLYXP_SET_STRING;
    set->val.str = str;

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath contains(string, string) function.
 *        Returns LLLYXP_SET_BOOLEAN whether the second argument can
 *        be found in the first or not.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_contains(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node, struct lllys_module *local_mod,
               struct lllyxp_set *set, int options)
{
    struct lllys_node_leaf *sleaf;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }

        if ((args[1]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[1]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if (lllyxp_set_cast(args[0], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }
    if (lllyxp_set_cast(args[1], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }

    if (strstr(args[0]->val.str, args[1]->val.str)) {
        set_fill_boolean(set, 1);
    } else {
        set_fill_boolean(set, 0);
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath count(node-set) function. Returns LLLYXP_SET_NUMBER
 *        with the size of the node-set from the argument.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_count(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *UNUSED(cur_node),
            struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    struct lllys_node *snode = NULL;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type != LLLYXP_SET_SNODE_SET) || !(snode = warn_get_snode_in_ctx(args[0]))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s not a node-set as expected.", __func__);
            ret = EXIT_FAILURE;
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if (args[0]->type == LLLYXP_SET_EMPTY) {
        set_fill_number(set, 0);
        return EXIT_SUCCESS;
    }

    if (args[0]->type != LLLYXP_SET_NODE_SET) {
        LOGVAL(local_mod->ctx, LLLYE_XPATH_INARGTYPE, LLLY_VLOG_NONE, NULL, 1, print_set_type(args[0]), "count(node-set)");
        return -1;
    }

    set_fill_number(set, args[0]->used);
    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath current() function. Returns LLLYXP_SET_NODE_SET
 *        with the context with the initial node.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_current(struct lllyxp_set **args, uint16_t arg_count, struct lllyd_node *cur_node, struct lllys_module *local_mod,
              struct lllyxp_set *set, int options)
{
    if (arg_count || args) {
        LOGVAL(local_mod->ctx, LLLYE_XPATH_INARGCOUNT, LLLY_VLOG_NONE, NULL, arg_count, "current()");
        return -1;
    }

    if (options & LLLYXP_SNODE_ALL) {
        set_snode_clear_ctx(set);

        set_snode_insert_node(set, (struct lllys_node *)cur_node, LLLYXP_NODE_ELEM);
    } else {
        lllyxp_set_cast(set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);

        /* position is filled later */
        set_insert_node(set, cur_node, 0, LLLYXP_NODE_ELEM, 0);
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the YANG 1.1 deref(node-set) function. Returns LLLYXP_SET_NODE_SET with either
 *        leafref or instance-identifier target node(s).
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_deref(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node, struct lllys_module *local_mod,
            struct lllyxp_set *set, int options)
{
    struct lllyd_node_leaf_list *leaf;
    struct lllys_node_leaf *sleaf;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type != LLLYXP_SET_SNODE_SET) || !(sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s not a node-set as expected.", __func__);
            ret = EXIT_FAILURE;
        } else if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
            ret = EXIT_FAILURE;
        } else if (!warn_is_specific_type(&sleaf->type, LLLY_TYPE_LEAFREF) && !warn_is_specific_type(&sleaf->type, LLLY_TYPE_INST)) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of type \"leafref\" neither \"instance-identifier\".",
                   __func__, sleaf->name);
            ret = EXIT_FAILURE;
        }
        set_snode_clear_ctx(set);
        if ((ret == EXIT_SUCCESS) && (sleaf->type.base == LLLY_TYPE_LEAFREF)) {
            assert(sleaf->type.info.lref.target);
            set_snode_insert_node(set, (struct lllys_node *)sleaf->type.info.lref.target, LLLYXP_NODE_ELEM);
        }
        return ret;
    }

    if ((args[0]->type != LLLYXP_SET_NODE_SET) && (args[0]->type != LLLYXP_SET_EMPTY)) {
        LOGVAL(local_mod->ctx, LLLYE_XPATH_INARGTYPE, LLLY_VLOG_NONE, NULL, 1, print_set_type(args[0]), "deref(node-set)");
        return -1;
    }

    lllyxp_set_cast(set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    if (args[0]->type != LLLYXP_SET_EMPTY) {
        leaf = (struct lllyd_node_leaf_list *)args[0]->val.nodes[0].node;
        sleaf = (struct lllys_node_leaf *)leaf->schema;
        if ((sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))
                && ((sleaf->type.base == LLLY_TYPE_LEAFREF) || (sleaf->type.base == LLLY_TYPE_INST))) {
            if (leaf->value_flags & LLLY_VALUE_UNRES) {
                /* this is bad */
                LOGVAL(local_mod->ctx, LLLYE_SPEC, LLLY_VLOG_LYD, args[0]->val.nodes[0].node,
                       "Trying to dereference an unresolved leafref or instance-identifier.");
                return -1;
            }
            /* works for both leafref and instid */
            set_insert_node(set, leaf->value.leafref, 0, LLLYXP_NODE_ELEM, 0);
        }
    }

    return EXIT_SUCCESS;
}

/* return 0 - match, 1 - mismatch */
static int
xpath_derived_from_ident_cmp(struct lllys_ident *ident, const char *ident_str)
{
    const char *ptr;
    int len;

    ptr = strchr(ident_str, ':');
    if (ptr) {
        len = ptr - ident_str;
        if (strncmp(ident->module->name, ident_str, len)
                || ident->module->name[len]) {
            /* module name mismatch BUG we expect JSON format prefix, but if the 2nd argument was
             * not a literal, we may easily be mistaken */
            return 1;
        }
        ++ptr;
    } else {
        ptr = ident_str;
    }

    len = strlen(ptr);
    if (strncmp(ident->name, ptr, len) || ident->name[len]) {
        /* name mismatch */
        return 1;
    }

    return 0;
}

/**
 * @brief Execute the YANG 1.1 derived-from(node-set, string) function. Returns LLLYXP_SET_BOOLEAN depending
 *        on whether the first argument nodes contain a node of an identity derived from the second
 *        argument identity.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_derived_from(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node, struct lllys_module *local_mod,
                   struct lllyxp_set *set, int options)
{
    uint16_t i, j;
    struct lllyd_node_leaf_list *leaf;
    struct lllys_node_leaf *sleaf;
    lllyd_val *val;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type != LLLYXP_SET_SNODE_SET) || !(sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s not a node-set as expected.", __func__);
            ret = EXIT_FAILURE;
        } else if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
            ret = EXIT_FAILURE;
        } else if (!warn_is_specific_type(&sleaf->type, LLLY_TYPE_IDENT)) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of type \"identityref\".", __func__, sleaf->name);
            ret = EXIT_FAILURE;
        }

        if ((args[1]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[1]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if ((args[0]->type != LLLYXP_SET_NODE_SET) && (args[0]->type != LLLYXP_SET_EMPTY)) {
        LOGVAL(local_mod->ctx, LLLYE_XPATH_INARGTYPE, LLLY_VLOG_NONE, NULL, 1, print_set_type(args[0]), "derived-from(node-set, string)");
        return -1;
    }
    if (lllyxp_set_cast(args[1], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }

    set_fill_boolean(set, 0);
    if (args[0]->type != LLLYXP_SET_EMPTY) {
        for (i = 0; i < args[0]->used; ++i) {
            val = NULL;
            if (args[0]->val.nodes[i].type == LLLYXP_NODE_ELEM) {
                leaf = (struct lllyd_node_leaf_list *)args[0]->val.nodes[i].node;
                sleaf = (struct lllys_node_leaf *)leaf->schema;
                if ((sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) && (sleaf->type.base == LLLY_TYPE_IDENT)) {
                    val = &leaf->value;
                }
            } else if (args[0]->val.nodes[i].type == LLLYXP_NODE_ATTR) {
                if (args[0]->val.attrs[i].attr->value_type == LLLY_TYPE_IDENT) {
                    val = &args[0]->val.attrs[i].attr->value;
                }
            }
            if (val) {
                for (j = 0; j < val->ident->base_size; ++j) {
                    if (!xpath_derived_from_ident_cmp(val->ident->base[j], args[1]->val.str)) {
                        set_fill_boolean(set, 1);
                        break;
                    }
                }

                if (j < val->ident->base_size) {
                    break;
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the YANG 1.1 derived-from-or-self(node-set, string) function. Returns LLLYXP_SET_BOOLEAN depending
 *        on whether the first argument nodes contain a node of an identity that either is or is derived from
 *        the second argument identity.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_derived_from_or_self(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node,
                           struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    uint16_t i, j;
    struct lllyd_node_leaf_list *leaf;
    struct lllys_node_leaf *sleaf;
    lllyd_val *val;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type != LLLYXP_SET_SNODE_SET) || !(sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s not a node-set as expected.", __func__);
            ret = EXIT_FAILURE;
        } else if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
            ret = EXIT_FAILURE;
        } else if (!warn_is_specific_type(&sleaf->type, LLLY_TYPE_IDENT)) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of type \"identityref\".", __func__, sleaf->name);
            ret = EXIT_FAILURE;
        }

        if ((args[1]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[1]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if ((args[0]->type != LLLYXP_SET_NODE_SET) && (args[0]->type != LLLYXP_SET_EMPTY)) {
        LOGVAL(local_mod->ctx, LLLYE_XPATH_INARGTYPE, LLLY_VLOG_NONE, NULL, 1, print_set_type(args[0]), "derived-from-or-self(node-set, string)");
        return -1;
    }
    if (lllyxp_set_cast(args[1], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }

    set_fill_boolean(set, 0);
    if (args[0]->type != LLLYXP_SET_EMPTY) {
        for (i = 0; i < args[0]->used; ++i) {
            val = NULL;
            if (args[0]->val.nodes[i].type == LLLYXP_NODE_ELEM) {
                leaf = (struct lllyd_node_leaf_list *)args[0]->val.nodes[i].node;
                sleaf = (struct lllys_node_leaf *)leaf->schema;
                if ((sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) && (sleaf->type.base == LLLY_TYPE_IDENT)) {
                    val = &leaf->value;
                }
            } else if (args[0]->val.nodes[i].type == LLLYXP_NODE_ATTR) {
                if (args[0]->val.attrs[i].attr->value_type == LLLY_TYPE_IDENT) {
                    val = &args[0]->val.attrs[i].attr->value;
                }
            }
            if (val) {
                if (!xpath_derived_from_ident_cmp(val->ident, args[1]->val.str)) {
                    set_fill_boolean(set, 1);
                    break;
                }

                for (j = 0; j < val->ident->base_size; ++j) {
                    if (!xpath_derived_from_ident_cmp(val->ident->base[j], args[1]->val.str)) {
                        set_fill_boolean(set, 1);
                        break;
                    }
                }

                if (j < val->ident->base_size) {
                    break;
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the YANG 1.1 enum-value(node-set) function. Returns LLLYXP_SET_NUMBER
 *        with the integer value of the first node's enum value, otherwise NaN.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_enum_value(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *UNUSED(cur_node),
                 struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    struct lllyd_node_leaf_list *leaf;
    struct lllys_node_leaf *sleaf;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type != LLLYXP_SET_SNODE_SET) || !(sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s not a node-set as expected.", __func__);
            ret = EXIT_FAILURE;
        } else if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
            ret = EXIT_FAILURE;
        } else if (!warn_is_specific_type(&sleaf->type, LLLY_TYPE_ENUM)) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of type \"enumeration\".", __func__, sleaf->name);
            ret = EXIT_FAILURE;
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if ((args[0]->type != LLLYXP_SET_NODE_SET) && (args[0]->type != LLLYXP_SET_EMPTY)) {
        LOGVAL(local_mod->ctx, LLLYE_XPATH_INARGTYPE, LLLY_VLOG_NONE, NULL, 1, print_set_type(args[0]), "enum-value(node-set)");
        return -1;
    }

    set_fill_number(set, NAN);
    if (args[0]->type == LLLYXP_SET_NODE_SET) {
        leaf = (struct lllyd_node_leaf_list *)args[0]->val.nodes[0].node;
        if ((leaf->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))
                && (((struct lllys_node_leaf *)leaf->schema)->type.base == LLLY_TYPE_ENUM)) {
            set_fill_number(set, leaf->value.enm->value);
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath false() function. Returns LLLYXP_SET_BOOLEAN
 *        with false value.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_false(struct lllyxp_set **UNUSED(args), uint16_t UNUSED(arg_count), struct lllyd_node *UNUSED(cur_node),
            struct lllys_module *UNUSED(local_mod), struct lllyxp_set *set, int options)
{
    if (options & LLLYXP_SNODE_ALL) {
        set_snode_clear_ctx(set);
        return EXIT_SUCCESS;
    }

    set_fill_boolean(set, 0);
    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath floor(number) function. Returns LLLYXP_SET_NUMBER
 *        with the first argument floored (truncated).
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_floor(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node, struct lllys_module *local_mod,
            struct lllyxp_set *set, int options)
{
    if (lllyxp_set_cast(args[0], LLLYXP_SET_NUMBER, cur_node, local_mod, options)) {
        return -1;
    }
    if (isfinite(args[0]->val.num)) {
        set_fill_number(set, (long long)args[0]->val.num);
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath lang(string) function. Returns LLLYXP_SET_BOOLEAN
 *        whether the language of the text matches the one from the argument.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_lang(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node, struct lllys_module *local_mod,
           struct lllyxp_set *set, int options)
{
    const struct lllyd_node *node, *root;
    struct lllys_node_leaf *sleaf;
    struct lllyd_attr *attr = NULL;
    int i, ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if (lllyxp_set_cast(args[0], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }

    if (set->type == LLLYXP_SET_EMPTY) {
        set_fill_boolean(set, 0);
        return EXIT_SUCCESS;
    }
    if (set->type != LLLYXP_SET_NODE_SET) {
        LOGVAL(local_mod->ctx, LLLYE_XPATH_INCTX, LLLY_VLOG_NONE, NULL, print_set_type(set), "lang(string)");
        return -1;
    }

    switch (set->val.nodes[0].type) {
    case LLLYXP_NODE_ELEM:
    case LLLYXP_NODE_TEXT:
        node = set->val.nodes[0].node;
        break;
    case LLLYXP_NODE_ATTR:
        root = moveto_get_root(cur_node, options, NULL);
        node = lllyd_attr_parent(root, set->val.attrs[0].attr);
        break;
    default:
        /* nothing to do with roots */
        set_fill_boolean(set, 0);
        return EXIT_SUCCESS;
    }

    /* find lang attribute */
    for (; node; node = node->parent) {
        for (attr = node->attr; attr; attr = attr->next) {
            if (attr->name && !strcmp(attr->name, "lang") && !strcmp(attr->annotation->module->name, "xml")) {
                break;
            }
        }

        if (attr) {
            break;
        }
    }

    /* compare languages */
    if (!attr) {
        set_fill_boolean(set, 0);
    } else {
        for (i = 0; args[0]->val.str[i]; ++i) {
            if (tolower(args[0]->val.str[i]) != tolower(attr->value_str[i])) {
                set_fill_boolean(set, 0);
                break;
            }
        }
        if (!args[0]->val.str[i]) {
            if (!attr->value_str[i] || (attr->value_str[i] == '-')) {
                set_fill_boolean(set, 1);
            } else {
                set_fill_boolean(set, 0);
            }
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath last() function. Returns LLLYXP_SET_NUMBER
 *        with the context size.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_last(struct lllyxp_set **UNUSED(args), uint16_t UNUSED(arg_count), struct lllyd_node *UNUSED(cur_node),
           struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    if (options & LLLYXP_SNODE_ALL) {
        set_snode_clear_ctx(set);
        return EXIT_SUCCESS;
    }

    if (set->type == LLLYXP_SET_EMPTY) {
        set_fill_number(set, 0);
        return EXIT_SUCCESS;
    }
    if (set->type != LLLYXP_SET_NODE_SET) {
        LOGVAL(local_mod->ctx, LLLYE_XPATH_INCTX, LLLY_VLOG_NONE, NULL, print_set_type(set), "last()");
        return -1;
    }

    set_fill_number(set, set->ctx_size);
    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath local-name(node-set?) function. Returns LLLYXP_SET_STRING
 *        with the node name without namespace from the argument or the context.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_local_name(struct lllyxp_set **args, uint16_t arg_count, struct lllyd_node *cur_node, struct lllys_module *local_mod,
                 struct lllyxp_set *set, int options)
{
    struct lllyxp_set_node *item;
    /* suppress unused variable warning */
    (void)cur_node;
    (void)options;

    if (options & LLLYXP_SNODE_ALL) {
        set_snode_clear_ctx(set);
        return EXIT_SUCCESS;
    }

    if (arg_count) {
        if (args[0]->type == LLLYXP_SET_EMPTY) {
            set_fill_string(set, "", 0);
            return EXIT_SUCCESS;
        }
        if (args[0]->type != LLLYXP_SET_NODE_SET) {
            LOGVAL(local_mod->ctx, LLLYE_XPATH_INARGTYPE, LLLY_VLOG_NONE, NULL, 1, print_set_type(args[0]), "local-name(node-set?)");
            return -1;
        }

        /* we need the set sorted, it affects the result */
        assert(!set_sort(args[0], cur_node, options));

        item = &args[0]->val.nodes[0];
    } else {
        if (set->type == LLLYXP_SET_EMPTY) {
            set_fill_string(set, "", 0);
            return EXIT_SUCCESS;
        }
        if (set->type != LLLYXP_SET_NODE_SET) {
            LOGVAL(local_mod->ctx, LLLYE_XPATH_INCTX, LLLY_VLOG_NONE, NULL, print_set_type(set), "local-name(node-set?)");
            return -1;
        }

        /* we need the set sorted, it affects the result */
        assert(!set_sort(set, cur_node, options));

        item = &set->val.nodes[0];
    }

    switch (item->type) {
    case LLLYXP_NODE_ROOT:
    case LLLYXP_NODE_ROOT_CONFIG:
    case LLLYXP_NODE_TEXT:
        set_fill_string(set, "", 0);
        break;
    case LLLYXP_NODE_ELEM:
        set_fill_string(set, item->node->schema->name, strlen(item->node->schema->name));
        break;
    case LLLYXP_NODE_ATTR:
        set_fill_string(set, ((struct lllyd_attr *)item->node)->name, strlen(((struct lllyd_attr *)item->node)->name));
        break;
    default:
        LOGINT(local_mod->ctx);
        return -1;
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath name(node-set?) function. Returns LLLYXP_SET_STRING
 *        with the node name fully qualified (with namespace) from the argument or the context.
 *        !! This function does not follow its definition and actually copies what local-name()
 *           function does, for the ietf-ipfix-psamp module that uses it incorrectly. !!
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_name(struct lllyxp_set **args, uint16_t arg_count, struct lllyd_node *cur_node, struct lllys_module *local_mod,
           struct lllyxp_set *set, int options)
{
    return xpath_local_name(args, arg_count, cur_node, local_mod, set, options);
}

/**
 * @brief Execute the XPath namespace-uri(node-set?) function. Returns LLLYXP_SET_STRING
 *        with the namespace of the node from the argument or the context.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_namespace_uri(struct lllyxp_set **args, uint16_t arg_count, struct lllyd_node *cur_node, struct lllys_module *local_mod,
                    struct lllyxp_set *set, int options)
{
    struct lllyxp_set_node *item;
    struct lllys_module *module;
    /* suppress unused variable warning */
    (void)cur_node;
    (void)options;

    if (options & LLLYXP_SNODE_ALL) {
        set_snode_clear_ctx(set);
        return EXIT_SUCCESS;
    }

    if (arg_count) {
        if (args[0]->type == LLLYXP_SET_EMPTY) {
            set_fill_string(set, "", 0);
            return EXIT_SUCCESS;
        }
        if (args[0]->type != LLLYXP_SET_NODE_SET) {
            LOGVAL(local_mod->ctx, LLLYE_XPATH_INARGTYPE, LLLY_VLOG_NONE, NULL, 1, print_set_type(args[0]), "namespace-uri(node-set?)");
            return -1;
        }

        /* we need the set sorted, it affects the result */
        assert(!set_sort(args[0], cur_node, options));

        item = &args[0]->val.nodes[0];
    } else {
        if (set->type == LLLYXP_SET_EMPTY) {
            set_fill_string(set, "", 0);
            return EXIT_SUCCESS;
        }
        if (set->type != LLLYXP_SET_NODE_SET) {
            LOGVAL(local_mod->ctx, LLLYE_XPATH_INCTX, LLLY_VLOG_NONE, NULL, print_set_type(set), "namespace-uri(node-set?)");
            return -1;
        }

        /* we need the set sorted, it affects the result */
        assert(!set_sort(set, cur_node, options));

        item = &set->val.nodes[0];
    }

    switch (item->type) {
    case LLLYXP_NODE_ROOT:
    case LLLYXP_NODE_ROOT_CONFIG:
    case LLLYXP_NODE_TEXT:
        set_fill_string(set, "", 0);
        break;
    case LLLYXP_NODE_ELEM:
    case LLLYXP_NODE_ATTR:
        if (item->type == LLLYXP_NODE_ELEM) {
            module =  item->node->schema->module;
        } else { /* LLLYXP_NODE_ATTR */
            module = ((struct lllyd_attr *)item->node)->annotation->module;
        }

        module = lllys_main_module(module);

        set_fill_string(set, module->ns, strlen(module->ns));
        break;
    default:
        LOGINT(local_mod->ctx);
        return -1;
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath node() function (node type). Returns LLLYXP_SET_NODE_SET
 *        with only nodes from the context. In practice it either leaves the context
 *        as it is or returns an empty node set.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_node(struct lllyxp_set **UNUSED(args), uint16_t UNUSED(arg_count), struct lllyd_node *cur_node,
           struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    if (options & LLLYXP_SNODE_ALL) {
        set_snode_clear_ctx(set);
        return EXIT_SUCCESS;
    }

    if (set->type != LLLYXP_SET_NODE_SET) {
        lllyxp_set_cast(set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    }
    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath normalize-space(string?) function. Returns LLLYXP_SET_STRING
 *        with normalized value (no leading, trailing, double white spaces) of the node
 *        from the argument or the context.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_normalize_space(struct lllyxp_set **args, uint16_t arg_count, struct lllyd_node *cur_node, struct lllys_module *local_mod,
                      struct lllyxp_set *set, int options)
{
    uint16_t i, new_used;
    char *new;
    int have_spaces = 0, space_before = 0, ret = EXIT_SUCCESS;
    struct lllys_node_leaf *sleaf;

    if (options & LLLYXP_SNODE_ALL) {
        if (arg_count && (args[0]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if (arg_count) {
        set_fill_set(set, args[0]);
    }
    if (lllyxp_set_cast(set, LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }

    /* is there any normalization necessary? */
    for (i = 0; set->val.str[i]; ++i) {
        if (is_xmlws(set->val.str[i])) {
            if ((i == 0) || space_before || (!set->val.str[i + 1])) {
                have_spaces = 1;
                break;
            }
            space_before = 1;
        } else {
            space_before = 0;
        }
    }

    /* yep, there is */
    if (have_spaces) {
        /* it's enough, at least one character will go, makes space for ending '\0' */
        new = malloc(strlen(set->val.str) * sizeof(char));
        LLLY_CHECK_ERR_RETURN(!new, LOGMEM(local_mod->ctx), -1);
        new_used = 0;

        space_before = 0;
        for (i = 0; set->val.str[i]; ++i) {
            if (is_xmlws(set->val.str[i])) {
                if ((i == 0) || space_before) {
                    space_before = 1;
                    continue;
                } else {
                    space_before = 1;
                }
            } else {
                space_before = 0;
            }

            new[new_used] = (space_before ? ' ' : set->val.str[i]);
            ++new_used;
        }

        /* at worst there is one trailing space now */
        if (new_used && is_xmlws(new[new_used - 1])) {
            --new_used;
        }

        new = llly_realloc(new, (new_used + 1) * sizeof(char));
        LLLY_CHECK_ERR_RETURN(!new, LOGMEM(local_mod->ctx), -1);
        new[new_used] = '\0';

        free(set->val.str);
        set->val.str = new;
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath not(boolean) function. Returns LLLYXP_SET_BOOLEAN
 *        with the argument converted to boolean and logically inverted.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_not(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node, struct lllys_module *local_mod,
          struct lllyxp_set *set, int options)
{
    if (options & LLLYXP_SNODE_ALL) {
        set_snode_clear_ctx(set);
        return EXIT_SUCCESS;
    }

    lllyxp_set_cast(args[0], LLLYXP_SET_BOOLEAN, cur_node, local_mod, options);
    if (args[0]->val.bool) {
        set_fill_boolean(set, 0);
    } else {
        set_fill_boolean(set, 1);
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath number(object?) function. Returns LLLYXP_SET_NUMBER
 *        with the number representation of either the argument or the context.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_number(struct lllyxp_set **args, uint16_t arg_count, struct lllyd_node *cur_node, struct lllys_module *local_mod,
             struct lllyxp_set *set, int options)
{
    if (options & LLLYXP_SNODE_ALL) {
        set_snode_clear_ctx(set);
        return EXIT_SUCCESS;
    }

    if (arg_count) {
        if (lllyxp_set_cast(args[0], LLLYXP_SET_NUMBER, cur_node, local_mod, options)) {
            return -1;
        }
        set_fill_set(set, args[0]);
    } else {
        if (lllyxp_set_cast(set, LLLYXP_SET_NUMBER, cur_node, local_mod, options)) {
            return -1;
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath position() function. Returns LLLYXP_SET_NUMBER
 *        with the context position.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_position(struct lllyxp_set **UNUSED(args), uint16_t UNUSED(arg_count), struct lllyd_node *UNUSED(cur_node),
               struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    if (options & LLLYXP_SNODE_ALL) {
        set_snode_clear_ctx(set);
        return EXIT_SUCCESS;
    }

    if (set->type == LLLYXP_SET_EMPTY) {
        set_fill_number(set, 0);
        return EXIT_SUCCESS;
    }
    if (set->type != LLLYXP_SET_NODE_SET) {
        LOGVAL(local_mod->ctx, LLLYE_XPATH_INCTX, LLLY_VLOG_NONE, NULL, print_set_type(set), "position()");
        return -1;
    }

    set_fill_number(set, set->ctx_pos);

    /* UNUSED in 'Release' build type */
    (void)options;
    return EXIT_SUCCESS;
}

/**
 * @brief Execute the YANG 1.1 re-match(string, string) function. Returns LLLYXP_SET_BOOLEAN
 *        depending on whether the second argument regex matches the first argument string. For details refer to
 *        YANG 1.1 RFC section 10.2.1.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_re_match(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node, struct lllys_module *local_mod,
               struct lllyxp_set *set, int options)
{
    pcre *precomp;
    struct lllys_node_leaf *sleaf;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }

        if ((args[1]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[1]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if (lllyxp_set_cast(args[0], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }
    if (lllyxp_set_cast(args[1], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }

    if (lllyp_check_pattern(local_mod->ctx, args[1]->val.str, &precomp)) {
        return -1;
    }
    if (pcre_exec(precomp, NULL, args[0]->val.str, strlen(args[0]->val.str), 0, 0, NULL, 0)) {
        set_fill_boolean(set, 0);
    } else {
        set_fill_boolean(set, 1);
    }
    free(precomp);

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath round(number) function. Returns LLLYXP_SET_NUMBER
 *        with the rounded first argument. For details refer to
 *        http://www.w3.org/TR/1999/REC-xpath-19991116/#function-round.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_round(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node, struct lllys_module *local_mod,
            struct lllyxp_set *set, int options)
{
    struct lllys_node_leaf *sleaf;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type != LLLYXP_SET_SNODE_SET) || !(sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s not a node-set as expected.", __func__);
            ret = EXIT_FAILURE;
        } else if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
            ret = EXIT_FAILURE;
        } else if (!warn_is_specific_type(&sleaf->type, LLLY_TYPE_DEC64)) {
            LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of type \"decimal64\".", __func__, sleaf->name);
            ret = EXIT_FAILURE;
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if (lllyxp_set_cast(args[0], LLLYXP_SET_NUMBER, cur_node, local_mod, options)) {
        return -1;
    }

    /* cover only the cases where floor can't be used */
    if ((args[0]->val.num == -0.0f) || ((args[0]->val.num < 0) && (args[0]->val.num >= -0.5))) {
        set_fill_number(set, -0.0f);
    } else {
        args[0]->val.num += 0.5;
        if (xpath_floor(args, 1, cur_node, local_mod, args[0], options)) {
            return -1;
        }
        set_fill_number(set, args[0]->val.num);
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath starts-with(string, string) function.
 *        Returns LLLYXP_SET_BOOLEAN whether the second argument is
 *        the prefix of the first or not.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_starts_with(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node, struct lllys_module *local_mod,
                  struct lllyxp_set *set, int options)
{
    struct lllys_node_leaf *sleaf;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }

        if ((args[1]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[1]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if (lllyxp_set_cast(args[0], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }
    if (lllyxp_set_cast(args[1], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }

    if (strncmp(args[0]->val.str, args[1]->val.str, strlen(args[1]->val.str))) {
        set_fill_boolean(set, 0);
    } else {
        set_fill_boolean(set, 1);
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath string(object?) function. Returns LLLYXP_SET_STRING
 *        with the string representation of either the argument or the context.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_string(struct lllyxp_set **args, uint16_t arg_count, struct lllyd_node *cur_node, struct lllys_module *local_mod,
             struct lllyxp_set *set, int options)
{
    if (options & LLLYXP_SNODE_ALL) {
        set_snode_clear_ctx(set);
        return EXIT_SUCCESS;
    }

    if (arg_count) {
        if (lllyxp_set_cast(args[0], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
            return -1;
        }
        set_fill_set(set, args[0]);
    } else {
        if (lllyxp_set_cast(set, LLLYXP_SET_STRING, cur_node, local_mod, options)) {
            return -1;
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath string-length(string?) function. Returns LLLYXP_SET_NUMBER
 *        with the length of the string in either the argument or the context.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_string_length(struct lllyxp_set **args, uint16_t arg_count, struct lllyd_node *cur_node, struct lllys_module *local_mod,
                    struct lllyxp_set *set, int options)
{
    struct lllys_node_leaf *sleaf;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if (arg_count && (args[0]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        if (!arg_count && (set->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(set))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #0 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #0 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if (arg_count) {
        if (lllyxp_set_cast(args[0], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
            return -1;
        }
        set_fill_number(set, strlen(args[0]->val.str));
    } else {
        if (lllyxp_set_cast(set, LLLYXP_SET_STRING, cur_node, local_mod, options)) {
            return -1;
        }
        set_fill_number(set, strlen(set->val.str));
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath substring(string, number, number?) function.
 *        Returns LLLYXP_SET_STRING substring of the first argument starting
 *        on the second argument index ending on the third argument index,
 *        indexed from 1. For exact definition refer to
 *        http://www.w3.org/TR/1999/REC-xpath-19991116/#function-substring.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_substring(struct lllyxp_set **args, uint16_t arg_count, struct lllyd_node *cur_node, struct lllys_module *local_mod,
                struct lllyxp_set *set, int options)
{
    int start, len, ret = EXIT_SUCCESS;
    uint16_t str_start, str_len, pos;
    struct lllys_node_leaf *sleaf;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }

        if ((args[1]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[1]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_numeric_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is node \"%s\", not of numeric type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }

        if ((arg_count == 3) && (args[2]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[2]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #3 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_numeric_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #3 of %s is node \"%s\", not of numeric type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if (lllyxp_set_cast(args[0], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }

    /* start */
    if (xpath_round(&args[1], 1, cur_node, local_mod, args[1], options)) {
        return -1;
    }
    if (isfinite(args[1]->val.num)) {
        start = args[1]->val.num - 1;
    } else if (isinf(args[1]->val.num) && signbit(args[1]->val.num)) {
        start = INT_MIN;
    } else {
        start = INT_MAX;
    }

    /* len */
    if (arg_count == 3) {
        if (xpath_round(&args[2], 1, cur_node, local_mod, args[2], options)) {
            return -1;
        }
        if (isfinite(args[2]->val.num)) {
            len = args[2]->val.num;
        } else if (isnan(args[2]->val.num) || signbit(args[2]->val.num)) {
            len = 0;
        } else {
            len = INT_MAX;
        }
    } else {
        len = INT_MAX;
    }

    /* find matching character positions */
    str_start = 0;
    str_len = 0;
    for (pos = 0; args[0]->val.str[pos]; ++pos) {
        if (pos < start) {
            ++str_start;
        } else if (pos < start + len) {
            ++str_len;
        } else {
            break;
        }
    }

    set_fill_string(set, args[0]->val.str + str_start, str_len);
    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath substring-after(string, string) function.
 *        Returns LLLYXP_SET_STRING with the string succeeding the occurrence
 *        of the second argument in the first or an empty string.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_substring_after(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node,
                      struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    char *ptr;
    struct lllys_node_leaf *sleaf;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }

        if ((args[1]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[1]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if (lllyxp_set_cast(args[0], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }
    if (lllyxp_set_cast(args[1], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }

    ptr = strstr(args[0]->val.str, args[1]->val.str);
    if (ptr) {
        set_fill_string(set, ptr + strlen(args[1]->val.str), strlen(ptr + strlen(args[1]->val.str)));
    } else {
        set_fill_string(set, "", 0);
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath substring-before(string, string) function.
 *        Returns LLLYXP_SET_STRING with the string preceding the occurrence
 *        of the second argument in the first or an empty string.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_substring_before(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node,
                       struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    char *ptr;
    struct lllys_node_leaf *sleaf;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }

        if ((args[1]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[1]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if (lllyxp_set_cast(args[0], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }
    if (lllyxp_set_cast(args[1], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }

    ptr = strstr(args[0]->val.str, args[1]->val.str);
    if (ptr) {
        set_fill_string(set, args[0]->val.str, ptr - args[0]->val.str);
    } else {
        set_fill_string(set, "", 0);
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath sum(node-set) function. Returns LLLYXP_SET_NUMBER
 *        with the sum of all the nodes in the context.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_sum(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node, struct lllys_module *local_mod,
          struct lllyxp_set *set, int options)
{
    long double num;
    char *str;
    uint16_t i;
    struct lllyxp_set set_item;
    struct lllys_node_leaf *sleaf;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if (args[0]->type == LLLYXP_SET_SNODE_SET) {
            for (i = 0; i < args[0]->used; ++i) {
                if (args[0]->val.snodes[i].in_ctx == 1) {
                    sleaf = (struct lllys_node_leaf *)args[0]->val.snodes[i].snode;
                    if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                        LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                        ret = EXIT_FAILURE;
                    } else if (!warn_is_numeric_type(&sleaf->type)) {
                        LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of numeric type.", __func__, sleaf->name);
                        ret = EXIT_FAILURE;
                    }
                }
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    set_fill_number(set, 0);
    if (args[0]->type == LLLYXP_SET_EMPTY) {
        return EXIT_SUCCESS;
    }

    if (args[0]->type != LLLYXP_SET_NODE_SET) {
        LOGVAL(local_mod->ctx, LLLYE_XPATH_INARGTYPE, LLLY_VLOG_NONE, NULL, 1, print_set_type(args[0]), "sum(node-set)");
        return -1;
    }

    set_item.type = LLLYXP_SET_NODE_SET;
    set_item.val.nodes = malloc(sizeof *set_item.val.nodes);
    LLLY_CHECK_ERR_RETURN(!set_item.val.nodes, LOGMEM(local_mod->ctx), -1);

    set_item.used = 1;
    set_item.size = 1;

    for (i = 0; i < args[0]->used; ++i) {
        set_item.val.nodes[0] = args[0]->val.nodes[i];

        str = cast_node_set_to_string(&set_item, cur_node, local_mod, options);
        if (!str) {
            return -1;
        }
        num = cast_string_to_number(str);
        free(str);
        set->val.num += num;
    }

    free(set_item.val.nodes);

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath text() function (node type). Returns LLLYXP_SET_NODE_SET
 *        with the text content of the nodes in the context.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_text(struct lllyxp_set **UNUSED(args), uint16_t UNUSED(arg_count), struct lllyd_node *UNUSED(cur_node),
           struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    uint32_t i;

    if (options & LLLYXP_SNODE_ALL) {
        set_snode_clear_ctx(set);
        return EXIT_SUCCESS;
    }

    if (set->type == LLLYXP_SET_EMPTY) {
        return EXIT_SUCCESS;
    }
    if (set->type != LLLYXP_SET_NODE_SET) {
        LOGVAL(local_mod->ctx, LLLYE_XPATH_INCTX, LLLY_VLOG_NONE, NULL, print_set_type(set), "text()");
        return -1;
    }

    for (i = 0; i < set->used;) {
        switch (set->val.nodes[i].type) {
        case LLLYXP_NODE_ELEM:
            if (set->val.nodes[i].node->validity & LLLYD_VAL_INUSE) {
                LOGVAL(local_mod->ctx, LLLYE_XPATH_DUMMY, LLLY_VLOG_LYD, set->val.nodes[i].node, set->val.nodes[i].node->schema->name);
                return -1;
            }
            if ((set->val.nodes[i].node->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))
                    && ((struct lllyd_node_leaf_list *)set->val.nodes[i].node)->value_str) {
                set->val.nodes[i].type = LLLYXP_NODE_TEXT;
                ++i;
                break;
            }
            /* fall through */
        case LLLYXP_NODE_ROOT:
        case LLLYXP_NODE_ROOT_CONFIG:
        case LLLYXP_NODE_TEXT:
        case LLLYXP_NODE_ATTR:
            set_remove_node(set, i);
            break;
        default:
            LOGINT(local_mod->ctx);
            return -1;
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath translate(string, string, string) function.
 *        Returns LLLYXP_SET_STRING with the first argument with the characters
 *        from the second argument replaced by those on the corresponding
 *        positions in the third argument.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_translate(struct lllyxp_set **args, uint16_t UNUSED(arg_count), struct lllyd_node *cur_node,
                struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    uint16_t i, j, new_used;
    char *new;
    int found, have_removed;
    struct lllys_node_leaf *sleaf;
    int ret = EXIT_SUCCESS;

    if (options & LLLYXP_SNODE_ALL) {
        if ((args[0]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[0]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #1 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }

        if ((args[1]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[1]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #2 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }

        if ((args[2]->type == LLLYXP_SET_SNODE_SET) && (sleaf = (struct lllys_node_leaf *)warn_get_snode_in_ctx(args[2]))) {
            if (!(sleaf->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
                LOGWRN(local_mod->ctx, "Argument #3 of %s is a %s node \"%s\".", __func__, strnodetype(sleaf->nodetype), sleaf->name);
                ret = EXIT_FAILURE;
            } else if (!warn_is_string_type(&sleaf->type)) {
                LOGWRN(local_mod->ctx, "Argument #3 of %s is node \"%s\", not of string-type.", __func__, sleaf->name);
                ret = EXIT_FAILURE;
            }
        }
        set_snode_clear_ctx(set);
        return ret;
    }

    if (lllyxp_set_cast(args[0], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }
    if (lllyxp_set_cast(args[1], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }
    if (lllyxp_set_cast(args[2], LLLYXP_SET_STRING, cur_node, local_mod, options)) {
        return -1;
    }

    new = malloc((strlen(args[0]->val.str) + 1) * sizeof(char));
    LLLY_CHECK_ERR_RETURN(!new, LOGMEM(local_mod->ctx), -1);
    new_used = 0;

    have_removed = 0;
    for (i = 0; args[0]->val.str[i]; ++i) {
        found = 0;

        for (j = 0; args[1]->val.str[j]; ++j) {
            if (args[0]->val.str[i] == args[1]->val.str[j]) {
                /* removing this char */
                if (j >= strlen(args[2]->val.str)) {
                    have_removed = 1;
                    found = 1;
                    break;
                }
                /* replacing this char */
                new[new_used] = args[2]->val.str[j];
                ++new_used;
                found = 1;
                break;
            }
        }

        /* copying this char */
        if (!found) {
            new[new_used] = args[0]->val.str[i];
            ++new_used;
        }
    }

    if (have_removed) {
        new = llly_realloc(new, (new_used + 1) * sizeof(char));
        LLLY_CHECK_ERR_RETURN(!new, LOGMEM(local_mod->ctx), -1);
    }
    new[new_used] = '\0';

    lllyxp_set_cast(set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    set->type = LLLYXP_SET_STRING;
    set->val.str = new;

    return EXIT_SUCCESS;
}

/**
 * @brief Execute the XPath true() function. Returns LLLYXP_SET_BOOLEAN
 *        with true value.
 *
 * @param[in] args Array of arguments.
 * @param[in] arg_count Count of elements in \p args.
 * @param[in] cur_node Original context node.
 * @param[in,out] set Context and result set at the same time.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
xpath_true(struct lllyxp_set **UNUSED(args), uint16_t UNUSED(arg_count), struct lllyd_node *UNUSED(cur_node),
           struct lllys_module *UNUSED(local_mod), struct lllyxp_set *set, int options)
{
    if (options & LLLYXP_SNODE_ALL) {
        set_snode_clear_ctx(set);
        return EXIT_SUCCESS;
    }

    set_fill_boolean(set, 1);
    return EXIT_SUCCESS;
}

/*
 * moveto functions
 *
 * They and only they actually change the context (set).
 */

/**
 * @brief Resolve and find a specific model. Does not log.
 *
 * \p cur_snode is required in 2 quite specific cases concerning
 * XPath on schema. Problem is when we are parsing a submodule
 * and referencing something in the main module or parsing
 * a module importing another module that references back
 * the original module. Then the target module is still being
 * parsed and it not yet in the context - it fails to resolve.
 * In these cases we can find the module using \p cur_snode.
 *
 * @param[in] mod_name_ns Either module name or namespace.
 * @param[in] mon_nam_ns_len Length of \p mod_name_ns.
 * @param[in] ctx libyang context.
 * @param[in] cur_snode Current schema node, on data XPath leave NULL.
 * @param[in] is_name Whether \p mod_name_ns is module name (1) or namespace (0).
 *
 * @return Corresponding module or NULL on error.
 */
static struct lllys_module *
moveto_resolve_model(const char *mod_name_ns, uint16_t mod_nam_ns_len, struct llly_ctx *ctx, struct lllys_node *cur_snode,
                     int is_name, int import_and_disabled_model)
{
    uint16_t i;
    const char *str;
    struct lllys_module *mod, *mainmod;

    if (cur_snode) {
        /* detect if the XPath is used in augment - in such a case the module of the context node (cur_snode)
         * differs from the currently processed module. Then, we have to use the currently processed module
         * for searching for the module/namespace instead of the module of the context node */
        if (ctx->models.parsing_sub_modules_count &&
                cur_snode->module != ctx->models.parsing_sub_modules[ctx->models.parsing_sub_modules_count - 1]) {
            mod = ctx->models.parsing_sub_modules[ctx->models.parsing_sub_modules_count - 1];
        } else {
            mod = cur_snode->module;
        }
        mainmod = lllys_main_module(mod);

        str = (is_name ? mainmod->name : mainmod->ns);
        if (!strncmp(str, mod_name_ns, mod_nam_ns_len) && !str[mod_nam_ns_len]) {
            return mainmod;
        }

        for (i = 0; i < mod->imp_size; ++i) {
            str = (is_name ? mod->imp[i].module->name : mod->imp[i].module->ns);
            if (!strncmp(str, mod_name_ns, mod_nam_ns_len) && !str[mod_nam_ns_len]) {
                return mod->imp[i].module;
            }
        }
    }

    for (i = 0; i < ctx->models.used; ++i) {
        if (!import_and_disabled_model && (!ctx->models.list[i]->implemented || ctx->models.list[i]->disabled)) {
            /* skip not implemented or disabled modules */
            continue;
        }
        str = (is_name ? ctx->models.list[i]->name : ctx->models.list[i]->ns);
        if (!strncmp(str, mod_name_ns, mod_nam_ns_len) && !str[mod_nam_ns_len]) {
            return ctx->models.list[i];
        }
    }

    return NULL;
}

/**
 * @brief Get the context root.
 *
 * @param[in] cur_node Original context node.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 * @param[out] root_type Root type, differs only in when, must evaluation.
 *
 * @return Context root.
 */
static const struct lllyd_node *
moveto_get_root(const struct lllyd_node *cur_node, int options, enum lllyxp_node_type *root_type)
{
    const struct lllyd_node *root;

    if (!cur_node) {
        return NULL;
    }

    if (!options) {
        /* special kind of root that can access everything */
        for (root = cur_node; root->parent; root = root->parent);
        for (; root->prev->next; root = root->prev);
        *root_type = LLLYXP_NODE_ROOT;
        return root;
    }

    if (cur_node->schema->flags & LLLYS_CONFIG_W) {
        *root_type = LLLYXP_NODE_ROOT_CONFIG;
    } else {
        *root_type = LLLYXP_NODE_ROOT;
    }

    for (root = cur_node; root->parent; root = root->parent);
    for (; root->prev->next; root = root->prev);

    return root;
}

static const struct lllys_node *
moveto_snode_get_root(const struct lllys_node *cur_node, int options, enum lllyxp_node_type *root_type)
{
    const struct lllys_node *root;

    assert(cur_node && root_type);

    if (options & LLLYXP_SNODE) {
        /* general root that can access everything */
        *root_type = LLLYXP_NODE_ROOT;
    } else if (cur_node->flags & LLLYS_CONFIG_W) {
        *root_type = LLLYXP_NODE_ROOT_CONFIG;
    } else {
        *root_type = LLLYXP_NODE_ROOT;
    }

    root = lllys_getnext(NULL, NULL, lllys_node_module(cur_node), LLLYS_GETNEXT_NOSTATECHECK);

    return root;
}

/**
 * @brief Move context \p set to the root. Handles absolute path.
 *        Result is LLLYXP_SET_NODE_SET.
 *
 * @param[in,out] set Set to use.
 * @param[in] cur_node Original context node.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 */
static void
moveto_root(struct lllyxp_set *set, struct lllyd_node *cur_node, int options)
{
    const struct lllyd_node *root;
    enum lllyxp_node_type root_type;

    if (!set) {
        return;
    }

    root = moveto_get_root(cur_node, options, &root_type);

    lllyxp_set_cast(set, LLLYXP_SET_EMPTY, cur_node, NULL, options);
    if (root) {
        set_insert_node(set, root, 0, root_type, 0);
    }
}

static void
moveto_snode_root(struct lllyxp_set *set, struct lllys_node *cur_node, int options)
{
    const struct lllys_node *root;
    enum lllyxp_node_type root_type;

    if (!set) {
        return;
    }

    if (!cur_node) {
        LOGINT(NULL);
        return;
    }

    root = moveto_snode_get_root(cur_node, options, &root_type);
    set_snode_clear_ctx(set);
    set_snode_insert_node(set, root, root_type);
}

/**
 * @brief Check \p node as a part of NameTest processing.
 *
 * @param[in] node Node to check.
 * @param[in] root_type XPath root node type.
 * @param[in] node_name Node name to move to. Must be in the dictionary!
 * @param[in] moveto_mod Expected module of the node.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
moveto_node_check(struct lllyd_node *node, enum lllyxp_node_type root_type, const char *node_name,
                  struct lllys_module *moveto_mod, int options)
{
    /* module check */
    if (moveto_mod && (lllyd_node_module(node) != moveto_mod)) {
        return -1;
    }

    /* context check */
    if ((root_type == LLLYXP_NODE_ROOT_CONFIG) && (node->schema->flags & LLLYS_CONFIG_R)) {
        return -1;
    }

    /* name check */
    if (strcmp(node_name, "*") && !llly_strequal(node->schema->name, node_name, 1)) {
        return -1;
    }

    /* when check */
    if ((options & LLLYXP_WHEN) && !LLLYD_WHEN_DONE(node->when_status)) {
        return EXIT_FAILURE;
    }

    /* match */
    return EXIT_SUCCESS;
}

static int
moveto_snode_check(const struct lllys_node *node, enum lllyxp_node_type root_type, const char *node_name,
                   struct lllys_module *moveto_mod, int options)
{
    struct lllys_node *parent;

    /* RPC input/output check */
    for (parent = lllys_parent(node); parent && (parent->nodetype == LLLYS_USES); parent = lllys_parent(parent));
    if (options & LLLYXP_SNODE_OUTPUT) {
        if (parent && (parent->nodetype == LLLYS_INPUT)) {
            return -1;
        }
    } else {
        if (parent && (parent->nodetype == LLLYS_OUTPUT)) {
            return -1;
        }
    }

    /* module check */
    if (strcmp(node_name, "*") && (lllys_node_module(node) != moveto_mod)) {
        return -1;
    }

    /* context check */
    if ((root_type == LLLYXP_NODE_ROOT_CONFIG) && (node->flags & LLLYS_CONFIG_R)) {
        return -1;
    }

    /* name check */
    if (strcmp(node_name, "*") && !llly_strequal(node->name, node_name, 1)) {
        return -1;
    }

    /* match */
    return EXIT_SUCCESS;
}

/**
 * @brief Move context \p set to a node. Handles '/' and '*', 'NAME', 'PREFIX:*', or 'PREFIX:NAME'.
 *        Result is LLLYXP_SET_NODE_SET (or LLLYXP_SET_EMPTY). Context position aware.
 *
 * @param[in,out] set Set to use.
 * @param[in] cur_node Original context node.
 * @param[in] qname Qualified node name to move to.
 * @param[in] qname_len Length of \p qname.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
moveto_node(struct lllyxp_set *set, struct lllyd_node *cur_node, const char *qname, uint16_t qname_len, int options)
{
    uint32_t i;
    int replaced, pref_len, ret;
    const char *ptr, *name_dict = NULL; /* optimalization - so we can do (==) instead (!strncmp(...)) in moveto_node_check() */
    struct lllys_module *moveto_mod;
    struct lllyd_node *sub;
    struct llly_ctx *ctx;
    enum lllyxp_node_type root_type;

    if (!set || (set->type == LLLYXP_SET_EMPTY)) {
        return EXIT_SUCCESS;
    }

    assert(cur_node);
    ctx = cur_node->schema->module->ctx;

    if (set->type != LLLYXP_SET_NODE_SET) {
        LOGVAL(ctx, LLLYE_XPATH_INOP_1, LLLY_VLOG_NONE, NULL, "path operator", print_set_type(set));
        return -1;
    }

    moveto_get_root(cur_node, options, &root_type);

    /* prefix */
    if ((ptr = strnchr(qname, ':', qname_len))) {
        /* specific module */
        pref_len = ptr - qname;
        moveto_mod = moveto_resolve_model(qname, pref_len, ctx, NULL, 1, 0);
        if (!moveto_mod) {
            LOGVAL(ctx, LLLYE_XPATH_INMOD, LLLY_VLOG_NONE, NULL, pref_len, qname);
            return -1;
        }
        qname += pref_len + 1;
        qname_len -= pref_len + 1;
    } else if ((qname[0] == '*') && (qname_len == 1)) {
        /* all modules - special case */
        moveto_mod = NULL;
    } else {
        /* content node module */
        moveto_mod = lllyd_node_module(cur_node);
    }

    /* name */
    name_dict = lllydict_insert(ctx, qname, qname_len);

    for (i = 0; i < set->used; ) {
        replaced = 0;

        if ((set->val.nodes[i].type == LLLYXP_NODE_ROOT_CONFIG) || (set->val.nodes[i].type == LLLYXP_NODE_ROOT)) {
            LLLY_TREE_FOR(set->val.nodes[i].node, sub) {
                ret = moveto_node_check(sub, root_type, name_dict, moveto_mod, options);
                if (!ret) {
                    /* pos filled later */
                    if (!replaced) {
                        set_replace_node(set, sub, 0, LLLYXP_NODE_ELEM, i);
                        replaced = 1;
                    } else {
                        set_insert_node(set, sub, 0, LLLYXP_NODE_ELEM, i);
                    }
                    ++i;
                } else if (ret == EXIT_FAILURE) {
                    lllydict_remove(ctx, name_dict);
                    return EXIT_FAILURE;
                }
            }

        /* skip nodes without children - leaves, leaflists, anyxmls, and dummy nodes (ouput root will eval to true) */
        } else if (!(set->val.nodes[i].node->validity & LLLYD_VAL_INUSE)
                && !(set->val.nodes[i].node->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA))) {

            LLLY_TREE_FOR(set->val.nodes[i].node->child, sub) {
                ret = moveto_node_check(sub, root_type, name_dict, moveto_mod, options);
                if (!ret) {
                    if (!replaced) {
                        set_replace_node(set, sub, 0, LLLYXP_NODE_ELEM, i);
                        replaced = 1;
                    } else {
                        set_insert_node(set, sub, 0, LLLYXP_NODE_ELEM, i);
                    }
                    ++i;
                } else if (ret == EXIT_FAILURE) {
                    lllydict_remove(ctx, name_dict);
                    return EXIT_FAILURE;
                }
            }
        }

        if (!replaced) {
            /* no match */
            set_remove_node(set, i);
        }
    }
    lllydict_remove(ctx, name_dict);

    return EXIT_SUCCESS;
}

static int
moveto_snode(struct lllyxp_set *set, struct lllys_node *cur_node, const char *qname, uint16_t qname_len, int options)
{
    int i, orig_used, pref_len, idx, temp_ctx = 0;
    uint32_t mod_idx;
    const char *ptr, *name_dict = NULL; /* optimalization - so we can do (==) instead (!strncmp(...)) in moveto_node_check() */
    struct lllys_module *moveto_mod, *tmp_mod;
    const struct lllys_node *sub, *start_parent;
    struct lllys_node_augment *last_aug;
    struct llly_ctx *ctx;
    enum lllyxp_node_type root_type;

    if (!set || (set->type == LLLYXP_SET_EMPTY)) {
        return EXIT_SUCCESS;
    }

    ctx = cur_node->module->ctx;

    if (set->type != LLLYXP_SET_SNODE_SET) {
        LOGVAL(ctx, LLLYE_XPATH_INOP_1, LLLY_VLOG_NONE, NULL, "path operator", print_set_type(set));
        return -1;
    }

    moveto_snode_get_root(cur_node, options, &root_type);

    /* prefix */
    if ((ptr = strnchr(qname, ':', qname_len))) {
        pref_len = ptr - qname;
        moveto_mod = moveto_resolve_model(qname, pref_len, ctx, cur_node, 1, 1);
        if (!moveto_mod) {
            LOGVAL(ctx, LLLYE_XPATH_INMOD, LLLY_VLOG_NONE, NULL, pref_len, qname);
            return -1;
        }
        qname += pref_len + 1;
        qname_len -= pref_len + 1;
    } else if ((qname[0] == '*') && (qname_len == 1)) {
        /* all modules - special case */
        moveto_mod = NULL;
    } else {
        /* content node module */
        moveto_mod = lllys_node_module(cur_node);
    }

    /* name */
    name_dict = lllydict_insert(ctx, qname, qname_len);

    orig_used = set->used;
    for (i = 0; i < orig_used; ++i) {
        if (set->val.snodes[i].in_ctx != 1) {
            continue;
        }
        set->val.snodes[i].in_ctx = 0;

        start_parent = set->val.snodes[i].snode;

        if ((set->val.snodes[i].type == LLLYXP_NODE_ROOT_CONFIG) || (set->val.snodes[i].type == LLLYXP_NODE_ROOT)) {
            /* it can actually be in any module, it's all <running>, but we know it's moveto_mod (if set),
             * so use it directly (root node itself is useless in this case) */
            mod_idx = 0;
            while (moveto_mod || (moveto_mod = (struct lllys_module *)llly_ctx_get_module_iter(ctx, &mod_idx))) {
                sub = NULL;
                while ((sub = lllys_getnext(sub, NULL, moveto_mod, LLLYS_GETNEXT_NOSTATECHECK))) {
                    if (!moveto_snode_check(sub, root_type, name_dict, moveto_mod, options)) {
                        idx = set_snode_insert_node(set, sub, LLLYXP_NODE_ELEM);
                        /* we need to prevent these nodes from being considered in this moveto */
                        if ((idx < orig_used) && (idx > i)) {
                            set->val.snodes[idx].in_ctx = 2;
                            temp_ctx = 1;
                        }
                    }
                }

                if (!mod_idx) {
                    /* moveto_mod was specified, we are not going through the whole context */
                    break;
                }
                /* next iteration */
                moveto_mod = NULL;
            }

        /* skip nodes without children - leaves, leaflists, and anyxmls (ouput root will eval to true) */
        } else if (!(start_parent->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA))) {
            /* the target may be from an augment that was not connected */
            last_aug = NULL;
            tmp_mod = NULL;
            if ((moveto_mod && !moveto_mod->implemented) || (!moveto_mod && !lllys_node_module(cur_node)->implemented)) {
                if (moveto_mod) {
                    tmp_mod = moveto_mod;
                } else {
                    tmp_mod = lllys_node_module(cur_node);
                }

get_next_augment:
                last_aug = lllys_getnext_target_aug(last_aug, tmp_mod, start_parent);
            }

            sub = NULL;
            while ((sub = lllys_getnext(sub, (last_aug ? (struct lllys_node *)last_aug : start_parent), NULL, LLLYS_GETNEXT_NOSTATECHECK))) {
                if (!moveto_snode_check(sub, root_type, name_dict, (moveto_mod ? moveto_mod : lllys_node_module(cur_node)), options)) {
                    idx = set_snode_insert_node(set, sub, LLLYXP_NODE_ELEM);
                    if ((idx < orig_used) && (idx > i)) {
                        set->val.snodes[idx].in_ctx = 2;
                        temp_ctx = 1;
                    }
                }
            }

            if (last_aug) {
                /* try also other augments */
                goto get_next_augment;
            }
        }
    }
    lllydict_remove(ctx, name_dict);

    /* correct temporary in_ctx values */
    if (temp_ctx) {
        for (i = 0; i < orig_used; ++i) {
            if (set->val.snodes[i].in_ctx == 2) {
                set->val.snodes[i].in_ctx = 1;
            }
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Move context \p set to a node and all its descendants. Handles '//' and '*', 'NAME',
 *        'PREFIX:*', or 'PREFIX:NAME'. Result is LLLYXP_SET_NODE_SET (or LLLYXP_SET_EMPTY).
 *        Context position aware.
 *
 * @param[in] set Set to use.
 * @param[in] cur_node Original context node.
 * @param[in] qname Qualified node name to move to.
 * @param[in] qname_len Length of \p qname.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, ECIT_FAILURE on unresolved when, -1 on error.
 */
static int
moveto_node_alldesc(struct lllyxp_set *set, struct lllyd_node *cur_node, const char *qname, uint16_t qname_len,
                    int options)
{
    uint32_t i;
    int pref_len, all = 0, match, ret;
    struct lllyd_node *next, *elem, *start;
    struct lllys_module *moveto_mod;
    enum lllyxp_node_type root_type;
    struct lllyxp_set ret_set;

    if (!set || (set->type == LLLYXP_SET_EMPTY)) {
        return EXIT_SUCCESS;
    }

    if (set->type != LLLYXP_SET_NODE_SET) {
        LOGVAL(cur_node->schema->module->ctx, LLLYE_XPATH_INOP_1, LLLY_VLOG_NONE, NULL, "path operator", print_set_type(set));
        return -1;
    }

    moveto_get_root(cur_node, options, &root_type);

    /* prefix */
    if (strnchr(qname, ':', qname_len) && cur_node) {
        pref_len = strnchr(qname, ':', qname_len) - qname;
        moveto_mod = moveto_resolve_model(qname, pref_len, cur_node->schema->module->ctx, NULL, 1, 0);
        if (!moveto_mod) {
            LOGVAL(cur_node->schema->module->ctx, LLLYE_XPATH_INMOD, LLLY_VLOG_NONE, NULL, pref_len, qname);
            return -1;
        }
        qname += pref_len + 1;
        qname_len -= pref_len + 1;
    } else {
        moveto_mod = NULL;
    }

    /* replace the original nodes (and throws away all text and attr nodes, root is replaced by a child) */
    ret = moveto_node(set, cur_node, "*", 1, options);
    if (ret) {
        return ret;
    }

    if ((qname_len == 1) && (qname[0] == '*')) {
        all = 1;
    }

    /* this loop traverses all the nodes in the set and addds/keeps only
     * those that match qname */
    memset(&ret_set, 0, sizeof ret_set);
    for (i = 0; i < set->used; ++i) {

        /* TREE DFS */
        start = set->val.nodes[i].node;
        for (elem = next = start; elem; elem = next) {

            /* when check */
            if ((options & LLLYXP_WHEN) && !LLLYD_WHEN_DONE(elem->when_status)) {
                return EXIT_FAILURE;
            }

            /* dummy and context check */
            if ((elem->validity & LLLYD_VAL_INUSE) || ((root_type == LLLYXP_NODE_ROOT_CONFIG) && (elem->schema->flags & LLLYS_CONFIG_R))) {
                goto skip_children;
            }

            match = 1;

            /* module check */
            if (!all) {
                if (moveto_mod && (lllys_node_module(elem->schema) != moveto_mod)) {
                    match = 0;
                } else if (!moveto_mod && (lllys_node_module(elem->schema) != lllyd_node_module(cur_node))) {
                    match = 0;
                }
            }

            /* name check */
            if (match && !all && (strncmp(elem->schema->name, qname, qname_len) || elem->schema->name[qname_len])) {
                match = 0;
            }

            if (match) {
                /* add matching node into result set */
                set_insert_node(&ret_set, elem, 0, LLLYXP_NODE_ELEM, ret_set.used);
                if (set_dup_node_check(set, elem, LLLYXP_NODE_ELEM, i)) {
                    /* the node is a duplicate, we'll process it later in the set */
                    goto skip_children;
                }
            }

            /* TREE DFS NEXT ELEM */
            /* select element for the next run - children first */
            if (elem->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA)) {
                next = NULL;
            } else {
                next = elem->child;
            }
            if (!next) {
skip_children:
                /* no children, so try siblings, but only if it's not the start,
                 * that is considered to be the root and it's siblings are not traversed */
                if (elem != start) {
                    next = elem->next;
                } else {
                    break;
                }
            }
            while (!next) {
                /* no siblings, go back through the parents */
                if (elem->parent == start) {
                    /* we are done, no next element to process */
                    break;
                }
                /* parent is already processed, go to its sibling */
                elem = elem->parent;
                next = elem->next;
            }
        }
    }

    /* make the temporary set the current one */
    ret_set.ctx_pos = set->ctx_pos;
    ret_set.ctx_size = set->ctx_size;
    set_free_content(set);
    memcpy(set, &ret_set, sizeof *set);

    return EXIT_SUCCESS;
}

static int
moveto_snode_alldesc(struct lllyxp_set *set, struct lllys_node *cur_node, const char *qname, uint16_t qname_len,
                     int options)
{
    int i, orig_used, pref_len, all = 0, match, idx;
    struct lllys_node *next, *elem, *start;
    struct lllys_module *moveto_mod;
    struct llly_ctx *ctx;
    enum lllyxp_node_type root_type;

    if (!set || (set->type == LLLYXP_SET_EMPTY)) {
        return EXIT_SUCCESS;
    }

    ctx = cur_node->module->ctx;

    if (set->type != LLLYXP_SET_SNODE_SET) {
        LOGVAL(ctx, LLLYE_XPATH_INOP_1, LLLY_VLOG_NONE, NULL, "path operator", print_set_type(set));
        return -1;
    }

    moveto_snode_get_root(cur_node, options, &root_type);

    /* prefix */
    if (strnchr(qname, ':', qname_len)) {
        pref_len = strnchr(qname, ':', qname_len) - qname;
        moveto_mod = moveto_resolve_model(qname, pref_len, ctx, cur_node, 1, 1);
        if (!moveto_mod) {
            LOGVAL(ctx, LLLYE_XPATH_INMOD, LLLY_VLOG_NONE, NULL, pref_len, qname);
            return -1;
        }
        qname += pref_len + 1;
        qname_len -= pref_len + 1;
    } else {
        moveto_mod = NULL;
    }

    if ((qname_len == 1) && (qname[0] == '*')) {
        all = 1;
    }

    orig_used = set->used;
    for (i = 0; i < orig_used; ++i) {
        if (set->val.snodes[i].in_ctx != 1) {
            continue;
        }
        set->val.snodes[i].in_ctx = 0;

        /* TREE DFS */
        start = set->val.snodes[i].snode;
        for (elem = next = start; elem; elem = next) {

            /* context/nodetype check */
            if ((root_type == LLLYXP_NODE_ROOT_CONFIG) && (elem->flags & LLLYS_CONFIG_R)) {
                /* valid node, but it is hidden in this context */
                goto skip_children;
            }
            switch (elem->nodetype) {
            case LLLYS_USES:
            case LLLYS_CHOICE:
            case LLLYS_CASE:
                /* schema-only nodes */
                goto next_iter;
            case LLLYS_INPUT:
                if (options & LLLYXP_SNODE_OUTPUT) {
                    goto skip_children;
                }
                goto next_iter;
            case LLLYS_OUTPUT:
                if (!(options & LLLYXP_SNODE_OUTPUT)) {
                    goto skip_children;
                }
                goto next_iter;
            case LLLYS_GROUPING:
                goto skip_children;
            default:
                break;
            }

            match = 1;

            /* skip root */
            if (elem == start) {
                match = 0;
            }

            /* module check */
            if (match && !all) {
                if (moveto_mod && (lllys_node_module(elem) != moveto_mod)) {
                    match = 0;
                } else if (!moveto_mod && (lllys_node_module(elem) != lllys_node_module(cur_node))) {
                    match = 0;
                }
            }

            /* name check */
            if (match && !all && (strncmp(elem->name, qname, qname_len) || elem->name[qname_len])) {
                match = 0;
            }

            if (match) {
                if ((idx = set_snode_dup_node_check(set, elem, LLLYXP_NODE_ELEM, i)) > -1) {
                    set->val.snodes[idx].in_ctx = 1;
                    if (idx > i) {
                        /* we will process it later in the set */
                        goto skip_children;
                    }
                } else {
                    set_snode_insert_node(set, elem, LLLYXP_NODE_ELEM);
                }
            }

next_iter:
            /* TREE DFS NEXT ELEM */
            /* select element for the next run - children first */
            next = elem->child;
            if (elem->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_ANYDATA)) {
                next = NULL;
            }
            if (!next) {
skip_children:
                /* no children, so try siblings, but only if it's not the start,
                 * that is considered to be the root and it's siblings are not traversed */
                if (elem != start) {
                    next = elem->next;
                } else {
                    break;
                }
            }
            while (!next) {
                /* no siblings, go back through the parents */
                if (lllys_parent(elem) == start) {
                    /* we are done, no next element to process */
                    break;
                }
                /* parent is already processed, go to its sibling */
                elem = lllys_parent(elem);
                next = elem->next;
            }
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Move context \p set to an attribute. Handles '/' and '@*', '@NAME', '@PREFIX:*',
 *        or '@PREFIX:NAME'. Result is LLLYXP_SET_NODE_SET (or LLLYXP_SET_EMPTY).
 *        Indirectly context position aware.
 *
 * @param[in,out] set Set to use.
 * @param[in] qname Qualified node name to move to.
 * @param[in] qname_len Length of \p qname.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
moveto_attr(struct lllyxp_set *set, struct lllyd_node *cur_node, const char *qname, uint16_t qname_len, int UNUSED(options))
{
    uint32_t i;
    int replaced, all = 0, pref_len;
    struct lllys_module *moveto_mod;
    struct lllyd_attr *sub;

    if (!set || (set->type == LLLYXP_SET_EMPTY)) {
        return EXIT_SUCCESS;
    }

    if (set->type != LLLYXP_SET_NODE_SET) {
        LOGVAL(cur_node->schema->module->ctx, LLLYE_XPATH_INOP_1, LLLY_VLOG_NONE, NULL, "path operator", print_set_type(set));
        return -1;
    }

    /* prefix */
    if (strnchr(qname, ':', qname_len) && cur_node) {
        pref_len = strnchr(qname, ':', qname_len) - qname;
        moveto_mod = moveto_resolve_model(qname, pref_len, cur_node->schema->module->ctx, NULL, 1, 0);
        if (!moveto_mod) {
            LOGVAL(cur_node->schema->module->ctx, LLLYE_XPATH_INMOD, LLLY_VLOG_NONE, NULL, pref_len, qname);
            return -1;
        }
        qname += pref_len + 1;
        qname_len -= pref_len + 1;
    } else {
        moveto_mod = NULL;
    }

    if ((qname_len == 1) && (qname[0] == '*')) {
        all = 1;
    }

    for (i = 0; i < set->used; ) {
        replaced = 0;

        /* only attributes of an elem (not dummy) can be in the result, skip all the rest;
         * our attributes are always qualified */
        if ((set->val.nodes[i].type == LLLYXP_NODE_ELEM) && !(set->val.nodes[i].node->validity & LLLYD_VAL_INUSE)) {
            LLLY_TREE_FOR(set->val.nodes[i].node->attr, sub) {

                /* check "namespace" */
                if (moveto_mod && (sub->annotation->module != moveto_mod)) {
                    /* no match */
                    continue;
                }

                if (all || (!strncmp(sub->name, qname, qname_len) && !sub->name[qname_len])) {
                    /* match */
                    if (!replaced) {
                        set->val.attrs[i].attr = sub;
                        set->val.attrs[i].type = LLLYXP_NODE_ATTR;
                        /* pos does not change */
                        replaced = 1;
                    } else {
                        set_insert_node(set, (struct lllyd_node *)sub, set->val.nodes[i].pos, LLLYXP_NODE_ATTR, i + 1);
                    }
                    ++i;
                }
            }
        }

        if (!replaced) {
            /* no match */
            set_remove_node(set, i);
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Move context \p set1 to union with \p set2. \p set2 is emptied afterwards.
 *        Result is LLLYXP_SET_NODE_SET (or LLLYXP_SET_EMPTY). Context position aware.
 *
 * @param[in,out] set1 Set to use for the result.
 * @param[in] set2 Set that is copied to \p set1.
 * @param[in] cur_node Original context node.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
moveto_union(struct lllyxp_set *set1, struct lllyxp_set *set2, struct lllyd_node *cur_node, int options)
{
    struct llly_ctx *ctx = (options & LLLYXP_SNODE) ? ((struct lllys_node *)cur_node)->module->ctx : cur_node->schema->module->ctx;

    if (((set1->type != LLLYXP_SET_NODE_SET) && (set1->type != LLLYXP_SET_EMPTY))
            || ((set2->type != LLLYXP_SET_NODE_SET) && (set2->type != LLLYXP_SET_EMPTY))) {
        LOGVAL(ctx, LLLYE_XPATH_INOP_2, LLLY_VLOG_NONE, NULL, "union", print_set_type(set1), print_set_type(set2));
        return -1;
    }

    /* set2 is empty or both set1 and set2 */
    if (set2->type == LLLYXP_SET_EMPTY) {
        return EXIT_SUCCESS;
    }

    if (set1->type == LLLYXP_SET_EMPTY) {
        memcpy(set1, set2, sizeof *set1);
        /* dynamic memory belongs to set1 now, do not free */
        set2->type = LLLYXP_SET_EMPTY;
        return EXIT_SUCCESS;
    }

    /* we assume sets are sorted */
    assert(!set_sort(set1, cur_node, options) && !set_sort(set2, cur_node, options));

    /* sort, remove duplicates */
    if (set_sorted_merge(set1, set2, cur_node, options)) {
        return -1;
    }

    /* final set must be sorted */
    assert(!set_sort(set1, cur_node, options));

    return EXIT_SUCCESS;
}

/**
 * @brief Move context \p set to an attribute in any of the descendants. Handles '//' and '@*',
 *        '@NAME', '@PREFIX:*', or '@PREFIX:NAME'. Result is LLLYXP_SET_NODE_SET (or LLLYXP_SET_EMPTY).
 *        Context position aware.
 *
 * @param[in,out] set Set to use.
 * @param[in] cur_node Original context node.
 * @param[in] qname Qualified node name to move to.
 * @param[in] qname_len Length of \p qname.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
moveto_attr_alldesc(struct lllyxp_set *set, struct lllyd_node *cur_node, const char *qname, uint16_t qname_len,
                    int options)
{
    uint32_t i;
    int pref_len, replaced, all = 0, ret;
    struct lllyd_attr *sub;
    struct lllys_module *moveto_mod;
    struct lllyxp_set *set_all_desc = NULL;

    if (!set || (set->type == LLLYXP_SET_EMPTY)) {
        return EXIT_SUCCESS;
    }

    if (set->type != LLLYXP_SET_NODE_SET) {
        LOGVAL(cur_node->schema->module->ctx, LLLYE_XPATH_INOP_1, LLLY_VLOG_NONE, NULL, "path operator", print_set_type(set));
        return -1;
    }

    /* prefix */
    if (strnchr(qname, ':', qname_len)) {
        pref_len = strnchr(qname, ':', qname_len) - qname;
        moveto_mod = moveto_resolve_model(qname, pref_len, cur_node->schema->module->ctx, NULL, 1, 0);
        if (!moveto_mod) {
            LOGVAL(cur_node->schema->module->ctx, LLLYE_XPATH_INMOD, LLLY_VLOG_NONE, NULL, pref_len, qname);
            return -1;
        }
        qname += pref_len + 1;
        qname_len -= pref_len + 1;
    } else {
        moveto_mod = NULL;
    }

    /* can be optimized similarly to moveto_node_alldesc() and save considerable amount of memory,
     * but it likely won't be used much, so it's a waste of time */
    /* copy the context */
    set_all_desc = set_copy(set);
    /* get all descendant nodes (the original context nodes are removed) */
    ret = moveto_node_alldesc(set_all_desc, cur_node, "*", 1, options);
    if (ret) {
        lllyxp_set_free(set_all_desc);
        return ret;
    }
    /* prepend the original context nodes */
    if (moveto_union(set, set_all_desc, cur_node, options)) {
        lllyxp_set_free(set_all_desc);
        return -1;
    }
    lllyxp_set_free(set_all_desc);

    if ((qname_len == 1) && (qname[0] == '*')) {
        all = 1;
    }

    for (i = 0; i < set->used; ) {
        replaced = 0;

        /* only attributes of an elem can be in the result, skip all the rest,
         * we have all attributes qualified in lllyd tree */
        if (set->val.nodes[i].type == LLLYXP_NODE_ELEM) {
            LLLY_TREE_FOR(set->val.nodes[i].node->attr, sub) {
                /* check "namespace" */
                if (moveto_mod && (sub->annotation->module != moveto_mod)) {
                    /* no match */
                    continue;
                }

                if (all || (!strncmp(sub->name, qname, qname_len) && !sub->name[qname_len])) {
                    /* match */
                    if (!replaced) {
                        set->val.attrs[i].attr = sub;
                        set->val.attrs[i].type = LLLYXP_NODE_ATTR;
                        /* pos does not change */
                        replaced = 1;
                    } else {
                        set_insert_node(set, (struct lllyd_node *)sub, set->val.attrs[i].pos, LLLYXP_NODE_ATTR, i + 1);
                    }
                    ++i;
                }
            }
        }

        if (!replaced) {
            /* no match */
            set_remove_node(set, i);
        }
    }

    return EXIT_SUCCESS;
}

static int
moveto_self_add_children_r(const struct lllyd_node *parent, uint32_t parent_pos, enum lllyxp_node_type parent_type,
                           struct lllyxp_set *to_set, const struct lllyxp_set *dup_check_set, enum lllyxp_node_type root_type,
                           int options)
{
    struct lllyd_node *sub;
    int ret;

    switch (parent_type) {
    case LLLYXP_NODE_ROOT:
    case LLLYXP_NODE_ROOT_CONFIG:
        /* add the same node but as an element */
        if (!set_dup_node_check(dup_check_set, parent, LLLYXP_NODE_ELEM, -1)) {
            set_insert_node(to_set, parent, 0, LLLYXP_NODE_ELEM, to_set->used);

            /* skip anydata/anyxml and dummy nodes */
            if (!(parent->schema->nodetype & LLLYS_ANYDATA) && !(parent->validity & LLLYD_VAL_INUSE)) {
                /* also add all the children of this node, recursively */
                ret = moveto_self_add_children_r(parent, 0, LLLYXP_NODE_ELEM, to_set, dup_check_set, root_type, options);
                if (ret) {
                    return ret;
                }
            }
        }
        break;
    case LLLYXP_NODE_ELEM:
        /* add all the children ... */
        if (!(parent->schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST))) {
            LLLY_TREE_FOR(parent->child, sub) {
                /* context check */
                if ((root_type == LLLYXP_NODE_ROOT_CONFIG) && (sub->schema->flags & LLLYS_CONFIG_R)) {
                    continue;
                }

                /* when check */
                if ((options & LLLYXP_WHEN) && !LLLYD_WHEN_DONE(sub->when_status)) {
                    return EXIT_FAILURE;
                }

                if (!set_dup_node_check(dup_check_set, sub, LLLYXP_NODE_ELEM, -1)) {
                    set_insert_node(to_set, sub, 0, LLLYXP_NODE_ELEM, to_set->used);

                    /* skip anydata/anyxml and dummy nodes */
                    if ((sub->schema->nodetype & LLLYS_ANYDATA) || (sub->validity & LLLYD_VAL_INUSE)) {
                        continue;
                    }

                    /* also add all the children of this node, recursively */
                    ret = moveto_self_add_children_r(sub, 0, LLLYXP_NODE_ELEM, to_set, dup_check_set, root_type, options);
                    if (ret) {
                        return ret;
                    }
                }
            }

        /* ... or add their text node, ... */
        } else {
            /* ... but only non-empty */
            if (((struct lllyd_node_leaf_list *)parent)->value_str) {
                if (!set_dup_node_check(dup_check_set, parent, LLLYXP_NODE_TEXT, -1)) {
                    set_insert_node(to_set, parent, parent_pos, LLLYXP_NODE_TEXT, to_set->used);
                }
            }
        }
        break;
    default:
        LOGINT(lllyd_node_module(parent)->ctx);
        return -1;
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Move context \p set to self. Handles '/' or '//' and '.'. Result is LLLYXP_SET_NODE_SET
 *        (or LLLYXP_SET_EMPTY). Context position aware.
 *
 * @param[in,out] set Set to use.
 * @param[in] cur_node Original context node.
 * @param[in] all_desc Whether to go to all descendants ('//') or not ('/').
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
moveto_self(struct lllyxp_set *set, struct lllyd_node *cur_node, int all_desc, int options)
{
    uint32_t i;
    enum lllyxp_node_type root_type;
    struct lllyxp_set ret_set;
    int ret;

    if (!set || (set->type == LLLYXP_SET_EMPTY)) {
        return EXIT_SUCCESS;
    }

    if (set->type != LLLYXP_SET_NODE_SET) {
        LOGVAL(cur_node->schema->module->ctx, LLLYE_XPATH_INOP_1, LLLY_VLOG_NONE, NULL, "path operator", print_set_type(set));
        return -1;
    }

    /* nothing to do */
    if (!all_desc) {
        return EXIT_SUCCESS;
    }

    moveto_get_root(cur_node, options, &root_type);

    /* add all the children, they get added recursively */
    memset(&ret_set, 0, sizeof ret_set);
    for (i = 0; i < set->used; ++i) {
        /* copy the current node to tmp */
        set_insert_node(&ret_set, set->val.nodes[i].node, set->val.nodes[i].pos, set->val.nodes[i].type, ret_set.used);

        /* do not touch attributes and text nodes */
        if ((set->val.nodes[i].type == LLLYXP_NODE_TEXT) || (set->val.nodes[i].type == LLLYXP_NODE_ATTR)) {
            continue;
        }

        /* skip anydata/anyxml and dummy nodes */
        if ((set->val.nodes[i].node->schema->nodetype & LLLYS_ANYDATA) || (set->val.nodes[i].node->validity & LLLYD_VAL_INUSE)) {
            continue;
        }

        /* add all the children */
        ret = moveto_self_add_children_r(set->val.nodes[i].node, set->val.nodes[i].pos, set->val.nodes[i].type, &ret_set,
                                         set, root_type, options);
        if (ret) {
            set_free_content(&ret_set);
            return ret;
        }
    }

    /* use the temporary set as the current one */
    ret_set.ctx_pos = set->ctx_pos;
    ret_set.ctx_size = set->ctx_size;
    set_free_content(set);
    memcpy(set, &ret_set, sizeof *set);

    return EXIT_SUCCESS;
}

static int
moveto_snode_self(struct lllyxp_set *set, struct lllys_node *cur_node, int all_desc, int options)
{
    const struct lllys_node *sub;
    uint32_t i;
    enum lllyxp_node_type root_type;

    if (!set || (set->type == LLLYXP_SET_EMPTY)) {
        return EXIT_SUCCESS;
    }

    if (set->type != LLLYXP_SET_SNODE_SET) {
        LOGVAL(cur_node->module->ctx, LLLYE_XPATH_INOP_1, LLLY_VLOG_NONE, NULL, "path operator", print_set_type(set));
        return -1;
    }

    /* nothing to do */
    if (!all_desc) {
        return EXIT_SUCCESS;
    }

    moveto_snode_get_root(cur_node, options, &root_type);

    /* add all the children, they get added recursively */
    for (i = 0; i < set->used; ++i) {
        if (set->val.snodes[i].in_ctx != 1) {
            continue;
        }

        /* add all the children */
        if (set->val.snodes[i].snode->nodetype & (LLLYS_LIST | LLLYS_CONTAINER)) {
            sub = NULL;
            while ((sub = lllys_getnext(sub, set->val.snodes[i].snode, NULL, LLLYS_GETNEXT_NOSTATECHECK))) {
                /* RPC input/output check */
                if (options & LLLYXP_SNODE_OUTPUT) {
                    if (lllys_parent(sub)->nodetype == LLLYS_INPUT) {
                        continue;
                    }
                } else {
                    if (lllys_parent(sub)->nodetype == LLLYS_OUTPUT) {
                        continue;
                    }
                }

                /* context check */
                if ((root_type == LLLYXP_NODE_ROOT_CONFIG) && (sub->flags & LLLYS_CONFIG_R)) {
                    continue;
                }

                set_snode_insert_node(set, sub, LLLYXP_NODE_ELEM);
                /* throw away the insert index, we want to consider that node again, recursively */
            }
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Move context \p set to parent. Handles '/' or '//' and '..'. Result is LLLYXP_SET_NODE_SET
 *        (or LLLYXP_SET_EMPTY). Context position aware.
 *
 * @param[in] set Set to use.
 * @param[in] cur_node Original context node.
 * @param[in] all_desc Whether to go to all descendants ('//') or not ('/').
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
moveto_parent(struct lllyxp_set *set, struct lllyd_node *cur_node, int all_desc, int options)
{
    struct llly_ctx *ctx = cur_node->schema->module->ctx;
    int ret;
    uint32_t i;
    struct lllyd_node *node, *new_node;
    const struct lllyd_node *root;
    enum lllyxp_node_type root_type, new_type;

    if (!set || (set->type == LLLYXP_SET_EMPTY)) {
        return EXIT_SUCCESS;
    }

    if (set->type != LLLYXP_SET_NODE_SET) {
        LOGVAL(ctx, LLLYE_XPATH_INOP_1, LLLY_VLOG_NONE, NULL, "path operator", print_set_type(set));
        return -1;
    }

    if (all_desc) {
        /* <path>//.. == <path>//./.. */
        ret = moveto_self(set, cur_node, 1, options);
        if (ret) {
            return ret;
        }
    }

    root = moveto_get_root(cur_node, options, &root_type);

    for (i = 0; i < set->used; ) {
        node = set->val.nodes[i].node;

        if (set->val.nodes[i].type == LLLYXP_NODE_ELEM) {
            new_node = node->parent;
        } else if (set->val.nodes[i].type == LLLYXP_NODE_TEXT) {
            new_node = node;
        } else if (set->val.nodes[i].type == LLLYXP_NODE_ATTR) {
            new_node = (struct lllyd_node *)lllyd_attr_parent(root, set->val.attrs[i].attr);
            if (!new_node) {
                LOGINT(ctx);
                return -1;
            }
        } else {
            /* root does not have a parent */
            set_remove_node(set, i);
            continue;
        }

        /* when check */
        if ((options & LLLYXP_WHEN) && new_node && !LLLYD_WHEN_DONE(new_node->when_status)) {
            return EXIT_FAILURE;
        }

        /* node already there can also be the root */
        if (root == node) {
            if (options && (cur_node->schema->flags & LLLYS_CONFIG_W)) {
                new_type = LLLYXP_NODE_ROOT_CONFIG;
            } else {
                new_type = LLLYXP_NODE_ROOT;
            }
            new_node = node;

        /* node has no parent */
        } else if (!new_node) {
            if (options && (cur_node->schema->flags & LLLYS_CONFIG_W)) {
                new_type = LLLYXP_NODE_ROOT_CONFIG;
            } else {
                new_type = LLLYXP_NODE_ROOT;
            }
#ifndef NDEBUG
            for (; node->prev->next; node = node->prev);
            if (node != root) {
                LOGINT(ctx);
            }
#endif
            new_node = (struct lllyd_node *)root;

        /* node has a standard parent (it can equal the root, it's not the root yet since they are fake) */
        } else {
            new_type = LLLYXP_NODE_ELEM;
        }

        assert((new_type == LLLYXP_NODE_ELEM) || ((new_type == root_type) && (new_node == root)));

        if (set_dup_node_check(set, new_node, new_type, -1)) {
            set_remove_node(set, i);
        } else {
            set_replace_node(set, new_node, 0, new_type, i);
            ++i;
        }
    }

    assert(!set_sort(set, cur_node, options) && !set_sorted_dup_node_clean(set));

    return EXIT_SUCCESS;
}

static int
moveto_snode_parent(struct lllyxp_set *set, struct lllys_node *cur_node, int all_desc, int options)
{
    int idx, i, orig_used, temp_ctx = 0;
    struct lllys_node *node, *new_node;
    const struct lllys_node *root;
    enum lllyxp_node_type root_type, new_type;

    if (!set || (set->type == LLLYXP_SET_EMPTY)) {
        return EXIT_SUCCESS;
    }

    if (set->type != LLLYXP_SET_SNODE_SET) {
        LOGVAL(cur_node->module->ctx, LLLYE_XPATH_INOP_1, LLLY_VLOG_NONE, NULL, "path operator", print_set_type(set));
        return -1;
    }

    if (all_desc) {
        /* <path>//.. == <path>//./.. */
        idx = moveto_snode_self(set, cur_node, 1, options);
        if (idx) {
            return idx;
        }
    }

    root = moveto_snode_get_root(cur_node, options, &root_type);

    orig_used = set->used;
    for (i = 0; i < orig_used; ++i) {
        if (set->val.snodes[i].in_ctx != 1) {
            continue;
        }
        set->val.snodes[i].in_ctx = 0;

        node = set->val.snodes[i].snode;

        if (set->val.snodes[i].type == LLLYXP_NODE_ELEM) {
            for (new_node = lllys_parent(node);
                 new_node && (new_node->nodetype & (LLLYS_USES | LLLYS_CHOICE | LLLYS_CASE | LLLYS_INPUT | LLLYS_OUTPUT));
                 new_node = lllys_parent(new_node));
        } else {
            /* root does not have a parent */
            continue;
        }

        /* node already there can also be the root */
        if (root == node) {
            if ((options & (LLLYXP_SNODE_MUST | LLLYXP_SNODE_WHEN)) && (cur_node->flags & LLLYS_CONFIG_W)) {
                new_type = LLLYXP_NODE_ROOT_CONFIG;
            } else {
                new_type = LLLYXP_NODE_ROOT;
            }
            new_node = node;

        /* node has no parent */
        } else if (!new_node) {
            if ((options & (LLLYXP_SNODE_MUST | LLLYXP_SNODE_WHEN)) && (cur_node->flags & LLLYS_CONFIG_W)) {
                new_type = LLLYXP_NODE_ROOT_CONFIG;
            } else {
                new_type = LLLYXP_NODE_ROOT;
            }
#ifndef NDEBUG
            node = (struct lllys_node *)lllys_getnext(NULL, NULL, lllys_node_module(node), LLLYS_GETNEXT_NOSTATECHECK);
            if (node != root) {
                LOGINT(cur_node->module->ctx);
            }
#endif
            new_node = (struct lllys_node *)root;

        /* node has a standard parent (it can equal the root, it's not the root yet since they are fake) */
        } else {
            new_type = LLLYXP_NODE_ELEM;
        }

        assert((new_type == LLLYXP_NODE_ELEM) || ((new_type == root_type) && (new_node == root)));

        idx = set_snode_insert_node(set, new_node, new_type);
        if ((idx < orig_used) && (idx > i)) {
            set->val.snodes[idx].in_ctx = 2;
            temp_ctx = 1;
        }
    }

    if (temp_ctx) {
        for (i = 0; i < orig_used; ++i) {
            if (set->val.snodes[i].in_ctx == 2) {
                set->val.snodes[i].in_ctx = 1;
            }
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Move context \p set to the result of a comparison. Handles '=', '!=', '<=', '<', '>=', or '>'.
 *        Result is LLLYXP_SET_BOOLEAN. Indirectly context position aware.
 *
 * @param[in,out] set1 Set to use for the result.
 * @param[in] set2 Set acting as the second operand for \p op.
 * @param[in] op Comparison operator to process.
 * @param[in] cur_node Original context node.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
moveto_op_comp(struct lllyxp_set *set1, struct lllyxp_set *set2, const char *op, struct lllyd_node *cur_node,
               struct lllys_module *local_mod, int options)
{
    /*
     * NODE SET + NODE SET = NODE SET + STRING /(1 NODE SET) 2 STRING
     * NODE SET + STRING = STRING + STRING     /1 STRING (2 STRING)
     * NODE SET + NUMBER = NUMBER + NUMBER     /1 NUMBER (2 NUMBER)
     * NODE SET + BOOLEAN = BOOLEAN + BOOLEAN  /1 BOOLEAN (2 BOOLEAN)
     * STRING + NODE SET = STRING + STRING     /(1 STRING) 2 STRING
     * NUMBER + NODE SET = NUMBER + NUMBER     /(1 NUMBER) 2 NUMBER
     * BOOLEAN + NODE SET = BOOLEAN + BOOLEAN  /(1 BOOLEAN) 2 BOOLEAN
     *
     * '=' or '!='
     * BOOLEAN + BOOLEAN
     * BOOLEAN + STRING = BOOLEAN + BOOLEAN    /(1 BOOLEAN) 2 BOOLEAN
     * BOOLEAN + NUMBER = BOOLEAN + BOOLEAN    /(1 BOOLEAN) 2 BOOLEAN
     * STRING + BOOLEAN = BOOLEAN + BOOLEAN    /1 BOOLEAN (2 BOOLEAN)
     * NUMBER + BOOLEAN = BOOLEAN + BOOLEAN    /1 BOOLEAN (2 BOOLEAN)
     * NUMBER + NUMBER
     * NUMBER + STRING = NUMBER + NUMBER       /(1 NUMBER) 2 NUMBER
     * STRING + NUMBER = NUMBER + NUMBER       /1 NUMBER (2 NUMBER)
     * STRING + STRING
     *
     * '<=', '<', '>=', '>'
     * NUMBER + NUMBER
     * BOOLEAN + BOOLEAN = NUMBER + NUMBER     /1 NUMBER, 2 NUMBER
     * BOOLEAN + NUMBER = NUMBER + NUMBER      /1 NUMBER (2 NUMBER)
     * BOOLEAN + STRING = NUMBER + NUMBER      /1 NUMBER, 2 NUMBER
     * NUMBER + STRING = NUMBER + NUMBER       /(1 NUMBER) 2 NUMBER
     * STRING + STRING = NUMBER + NUMBER       /1 NUMBER, 2 NUMBER
     * STRING + NUMBER = NUMBER + NUMBER       /1 NUMBER (2 NUMBER)
     * NUMBER + BOOLEAN = NUMBER + NUMBER      /(1 NUMBER) 2 NUMBER
     * STRING + BOOLEAN = NUMBER + NUMBER      /(1 NUMBER) 2 NUMBER
     */
    struct lllyxp_set iter1, iter2;
    int result;
    int64_t i;

    iter1.type = LLLYXP_SET_EMPTY;

    /* empty node-sets are always false */
    if ((set1->type == LLLYXP_SET_EMPTY) || (set2->type == LLLYXP_SET_EMPTY)) {
        set_fill_boolean(set1, 0);
        return EXIT_SUCCESS;
    }

    /* iterative evaluation with node-sets */
    if ((set1->type == LLLYXP_SET_NODE_SET) || (set2->type == LLLYXP_SET_NODE_SET)) {
        if (set1->type == LLLYXP_SET_NODE_SET) {
            if (set2->type != LLLYXP_SET_NODE_SET) {
                /* canonize the value (wait until set1 is not node set if both are) */
                if (set_canonize(set2, set1)) {
                    return -1;
                }
            }
            for (i = 0; i < set1->used; ++i) {
                switch (set2->type) {
                case LLLYXP_SET_NUMBER:
                    if (set_comp_cast(&iter1, set1, LLLYXP_SET_NUMBER, cur_node, local_mod, i, options)) {
                        return -1;
                    }
                    break;
                case LLLYXP_SET_BOOLEAN:
                    if (set_comp_cast(&iter1, set1, LLLYXP_SET_BOOLEAN, cur_node, local_mod, i, options)) {
                        return -1;
                    }
                    break;
                default:
                    if (set_comp_cast(&iter1, set1, LLLYXP_SET_STRING, cur_node, local_mod, i, options)) {
                        return -1;
                    }
                    break;
                }

                if (moveto_op_comp(&iter1, set2, op, cur_node, local_mod, options)) {
                    set_free_content(&iter1);
                    return -1;
                }

                /* lazy evaluation until true */
                if (iter1.val.bool) {
                    set_fill_boolean(set1, 1);
                    return EXIT_SUCCESS;
                }
            }
        } else {
            /* canonize the value */
            if (set_canonize(set1, set2)) {
                return -1;
            }
            for (i = 0; i < set2->used; ++i) {
                switch (set1->type) {
                    case LLLYXP_SET_NUMBER:
                        if (set_comp_cast(&iter2, set2, LLLYXP_SET_NUMBER, cur_node, local_mod, i, options)) {
                            return -1;
                        }
                        break;
                    case LLLYXP_SET_BOOLEAN:
                        if (set_comp_cast(&iter2, set2, LLLYXP_SET_BOOLEAN, cur_node, local_mod, i, options)) {
                            return -1;
                        }
                        break;
                    default:
                        if (set_comp_cast(&iter2, set2, LLLYXP_SET_STRING, cur_node, local_mod, i, options)) {
                            return -1;
                        }
                        break;
                }

                set_fill_set(&iter1, set1);

                if (moveto_op_comp(&iter1, &iter2, op, cur_node, local_mod, options)) {
                    set_free_content(&iter1);
                    set_free_content(&iter2);
                    return -1;
                }
                set_free_content(&iter2);

                /* lazy evaluation until true */
                if (iter1.val.bool) {
                    set_fill_boolean(set1, 1);
                    return EXIT_SUCCESS;
                }
            }
        }

        /* false for all nodes */
        set_fill_boolean(set1, 0);
        return EXIT_SUCCESS;
    }

    /* first convert properly */
    if ((op[0] == '=') || (op[0] == '!')) {
        if ((set1->type == LLLYXP_SET_BOOLEAN) || (set2->type == LLLYXP_SET_BOOLEAN)) {
            lllyxp_set_cast(set1, LLLYXP_SET_BOOLEAN, cur_node, local_mod, options);
            lllyxp_set_cast(set2, LLLYXP_SET_BOOLEAN, cur_node, local_mod, options);
        } else if ((set1->type == LLLYXP_SET_NUMBER) || (set2->type == LLLYXP_SET_NUMBER)) {
            if (lllyxp_set_cast(set1, LLLYXP_SET_NUMBER, cur_node, local_mod, options)) {
                return -1;
            }
            if (lllyxp_set_cast(set2, LLLYXP_SET_NUMBER, cur_node, local_mod, options)) {
                return -1;
            }
        } /* else we have 2 strings */
    } else {
        if (lllyxp_set_cast(set1, LLLYXP_SET_NUMBER, cur_node, local_mod, options)) {
            return -1;
        }
        if (lllyxp_set_cast(set2, LLLYXP_SET_NUMBER, cur_node, local_mod, options)) {
            return -1;
        }
    }

    assert(set1->type == set2->type);

    /* compute result */
    if (op[0] == '=') {
        if (set1->type == LLLYXP_SET_BOOLEAN) {
            result = (set1->val.bool == set2->val.bool);
        } else if (set1->type == LLLYXP_SET_NUMBER) {
            result = (set1->val.num == set2->val.num);
        } else {
            assert(set1->type == LLLYXP_SET_STRING);
            result = (llly_strequal(set1->val.str, set2->val.str, 0));
        }
    } else if (op[0] == '!') {
        if (set1->type == LLLYXP_SET_BOOLEAN) {
            result = (set1->val.bool != set2->val.bool);
        } else if (set1->type == LLLYXP_SET_NUMBER) {
            result = (set1->val.num != set2->val.num);
        } else {
            assert(set1->type == LLLYXP_SET_STRING);
            result = (!llly_strequal(set1->val.str, set2->val.str, 0));
        }
    } else {
        assert(set1->type == LLLYXP_SET_NUMBER);
        if (op[0] == '<') {
            if (op[1] == '=') {
                result = (set1->val.num <= set2->val.num);
            } else {
                result = (set1->val.num < set2->val.num);
            }
        } else {
            if (op[1] == '=') {
                result = (set1->val.num >= set2->val.num);
            } else {
                result = (set1->val.num > set2->val.num);
            }
        }
    }

    /* assign result */
    if (result) {
        set_fill_boolean(set1, 1);
    } else {
        set_fill_boolean(set1, 0);
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Move context \p set to the result of a basic operation. Handles '+', '-', unary '-', '*', 'div',
 *        or 'mod'. Result is LLLYXP_SET_NUMBER. Indirectly context position aware.
 *
 * @param[in,out] set1 Set to use for the result.
 * @param[in] set2 Set acting as the second operand for \p op.
 * @param[in] op Operator to process.
 * @param[in] cur_node Original context node.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
moveto_op_math(struct lllyxp_set *set1, struct lllyxp_set *set2, const char *op, struct lllyd_node *cur_node,
               struct lllys_module *local_mod, int options)
{
    /* unary '-' */
    if (!set2 && (op[0] == '-')) {
        if (lllyxp_set_cast(set1, LLLYXP_SET_NUMBER, cur_node, local_mod, options)) {
            return -1;
        }
        set1->val.num *= -1;
        lllyxp_set_free(set2);
        return EXIT_SUCCESS;
    }

    assert(set1 && set2);

    if (lllyxp_set_cast(set1, LLLYXP_SET_NUMBER, cur_node, local_mod, options)) {
        return -1;
    }
    if (lllyxp_set_cast(set2, LLLYXP_SET_NUMBER, cur_node, local_mod, options)) {
        return -1;
    }

    switch (op[0]) {
    /* '+' */
    case '+':
        set1->val.num += set2->val.num;
        break;

    /* '-' */
    case '-':
        set1->val.num -= set2->val.num;
        break;

    /* '*' */
    case '*':
        set1->val.num *= set2->val.num;
        break;

    /* 'div' */
    case 'd':
        set1->val.num /= set2->val.num;
        break;

    /* 'mod' */
    case 'm':
        set1->val.num = ((long long)set1->val.num) % ((long long)set2->val.num);
        break;

    default:
        LOGINT(local_mod ? local_mod->ctx : NULL);
        return -1;
    }

    return EXIT_SUCCESS;
}

/*
 * eval functions
 *
 * They execute a parsed XPath expression on some data subtree.
 */

/**
 * @brief Evaluate Literal. Logs directly on error.
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static void
eval_literal(struct lllyxp_expr *exp, uint16_t *exp_idx, struct lllyxp_set *set)
{
    if (set) {
        if (exp->tok_len[*exp_idx] == 2) {
            set_fill_string(set, "", 0);
        } else {
            set_fill_string(set, &exp->expr[exp->expr_pos[*exp_idx] + 1], exp->tok_len[*exp_idx] - 2);
        }
    }
    LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
    ++(*exp_idx);
}

/**
 * @brief Evaluate NodeTest. Logs directly on error.
 *
 * [6] NodeTest ::= NameTest | NodeType '(' ')'
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in] attr_axis Whether to search attributes or standard nodes.
 * @param[in] all_desc Whether to search all the descendants or children only.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_node_test(struct lllyxp_expr *exp, uint16_t *exp_idx, struct lllyd_node *cur_node, struct lllys_module *local_mod,
               int attr_axis, int all_desc, struct lllyxp_set *set, int options)
{
    int i, rc = 0;
    char *path;

    switch (exp->tokens[*exp_idx]) {
    case LLLYXP_TOKEN_NAMETEST:
        if (attr_axis) {
            if (set && (options & LLLYXP_SNODE_ALL)) {
                set_snode_clear_ctx(set);
            } else {
                if (all_desc) {
                    rc = moveto_attr_alldesc(set, cur_node, &exp->expr[exp->expr_pos[*exp_idx]],
                                             exp->tok_len[*exp_idx], options);
                } else {
                    rc = moveto_attr(set, cur_node, &exp->expr[exp->expr_pos[*exp_idx]], exp->tok_len[*exp_idx],
                                     options);
                }
            }
        } else {
            if (all_desc) {
                if (set && (options & LLLYXP_SNODE_ALL)) {
                    rc = moveto_snode_alldesc(set, (struct lllys_node *)cur_node, &exp->expr[exp->expr_pos[*exp_idx]],
                                              exp->tok_len[*exp_idx], options);
                } else {
                    rc = moveto_node_alldesc(set, cur_node, &exp->expr[exp->expr_pos[*exp_idx]],
                                             exp->tok_len[*exp_idx], options);
                }
            } else {
                if (set && (options & LLLYXP_SNODE_ALL)) {
                    rc = moveto_snode(set, (struct lllys_node *)cur_node, &exp->expr[exp->expr_pos[*exp_idx]],
                                      exp->tok_len[*exp_idx], options);
                } else {
                    rc = moveto_node(set, cur_node, &exp->expr[exp->expr_pos[*exp_idx]], exp->tok_len[*exp_idx],
                                     options);
                }
            }

            if (!rc && set && (options & LLLYXP_SNODE_ALL)) {
                for (i = set->used - 1; i > -1; --i) {
                    if (set->val.snodes[i].in_ctx) {
                        break;
                    }
                }
                if (i == -1) {
                    path = lllys_path((struct lllys_node *)cur_node, LLLYS_PATH_FIRST_PREFIX);
                    LOGWRN(local_mod->ctx, "Schema node \"%.*s\" not found (%.*s) with context node \"%s\".",
                           exp->tok_len[*exp_idx], &exp->expr[exp->expr_pos[*exp_idx]],
                           exp->expr_pos[*exp_idx] + exp->tok_len[*exp_idx], exp->expr, path);
                    free(path);
                }
            }
        }
        if (rc) {
            return rc;
        }

        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);
        break;

    case LLLYXP_TOKEN_NODETYPE:
        if (set) {
            assert(exp->tok_len[*exp_idx] == 4);
            if (set->type == LLLYXP_SET_SNODE_SET) {
                set_snode_clear_ctx(set);
                /* just for the debug message underneath */
                set = NULL;
            } else {
                if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "node", 4)) {
                    if (xpath_node(NULL, 0, cur_node, local_mod, set, options)) {
                        return -1;
                    }
                } else {
                    assert(!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "text", 4));
                    if (xpath_text(NULL, 0, cur_node, local_mod, set, options)) {
                        return -1;
                    }
                }
            }
        }
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        /* '(' */
        assert(exp->tokens[*exp_idx] == LLLYXP_TOKEN_PAR1);
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        /* ')' */
        assert(exp->tokens[*exp_idx] == LLLYXP_TOKEN_PAR2);
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);
        break;

    default:
        LOGINT(local_mod ? local_mod->ctx : NULL);
        return -1;
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Evaluate Predicate. Logs directly on error.
 *
 * [7] Predicate ::= '[' Expr ']'
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_predicate(struct lllyxp_expr *exp, uint16_t *exp_idx, struct lllyd_node *cur_node, struct lllys_module *local_mod,
               struct lllyxp_set *set, int options, int parent_pos_pred)
{
    int ret;
    uint16_t i, orig_exp;
    uint32_t orig_pos, orig_size, pred_in_ctx;
    struct lllyxp_set set2;
    struct lllyd_node *orig_parent;

    /* '[' */
    LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
           print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
    ++(*exp_idx);

    if (!set) {
only_parse:
        ret = eval_expr_select(exp, exp_idx, 0, cur_node, local_mod, NULL, options);
        if (ret == -1 || ret == EXIT_FAILURE) {
            return ret;
        }
    } else if (set->type == LLLYXP_SET_NODE_SET) {
        /* we (possibly) need the set sorted, it can affect the result (if the predicate result is a number) */
        assert(!set_sort(set, cur_node, options));

        /* empty set, nothing to evaluate */
        if (!set->used) {
            goto only_parse;
        }

        orig_exp = *exp_idx;
        orig_pos = 0;
        orig_size = set->used;
        orig_parent = NULL;
        for (i = 0; i < set->used; ++i) {
            memset(&set2, 0, sizeof set2);
            set_insert_node(&set2, set->val.nodes[i].node, set->val.nodes[i].pos, set->val.nodes[i].type, 0);
            /* remember the node context position for position() and context size for last(),
             * predicates should always be evaluated with respect to the child axis (since we do
             * not support explicit axes) so we assign positions based on their parents */
            if (parent_pos_pred && (set->val.nodes[i].node->parent != orig_parent)) {
                orig_parent = set->val.nodes[i].node->parent;
                orig_pos = 1;
            } else {
                ++orig_pos;
            }

            set2.ctx_pos = orig_pos;
            set2.ctx_size = orig_size;
            *exp_idx = orig_exp;

            ret = eval_expr_select(exp, exp_idx, 0, cur_node, local_mod, &set2, options);
            if (ret == -1 || ret == EXIT_FAILURE) {
                lllyxp_set_cast(&set2, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
                return ret;
            }

            /* number is a position */
            if (set2.type == LLLYXP_SET_NUMBER) {
                if ((long long)set2.val.num == orig_pos) {
                    set2.val.num = 1;
                } else {
                    set2.val.num = 0;
                }
            }
            lllyxp_set_cast(&set2, LLLYXP_SET_BOOLEAN, cur_node, local_mod, options);

            /* predicate satisfied or not? */
            if (!set2.val.bool) {
#ifdef LLLY_ENABLED_CACHE
                set_remove_node_hash(set, set->val.nodes[i].node, set->val.nodes[i].type);
#endif
                set->val.nodes[i].type = LLLYXP_NODE_NONE;
            }
        }

        /* now actually remove all nodes that have not satisfied the predicate */
        set_remove_none_nodes(set);

    } else if (set->type == LLLYXP_SET_SNODE_SET) {
        for (i = 0; i < set->used; ++i) {
            if (set->val.snodes[i].in_ctx == 1) {
                /* there is a currently-valid node */
                break;
            }
        }
        /* empty set, nothing to evaluate */
        if (i == set->used) {
            goto only_parse;
        }

        orig_exp = *exp_idx;

        /* set special in_ctx to all the valid snodes */
        pred_in_ctx = set_snode_new_in_ctx(set);

        /* use the valid snodes one-by-one */
        for (i = 0; i < set->used; ++i) {
            if (set->val.snodes[i].in_ctx != pred_in_ctx) {
                continue;
            }
            set->val.snodes[i].in_ctx = 1;

            *exp_idx = orig_exp;

            ret = eval_expr_select(exp, exp_idx, 0, cur_node, local_mod, set, options);
            if (ret == -1 || ret == EXIT_FAILURE) {
                return ret;
            }

            set->val.snodes[i].in_ctx = pred_in_ctx;
        }

        /* restore the state as it was before the predicate */
        for (i = 0; i < set->used; ++i) {
            if (set->val.snodes[i].in_ctx == 1) {
                set->val.snodes[i].in_ctx = 0;
            } else if (set->val.snodes[i].in_ctx == pred_in_ctx) {
                set->val.snodes[i].in_ctx = 1;
            }
        }

    } else {
        set2.type = LLLYXP_SET_EMPTY;
        set_fill_set(&set2, set);

        ret = eval_expr_select(exp, exp_idx, 0, cur_node, local_mod, &set2, options);
        if (ret == -1 || ret == EXIT_FAILURE) {
            lllyxp_set_cast(&set2, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
            return ret;
        }

        lllyxp_set_cast(&set2, LLLYXP_SET_BOOLEAN, cur_node, local_mod, options);
        if (!set2.val.bool) {
            lllyxp_set_cast(set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
        }
        lllyxp_set_cast(&set2, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    }

    /* ']' */
    assert(exp->tokens[*exp_idx] == LLLYXP_TOKEN_BRACK2);
    LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
           print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
    ++(*exp_idx);

    return EXIT_SUCCESS;
}

/**
 * @brief Evaluate RelativeLocationPath. Logs directly on error.
 *
 * [4] RelativeLocationPath ::= Step | RelativeLocationPath '/' Step | RelativeLocationPath '//' Step
 * [5] Step ::= '@'? NodeTest Predicate* | '.' | '..'
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in] all_desc Whether to search all the descendants or children only.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_relative_location_path(struct lllyxp_expr *exp, uint16_t *exp_idx, struct lllyd_node *cur_node, struct lllys_module *local_mod,
                            int all_desc, struct lllyxp_set *set, int options)
{
    int attr_axis, ret;

    goto step;
    do {
        /* evaluate '/' or '//' */
        if (exp->tok_len[*exp_idx] == 1) {
            all_desc = 0;
        } else {
            assert(exp->tok_len[*exp_idx] == 2);
            all_desc = 1;
        }
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

step:
        /* Step */
        attr_axis = 0;
        switch (exp->tokens[*exp_idx]) {
        case LLLYXP_TOKEN_DOT:
            /* evaluate '.' */
            if (set && (options & LLLYXP_SNODE_ALL)) {
                ret = moveto_snode_self(set, (struct lllys_node *)cur_node, all_desc, options);
            } else {
                ret = moveto_self(set, cur_node, all_desc, options);
            }
            if (ret) {
                return ret;
            }
            LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
            ++(*exp_idx);
            break;

        case LLLYXP_TOKEN_DDOT:
            /* evaluate '..' */
            if (set && (options & LLLYXP_SNODE_ALL)) {
                ret = moveto_snode_parent(set, (struct lllys_node *)cur_node, all_desc, options);
            } else {
                ret = moveto_parent(set, cur_node, all_desc, options);
            }
            if (ret) {
                return ret;
            }
            LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
            ++(*exp_idx);
            break;

        case LLLYXP_TOKEN_AT:
            /* evaluate '@' */
            attr_axis = 1;
            LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
            ++(*exp_idx);

            /* fall through */
        case LLLYXP_TOKEN_NAMETEST:
        case LLLYXP_TOKEN_NODETYPE:
            ret = eval_node_test(exp, exp_idx, cur_node, local_mod, attr_axis, all_desc, set, options);
            if (ret) {
                return ret;
            }

            while ((exp->used > *exp_idx) && (exp->tokens[*exp_idx] == LLLYXP_TOKEN_BRACK1)) {
                ret = eval_predicate(exp, exp_idx, cur_node, local_mod, set, options, 1);
                if (ret) {
                    return ret;
                }
            }
            break;

        default:
            LOGINT(local_mod ? local_mod->ctx : NULL);
            return -1;
        }
    } while ((exp->used > *exp_idx) && (exp->tokens[*exp_idx] == LLLYXP_TOKEN_OPERATOR_PATH));

    return EXIT_SUCCESS;
}

/**
 * @brief Evaluate AbsoluteLocationPath. Logs directly on error.
 *
 * [3] AbsoluteLocationPath ::= '/' RelativeLocationPath? | '//' RelativeLocationPath
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_absolute_location_path(struct lllyxp_expr *exp, uint16_t *exp_idx, struct lllyd_node *cur_node, struct lllys_module *local_mod,
                            struct lllyxp_set *set, int options)
{
    int all_desc, ret;

    if (set) {
        /* no matter what tokens follow, we need to be at the root */
        if (options & LLLYXP_SNODE_ALL) {
            moveto_snode_root(set, (struct lllys_node *)cur_node, options);
        } else {
            moveto_root(set, cur_node, options);
        }
    }

    /* '/' RelativeLocationPath? */
    if (exp->tok_len[*exp_idx] == 1) {
        /* evaluate '/' - deferred */
        all_desc = 0;
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        if (exp_check_token(local_mod->ctx, exp, *exp_idx, LLLYXP_TOKEN_NONE, 0)) {
            return EXIT_SUCCESS;
        }
        switch (exp->tokens[*exp_idx]) {
        case LLLYXP_TOKEN_DOT:
        case LLLYXP_TOKEN_DDOT:
        case LLLYXP_TOKEN_AT:
        case LLLYXP_TOKEN_NAMETEST:
        case LLLYXP_TOKEN_NODETYPE:
            ret = eval_relative_location_path(exp, exp_idx, cur_node, local_mod, all_desc, set, options);
            if (ret) {
                return ret;
            }
            break;
        default:
            break;
        }

    /* '//' RelativeLocationPath */
    } else {
        /* evaluate '//' - deferred so as not to waste memory by remembering all the nodes */
        all_desc = 1;
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        ret =  eval_relative_location_path(exp, exp_idx, cur_node, local_mod, all_desc, set, options);
        if (ret) {
            return ret;
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Evaluate FunctionCall. Logs directly on error.
 *
 * [9] FunctionCall ::= FunctionName '(' ( Expr ( ',' Expr )* )? ')'
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_function_call(struct lllyxp_expr *exp, uint16_t *exp_idx, struct lllyd_node *cur_node, struct lllys_module *local_mod,
                   struct lllyxp_set *set, int options)
{
    int rc = EXIT_FAILURE;
    int (*xpath_func)(struct lllyxp_set **, uint16_t, struct lllyd_node *, struct lllys_module *, struct lllyxp_set *, int) = NULL;
    uint16_t arg_count = 0, i, func_exp = *exp_idx;
    struct lllyxp_set **args = NULL, **args_aux;

    if (set) {
        /* FunctionName */
        switch (exp->tok_len[*exp_idx]) {
        case 3:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "not", 3)) {
                xpath_func = &xpath_not;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "sum", 3)) {
                xpath_func = &xpath_sum;
            }
            break;
        case 4:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "lang", 4)) {
                xpath_func = &xpath_lang;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "last", 4)) {
                xpath_func = &xpath_last;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "name", 4)) {
                xpath_func = &xpath_name;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "true", 4)) {
                xpath_func = &xpath_true;
            }
            break;
        case 5:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "count", 5)) {
                xpath_func = &xpath_count;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "false", 5)) {
                xpath_func = &xpath_false;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "floor", 5)) {
                xpath_func = &xpath_floor;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "round", 5)) {
                xpath_func = &xpath_round;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "deref", 5)) {
                xpath_func = &xpath_deref;
            }
            break;
        case 6:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "concat", 6)) {
                xpath_func = &xpath_concat;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "number", 6)) {
                xpath_func = &xpath_number;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "string", 6)) {
                xpath_func = &xpath_string;
            }
            break;
        case 7:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "boolean", 7)) {
                xpath_func = &xpath_boolean;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "ceiling", 7)) {
                xpath_func = &xpath_ceiling;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "current", 7)) {
                xpath_func = &xpath_current;
            }
            break;
        case 8:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "contains", 8)) {
                xpath_func = &xpath_contains;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "position", 8)) {
                xpath_func = &xpath_position;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "re-match", 8)) {
                xpath_func = &xpath_re_match;
            }
            break;
        case 9:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "substring", 9)) {
                xpath_func = &xpath_substring;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "translate", 9)) {
                xpath_func = &xpath_translate;
            }
            break;
        case 10:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "local-name", 10)) {
                xpath_func = &xpath_local_name;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "enum-value", 10)) {
                xpath_func = &xpath_enum_value;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "bit-is-set", 10)) {
                xpath_func = &xpath_bit_is_set;
            }
            break;
        case 11:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "starts-with", 11)) {
                xpath_func = &xpath_starts_with;
            }
            break;
        case 12:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "derived-from", 12)) {
                xpath_func = &xpath_derived_from;
            }
            break;
        case 13:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "namespace-uri", 13)) {
                xpath_func = &xpath_namespace_uri;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "string-length", 13)) {
                xpath_func = &xpath_string_length;
            }
            break;
        case 15:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "normalize-space", 15)) {
                xpath_func = &xpath_normalize_space;
            } else if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "substring-after", 15)) {
                xpath_func = &xpath_substring_after;
            }
            break;
        case 16:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "substring-before", 16)) {
                xpath_func = &xpath_substring_before;
            }
            break;
        case 20:
            if (!strncmp(&exp->expr[exp->expr_pos[*exp_idx]], "derived-from-or-self", 20)) {
                xpath_func = &xpath_derived_from_or_self;
            }
            break;
        }

        if (!xpath_func) {
            LOGVAL(local_mod->ctx, LLLYE_XPATH_INTOK, LLLY_VLOG_NONE, NULL, "Unknown", &exp->expr[exp->expr_pos[*exp_idx]]);
            LOGVAL(local_mod->ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL,
                   "Unknown XPath function \"%.*s\".", exp->tok_len[*exp_idx], &exp->expr[exp->expr_pos[*exp_idx]]);
            return -1;
        }
    }

    LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
    ++(*exp_idx);

    /* '(' */
    assert(exp->tokens[*exp_idx] == LLLYXP_TOKEN_PAR1);
    LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
    ++(*exp_idx);

    /* ( Expr ( ',' Expr )* )? */
    if (exp->tokens[*exp_idx] != LLLYXP_TOKEN_PAR2) {
        if (set) {
            args = malloc(sizeof *args);
            LLLY_CHECK_ERR_GOTO(!args, LOGMEM(local_mod->ctx), cleanup);
            arg_count = 1;
            args[0] = set_copy(set);
            if (!args[0]) {
                goto cleanup;
            }

            rc = eval_expr_select(exp, exp_idx, 0, cur_node, local_mod, args[0], options);
            if (rc == -1 || rc == EXIT_FAILURE) {
                goto cleanup;
            }
        } else {
            rc = eval_expr_select(exp, exp_idx, 0, cur_node, local_mod, NULL, options);
            if (rc == -1 || rc == EXIT_FAILURE) {
                goto cleanup;
            }
        }
    }
    while ((exp->used > *exp_idx) && (exp->tokens[*exp_idx] == LLLYXP_TOKEN_COMMA)) {
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        if (set) {
            ++arg_count;
            args_aux = realloc(args, arg_count * sizeof *args);
            LLLY_CHECK_ERR_GOTO(!args_aux, arg_count--; LOGMEM(local_mod->ctx), cleanup);
            args = args_aux;
            args[arg_count - 1] = set_copy(set);
            if (!args[arg_count - 1]) {
                goto cleanup;
            }

            rc = eval_expr_select(exp, exp_idx, 0, cur_node, local_mod, args[arg_count - 1], options);
            if (rc == -1 || rc == EXIT_FAILURE) {
                goto cleanup;
            }
        } else {
            rc = eval_expr_select(exp, exp_idx, 0, cur_node, local_mod, NULL, options);
            if (rc == -1 || rc == EXIT_FAILURE) {
                goto cleanup;
            }
        }
    }

    /* ')' */
    assert(exp->tokens[*exp_idx] == LLLYXP_TOKEN_PAR2);
    LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
    ++(*exp_idx);

    if (set) {
        /* evaluate function */
        rc = xpath_func(args, arg_count, cur_node, local_mod, set, options);

        if (options & LLLYXP_SNODE_ALL) {
            if (rc == EXIT_FAILURE) {
                /* some validation warning */
                LOGWRN(local_mod->ctx, "Previous warning generated by XPath function \"%.*s\".",
                       (exp->expr_pos[*exp_idx - 1] - exp->expr_pos[func_exp]) + 1, &exp->expr[exp->expr_pos[func_exp]]);
                rc = EXIT_SUCCESS;
            }

            /* merge all nodes from arg evaluations */
            for (i = 0; i < arg_count; ++i) {
                set_snode_clear_ctx(args[i]);
                set_snode_merge(set, args[i]);
            }
        }
    } else {
        rc = EXIT_SUCCESS;
    }

cleanup:
    for (i = 0; i < arg_count; ++i) {
        lllyxp_set_free(args[i]);
    }
    free(args);

    return rc;
}

/**
 * @brief Evaluate Number. Logs directly on error.
 *
 * @param[in] ctx Context for errors.
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
static int
eval_number(struct llly_ctx *ctx, struct lllyxp_expr *exp, uint16_t *exp_idx, struct lllyxp_set *set)
{
    long double num;
    char *endptr;

    if (set) {
        errno = 0;
        num = strtold(&exp->expr[exp->expr_pos[*exp_idx]], &endptr);
        if (errno) {
            LOGVAL(ctx, LLLYE_XPATH_INTOK, LLLY_VLOG_NONE, NULL, "Unknown", &exp->expr[exp->expr_pos[*exp_idx]]);
            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Failed to convert \"%.*s\" into a long double (%s).",
                   exp->tok_len[*exp_idx], &exp->expr[exp->expr_pos[*exp_idx]], strerror(errno));
            return -1;
        } else if (endptr - &exp->expr[exp->expr_pos[*exp_idx]] != exp->tok_len[*exp_idx]) {
            LOGVAL(ctx, LLLYE_XPATH_INTOK, LLLY_VLOG_NONE, NULL, "Unknown", &exp->expr[exp->expr_pos[*exp_idx]]);
            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Failed to convert \"%.*s\" into a long double.",
                   exp->tok_len[*exp_idx], &exp->expr[exp->expr_pos[*exp_idx]]);
            return -1;
        }

        set_fill_number(set, num);
    }

    LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
    ++(*exp_idx);
    return EXIT_SUCCESS;
}

/**
 * @brief Evaluate PathExpr. Logs directly on error.
 *
 * [10] PathExpr ::= LocationPath | PrimaryExpr Predicate*
 *                 | PrimaryExpr Predicate* '/' RelativeLocationPath
 *                 | PrimaryExpr Predicate* '//' RelativeLocationPath
 * [2] LocationPath ::= RelativeLocationPath | AbsoluteLocationPath
 * [8] PrimaryExpr ::= '(' Expr ')' | Literal | Number | FunctionCall
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_path_expr(struct lllyxp_expr *exp, uint16_t *exp_idx, struct lllyd_node *cur_node, struct lllys_module *local_mod,
               struct lllyxp_set *set, int options)
{
    int all_desc, ret, parent_pos_pred;

    switch (exp->tokens[*exp_idx]) {
    case LLLYXP_TOKEN_PAR1:
        /* '(' Expr ')' */

        /* '(' */
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        /* Expr */
        ret = eval_expr_select(exp, exp_idx, 0, cur_node, local_mod, set, options);
        if (ret == -1 || ret == EXIT_FAILURE) {
            return ret;
        }

        /* ')' */
        assert(exp->tokens[*exp_idx] == LLLYXP_TOKEN_PAR2);
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        parent_pos_pred = 0;
        goto predicate;

    case LLLYXP_TOKEN_DOT:
    case LLLYXP_TOKEN_DDOT:
    case LLLYXP_TOKEN_AT:
    case LLLYXP_TOKEN_NAMETEST:
    case LLLYXP_TOKEN_NODETYPE:
        /* RelativeLocationPath */
        ret = eval_relative_location_path(exp, exp_idx, cur_node, local_mod, 0, set, options);
        if (ret) {
            return ret;
        }
        break;

    case LLLYXP_TOKEN_FUNCNAME:
        /* FunctionCall */
        if (!set) {
            ret = eval_function_call(exp, exp_idx, cur_node, local_mod, NULL, options);
        } else {
            ret = eval_function_call(exp, exp_idx, cur_node, local_mod, set, options);
        }
        if (ret) {
            return ret;
        }

        parent_pos_pred = 1;
        goto predicate;

    case LLLYXP_TOKEN_OPERATOR_PATH:
        /* AbsoluteLocationPath */
        ret = eval_absolute_location_path(exp, exp_idx, cur_node, local_mod, set, options);
        if (ret) {
            return ret;
        }
        break;

    case LLLYXP_TOKEN_LITERAL:
        /* Literal */
        if (!set || (options & LLLYXP_SNODE_ALL)) {
            if (set) {
                set_snode_clear_ctx(set);
            }
            eval_literal(exp, exp_idx, NULL);
        } else {
            eval_literal(exp, exp_idx, set);
        }

        parent_pos_pred = 1;
        goto predicate;

    case LLLYXP_TOKEN_NUMBER:
        /* Number */
        if (!set || (options & LLLYXP_SNODE_ALL)) {
            if (set) {
                set_snode_clear_ctx(set);
            }
            ret = eval_number(local_mod->ctx, exp, exp_idx, NULL);
        } else {
            ret = eval_number(local_mod->ctx, exp, exp_idx, set);
        }
        if (ret) {
            return ret;
        }

        parent_pos_pred = 1;
        goto predicate;

    default:
        LOGVAL(local_mod->ctx, LLLYE_XPATH_INTOK, LLLY_VLOG_NONE, NULL,
               print_token(exp->tokens[*exp_idx]), &exp->expr[exp->expr_pos[*exp_idx]]);
        return -1;
    }

    return EXIT_SUCCESS;

predicate:
    /* Predicate* */
    while ((exp->used > *exp_idx) && (exp->tokens[*exp_idx] == LLLYXP_TOKEN_BRACK1)) {
        ret = eval_predicate(exp, exp_idx, cur_node, local_mod, set, options, parent_pos_pred);
        if (ret) {
            return ret;
        }
    }

    /* ('/' or '//') RelativeLocationPath */
    if ((exp->used > *exp_idx) && (exp->tokens[*exp_idx] == LLLYXP_TOKEN_OPERATOR_PATH)) {

        /* evaluate '/' or '//' */
        if (exp->tok_len[*exp_idx] == 1) {
            all_desc = 0;
        } else {
            assert(exp->tok_len[*exp_idx] == 2);
            all_desc = 1;
        }

        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        ret = eval_relative_location_path(exp, exp_idx, cur_node, local_mod, all_desc, set, options);
        if (ret) {
            return ret;
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Evaluate UnionExpr. Logs directly on error.
 *
 * [18] UnionExpr ::= PathExpr | UnionExpr '|' PathExpr
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_union_expr(struct lllyxp_expr *exp, uint16_t *exp_idx, uint16_t repeat, struct lllyd_node *cur_node,
                struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    int ret;
    struct lllyxp_set orig_set, set2;
    uint16_t i;

    assert(repeat);

    memset(&orig_set, 0, sizeof orig_set);
    memset(&set2, 0, sizeof set2);

    set_fill_set(&orig_set, set);

    ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_UNION, cur_node, local_mod, set, options);
    if (ret) {
        goto finish;
    }

    /* ('|' PathExpr)* */
    for (i = 0; i < repeat; ++i) {
        assert(exp->tokens[*exp_idx] == LLLYXP_TOKEN_OPERATOR_UNI);
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        if (!set) {
            ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_UNION, cur_node, local_mod, NULL, options);
            if (ret) {
                goto finish;
            }
            continue;
        }

        set_fill_set(&set2, &orig_set);
        ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_UNION, cur_node, local_mod, &set2, options);
        if (ret) {
            goto finish;
        }

        /* eval */
        if (options & LLLYXP_SNODE_ALL) {
            set_snode_merge(set, &set2);
        } else if (moveto_union(set, &set2, cur_node, options)) {
            ret = -1;
            goto finish;
        }
    }

finish:
    lllyxp_set_cast(&orig_set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    lllyxp_set_cast(&set2, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    return ret;
}

/**
 * @brief Evaluate UnaryExpr. Logs directly on error.
 *
 * [17] UnaryExpr ::= UnionExpr | '-' UnaryExpr
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_unary_expr(struct lllyxp_expr *exp, uint16_t *exp_idx, uint16_t repeat, struct lllyd_node *cur_node,
                struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    int ret;
    uint16_t this_op, i;

    assert(repeat);

    /* ('-')+ */
    this_op = *exp_idx;
    for (i = 0; i < repeat; ++i) {
        assert(!exp_check_token(local_mod->ctx, exp, *exp_idx, LLLYXP_TOKEN_OPERATOR_MATH, 0) && (exp->expr[exp->expr_pos[*exp_idx]] == '-'));

        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);
    }

    ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_UNARY, cur_node, local_mod, set, options);
    if (ret) {
        return ret;
    }

    if (set && (repeat % 2)) {
        if (options & LLLYXP_SNODE_ALL) {
            warn_operands(local_mod->ctx, set, NULL, 1, exp->expr, exp->expr_pos[this_op]);
        } else {
            if (moveto_op_math(set, NULL, &exp->expr[exp->expr_pos[this_op]], cur_node, local_mod, options)) {
                return -1;
            }
        }
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Evaluate MultiplicativeExpr. Logs directly on error.
 *
 * [16] MultiplicativeExpr ::= UnaryExpr
 *                     | MultiplicativeExpr '*' UnaryExpr
 *                     | MultiplicativeExpr 'div' UnaryExpr
 *                     | MultiplicativeExpr 'mod' UnaryExpr
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_multiplicative_expr(struct lllyxp_expr *exp, uint16_t *exp_idx, uint16_t repeat, struct lllyd_node *cur_node,
                         struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    int ret;
    uint16_t this_op;
    struct lllyxp_set orig_set, set2;
    uint16_t i;

    assert(repeat);

    memset(&orig_set, 0, sizeof orig_set);
    memset(&set2, 0, sizeof set2);

    set_fill_set(&orig_set, set);

    ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_MULTIPLICATIVE, cur_node, local_mod, set, options);
    if (ret) {
        goto finish;
    }

    /* ('*' / 'div' / 'mod' UnaryExpr)* */
    for (i = 0; i < repeat; ++i) {
        this_op = *exp_idx;

        assert(exp->tokens[*exp_idx] == LLLYXP_TOKEN_OPERATOR_MATH);
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        if (!set) {
            ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_MULTIPLICATIVE, cur_node, local_mod, NULL, options);
            if (ret) {
                goto finish;
            }
            continue;
        }

        set_fill_set(&set2, &orig_set);
        ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_MULTIPLICATIVE, cur_node, local_mod, &set2, options);
        if (ret) {
            goto finish;
        }

        /* eval */
        if (options & LLLYXP_SNODE_ALL) {
            warn_operands(local_mod->ctx, set, &set2, 1, exp->expr, exp->expr_pos[this_op - 1]);
            set_snode_merge(set, &set2);
            set_snode_clear_ctx(set);
        } else {
            if (moveto_op_math(set, &set2, &exp->expr[exp->expr_pos[this_op]], cur_node, local_mod, options)) {
                ret = -1;
                goto finish;
            }
        }
    }

finish:
    lllyxp_set_cast(&orig_set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    lllyxp_set_cast(&set2, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    return ret;
}

/**
 * @brief Evaluate AdditiveExpr. Logs directly on error.
 *
 * [15] AdditiveExpr ::= MultiplicativeExpr
 *                     | AdditiveExpr '+' MultiplicativeExpr
 *                     | AdditiveExpr '-' MultiplicativeExpr
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_additive_expr(struct lllyxp_expr *exp, uint16_t *exp_idx, uint16_t repeat, struct lllyd_node *cur_node,
                   struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    int ret;
    uint16_t this_op;
    struct lllyxp_set orig_set, set2;
    uint16_t i;

    assert(repeat);

    memset(&orig_set, 0, sizeof orig_set);
    memset(&set2, 0, sizeof set2);

    set_fill_set(&orig_set, set);

    ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_ADDITIVE, cur_node, local_mod, set, options);
    if (ret) {
        goto finish;
    }

    /* ('+' / '-' MultiplicativeExpr)* */
    for (i = 0; i < repeat; ++i) {
        this_op = *exp_idx;

        assert(exp->tokens[*exp_idx] == LLLYXP_TOKEN_OPERATOR_MATH);
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        if (!set) {
            ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_ADDITIVE, cur_node, local_mod, NULL, options);
            if (ret) {
                goto finish;
            }
            continue;
        }

        set_fill_set(&set2, &orig_set);
        ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_ADDITIVE, cur_node, local_mod, &set2, options);
        if (ret) {
            goto finish;
        }

        /* eval */
        if (options & LLLYXP_SNODE_ALL) {
            warn_operands(local_mod->ctx, set, &set2, 1, exp->expr, exp->expr_pos[this_op - 1]);
            set_snode_merge(set, &set2);
            set_snode_clear_ctx(set);
        } else {
            if (moveto_op_math(set, &set2, &exp->expr[exp->expr_pos[this_op]], cur_node, local_mod, options)) {
                goto finish;
            }
        }
    }

finish:
    lllyxp_set_cast(&orig_set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    lllyxp_set_cast(&set2, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    return ret;
}

/**
 * @brief Evaluate RelationalExpr. Logs directly on error.
 *
 * [14] RelationalExpr ::= AdditiveExpr
 *                       | RelationalExpr '<' AdditiveExpr
 *                       | RelationalExpr '>' AdditiveExpr
 *                       | RelationalExpr '<=' AdditiveExpr
 *                       | RelationalExpr '>=' AdditiveExpr
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_relational_expr(struct lllyxp_expr *exp, uint16_t *exp_idx, uint16_t repeat, struct lllyd_node *cur_node,
                     struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    int ret;
    uint16_t this_op;
    struct lllyxp_set orig_set, set2;
    uint16_t i;

    assert(repeat);

    memset(&orig_set, 0, sizeof orig_set);
    memset(&set2, 0, sizeof set2);

    set_fill_set(&orig_set, set);

    ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_RELATIONAL, cur_node, local_mod, set, options);
    if (ret) {
        goto finish;
    }

    /* ('<' / '>' / '<=' / '>=' AdditiveExpr)* */
    for (i = 0; i < repeat; ++i) {
        this_op = *exp_idx;

        assert(exp->tokens[*exp_idx] == LLLYXP_TOKEN_OPERATOR_COMP);
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        if (!set) {
            ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_RELATIONAL, cur_node, local_mod, NULL, options);
            if (ret) {
                goto finish;
            }
            continue;
        }

        set_fill_set(&set2, &orig_set);
        ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_RELATIONAL, cur_node, local_mod, &set2, options);
        if (ret) {
            goto finish;
        }

        /* eval */
        if (options & LLLYXP_SNODE_ALL) {
            warn_operands(local_mod->ctx, set, &set2, 1, exp->expr, exp->expr_pos[this_op - 1]);
            set_snode_merge(set, &set2);
            set_snode_clear_ctx(set);
        } else {
            if (moveto_op_comp(set, &set2, &exp->expr[exp->expr_pos[this_op]], cur_node, local_mod, options)) {
                ret = -1;
                goto finish;
            }
        }
    }

finish:
    lllyxp_set_cast(&orig_set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    lllyxp_set_cast(&set2, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    return ret;
}

/**
 * @brief Evaluate EqualityExpr. Logs directly on error.
 *
 * [13] EqualityExpr ::= RelationalExpr | EqualityExpr '=' RelationalExpr
 *                     | EqualityExpr '!=' RelationalExpr
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_equality_expr(struct lllyxp_expr *exp, uint16_t *exp_idx, uint16_t repeat, struct lllyd_node *cur_node,
                   struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    int ret;
    uint16_t this_op;
    struct lllyxp_set orig_set, set2;
    uint16_t i;

    assert(repeat);

    memset(&orig_set, 0, sizeof orig_set);
    memset(&set2, 0, sizeof set2);

    set_fill_set(&orig_set, set);

    ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_EQUALITY, cur_node, local_mod, set, options);
    if (ret) {
        goto finish;
    }

    /* ('=' / '!=' RelationalExpr)* */
    for (i = 0; i < repeat; ++i) {
        this_op = *exp_idx;

        assert(exp->tokens[*exp_idx] == LLLYXP_TOKEN_OPERATOR_COMP);
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (set ? "parsed" : "skipped"),
               print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        if (!set) {
            ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_EQUALITY, cur_node, local_mod, NULL, options);
            if (ret) {
                return ret;
            }
            continue;
        }

        set_fill_set(&set2, &orig_set);
        ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_EQUALITY, cur_node, local_mod, &set2, options);
        if (ret) {
            goto finish;
        }

        /* eval */
        if (options & LLLYXP_SNODE_ALL) {
            warn_operands(local_mod->ctx, set, &set2, 0, exp->expr, exp->expr_pos[this_op - 1]);
            warn_equality_value(local_mod->ctx, exp, set, *exp_idx - 1, this_op - 1, *exp_idx - 1);
            warn_equality_value(local_mod->ctx, exp, &set2, this_op - 1, this_op - 1, *exp_idx - 1);
            set_snode_merge(set, &set2);
            set_snode_clear_ctx(set);
        } else {
            if (moveto_op_comp(set, &set2, &exp->expr[exp->expr_pos[this_op]], cur_node, local_mod, options)) {
                ret = -1;
                goto finish;
            }
        }
    }

finish:
    lllyxp_set_cast(&orig_set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    lllyxp_set_cast(&set2, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    return ret;
}

/**
 * @brief Evaluate AndExpr. Logs directly on error.
 *
 * [12] AndExpr ::= EqualityExpr | AndExpr 'and' EqualityExpr
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_and_expr(struct lllyxp_expr *exp, uint16_t *exp_idx, uint16_t repeat, struct lllyd_node *cur_node,
              struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    int ret;
    struct lllyxp_set orig_set, set2;
    uint16_t i;

    assert(repeat);

    memset(&orig_set, 0, sizeof orig_set);
    memset(&set2, 0, sizeof set2);

    set_fill_set(&orig_set, set);

    ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_AND, cur_node, local_mod, set, options);
    if (ret) {
        goto finish;
    }

    /* cast to boolean, we know that will be the final result */
    if (set && (options & LLLYXP_SNODE_ALL)) {
        set_snode_clear_ctx(set);
    } else {
        lllyxp_set_cast(set, LLLYXP_SET_BOOLEAN, cur_node, local_mod, options);
    }

    /* ('and' EqualityExpr)* */
    for (i = 0; i < repeat; ++i) {
        assert(exp->tokens[*exp_idx] == LLLYXP_TOKEN_OPERATOR_LOG);
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (!set || !set->val.bool ? "skipped" : "parsed"),
            print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        /* lazy evaluation */
        if (!set || ((set->type == LLLYXP_SET_BOOLEAN) && !set->val.bool)) {
            ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_AND, cur_node, local_mod, NULL, options);
            if (ret) {
                goto finish;
            }
            continue;
        }

        set_fill_set(&set2, &orig_set);
        ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_AND, cur_node, local_mod, &set2, options);
        if (ret) {
            goto finish;
        }

        /* eval - just get boolean value actually */
        if (set->type == LLLYXP_SET_SNODE_SET) {
            set_snode_clear_ctx(&set2);
            set_snode_merge(set, &set2);
        } else {
            lllyxp_set_cast(&set2, LLLYXP_SET_BOOLEAN, cur_node, local_mod, options);
            set_fill_set(set, &set2);
        }
    }

finish:
    lllyxp_set_cast(&orig_set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    lllyxp_set_cast(&set2, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    return ret;
}

/**
 * @brief Evaluate OrExpr. Logs directly on error.
 *
 * [11] OrExpr ::= AndExpr | OrExpr 'or' AndExpr
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_or_expr(struct lllyxp_expr *exp, uint16_t *exp_idx, uint16_t repeat, struct lllyd_node *cur_node,
             struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    int ret;
    struct lllyxp_set orig_set, set2;
    uint16_t i;

    assert(repeat);

    memset(&orig_set, 0, sizeof orig_set);
    memset(&set2, 0, sizeof set2);

    set_fill_set(&orig_set, set);

    ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_OR, cur_node, local_mod, set, options);
    if (ret) {
        goto finish;
    }

    /* cast to boolean, we know that will be the final result */
    if (set && (options & LLLYXP_SNODE_ALL)) {
        set_snode_clear_ctx(set);
    } else {
        lllyxp_set_cast(set, LLLYXP_SET_BOOLEAN, cur_node, local_mod, options);
    }

    /* ('or' AndExpr)* */
    for (i = 0; i < repeat; ++i) {
        assert(exp->tokens[*exp_idx] == LLLYXP_TOKEN_OPERATOR_LOG);
        LOGDBG(LLLY_LDGXPATH, "%-27s %s %s[%u]", __func__, (!set || set->val.bool ? "skipped" : "parsed"),
            print_token(exp->tokens[*exp_idx]), exp->expr_pos[*exp_idx]);
        ++(*exp_idx);

        /* lazy evaluation */
        if (!set || ((set->type == LLLYXP_SET_BOOLEAN) && set->val.bool)) {
            ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_OR, cur_node, local_mod, NULL, options);
            if (ret) {
                goto finish;
            }
            continue;
        }

        set_fill_set(&set2, &orig_set);
        /* expr_type cound have been LLLYXP_EXPR_NONE in all these later calls (except for the first one),
         * but it does not matter */
        ret = eval_expr_select(exp, exp_idx, LLLYXP_EXPR_OR, cur_node, local_mod, &set2, options);
        if (ret) {
            goto finish;
        }

        /* eval - just get boolean value actually */
        if (set->type == LLLYXP_SET_SNODE_SET) {
            set_snode_clear_ctx(&set2);
            set_snode_merge(set, &set2);
        } else {
            lllyxp_set_cast(&set2, LLLYXP_SET_BOOLEAN, cur_node, local_mod, options);
            set_fill_set(set, &set2);
        }
    }

finish:
    lllyxp_set_cast(&orig_set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    lllyxp_set_cast(&set2, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    return ret;
}

/**
 * @brief Decide what expression is at the pointer \p exp_idx and evaluate it accordingly.
 *
 * @param[in] exp Parsed XPath expression.
 * @param[in] exp_idx Position in the expression \p exp.
 * @param[in] repeat_used How many values were already used from the current token repeat array.
 * @param[in] cur_node Start node for the expression \p exp.
 * @param[in] local_mod Module considered to be local.
 * @param[in,out] set Context and result set. On NULL the rule is only parsed.
 * @param[in] options Whether to apply data node access restrictions defined for 'when' and 'must' evaluation.
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when, -1 on error.
 */
static int
eval_expr_select(struct lllyxp_expr *exp, uint16_t *exp_idx, enum lllyxp_expr_type etype, struct lllyd_node *cur_node,
                 struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    int ret;
    uint16_t i, count;
    enum lllyxp_expr_type next_etype;

    /* process operator repeats */
    if (!exp->repeat[*exp_idx]) {
        next_etype = LLLYXP_EXPR_NONE;
    } else {
        /* find etype repeat */
        for (i = 0; exp->repeat[*exp_idx][i] > etype; ++i);

        /* select one-priority lower because etype expression called us */
        if (i) {
            next_etype = exp->repeat[*exp_idx][i - 1];
            /* count repeats for that expression */
            for (count = 0; i && exp->repeat[*exp_idx][i - 1] == next_etype; ++count, --i);
        } else {
            next_etype = LLLYXP_EXPR_NONE;
        }
    }

    /* decide what expression are we parsing based on the repeat */
    switch (next_etype) {
    case LLLYXP_EXPR_OR:
        ret = eval_or_expr(exp, exp_idx, count, cur_node, local_mod, set, options);
        break;
    case LLLYXP_EXPR_AND:
        ret = eval_and_expr(exp, exp_idx, count, cur_node, local_mod, set, options);
        break;
    case LLLYXP_EXPR_EQUALITY:
        ret = eval_equality_expr(exp, exp_idx, count, cur_node, local_mod, set, options);
        break;
    case LLLYXP_EXPR_RELATIONAL:
        ret = eval_relational_expr(exp, exp_idx, count, cur_node, local_mod, set, options);
        break;
    case LLLYXP_EXPR_ADDITIVE:
        ret = eval_additive_expr(exp, exp_idx, count, cur_node, local_mod, set, options);
        break;
    case LLLYXP_EXPR_MULTIPLICATIVE:
        ret = eval_multiplicative_expr(exp, exp_idx, count, cur_node, local_mod, set, options);
        break;
    case LLLYXP_EXPR_UNARY:
        ret = eval_unary_expr(exp, exp_idx, count, cur_node, local_mod, set, options);
        break;
    case LLLYXP_EXPR_UNION:
        ret = eval_union_expr(exp, exp_idx, count, cur_node, local_mod, set, options);
        break;
    case LLLYXP_EXPR_NONE:
        ret = eval_path_expr(exp, exp_idx, cur_node, local_mod, set, options);
        break;
    default:
        ret = -1;
        LOGINT(local_mod ? local_mod->ctx : NULL);
        break;
    }

    return ret;
}

int
lllyxp_eval(const char *expr, const struct lllyd_node *cur_node, enum lllyxp_node_type cur_node_type,
          const struct lllys_module *local_mod, struct lllyxp_set *set, int options)
{
    struct llly_ctx *ctx;
    struct lllyxp_expr *exp;
    uint16_t exp_idx = 0;
    int rc = -1;

    if (!expr || !local_mod || !set) {
        LOGARG;
        return EXIT_FAILURE;
    }

    ctx = local_mod->ctx;

    exp = lllyxp_parse_expr(ctx, expr);
    if (!exp) {
        rc = -1;
        goto finish;
    }

    rc = reparse_or_expr(ctx, exp, &exp_idx);
    if (rc) {
        goto finish;
    } else if (exp->used > exp_idx) {
        LOGVAL(ctx, LLLYE_XPATH_INTOK, LLLY_VLOG_NONE, NULL, "Unknown", &exp->expr[exp->expr_pos[exp_idx]]);
        LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Unparsed characters \"%s\" left at the end of an XPath expression.",
               &exp->expr[exp->expr_pos[exp_idx]]);
        rc = -1;
        goto finish;
    }

    print_expr_struct_debug(exp);

    exp_idx = 0;
    memset(set, 0, sizeof *set);
    set->type = LLLYXP_SET_EMPTY;
    if (cur_node) {
        set_insert_node(set, (struct lllyd_node *)cur_node, 0, cur_node_type, 0);
    }

    rc = eval_expr_select(exp, &exp_idx, 0, (struct lllyd_node *)cur_node, (struct lllys_module *)local_mod, set, options);
    if (rc == 2) {
        rc = EXIT_SUCCESS;
    }
    if ((rc == -1) && cur_node) {
        LOGPATH(ctx, LLLY_VLOG_LYD, cur_node);
        lllyxp_set_cast(set, LLLYXP_SET_EMPTY, cur_node, local_mod, options);
    }

finish:
    lllyxp_expr_free(exp);
    return rc;
}

#if 0

/* full xml printing of set elements, not used currently */

void
lllyxp_set_print_xml(FILE *f, struct lllyxp_set *set)
{
    uint32_t i;
    char *str_num;
    struct lllyout out;

    memset(&out, 0, sizeof out);

    out.type = LLLYOUT_STREAM;
    out.method.f = f;

    switch (set->type) {
    case LLLYXP_SET_EMPTY:
        llly_print(&out, "Empty XPath set\n\n");
        break;
    case LLLYXP_SET_BOOLEAN:
        llly_print(&out, "Boolean XPath set:\n");
        llly_print(&out, "%s\n\n", set->value.bool ? "true" : "false");
        break;
    case LLLYXP_SET_STRING:
        llly_print(&out, "String XPath set:\n");
        llly_print(&out, "\"%s\"\n\n", set->value.str);
        break;
    case LLLYXP_SET_NUMBER:
        llly_print(&out, "Number XPath set:\n");

        if (isnan(set->value.num)) {
            str_num = strdup("NaN");
        } else if ((set->value.num == 0) || (set->value.num == -0.0f)) {
            str_num = strdup("0");
        } else if (isinf(set->value.num) && !signbit(set->value.num)) {
            str_num = strdup("Infinity");
        } else if (isinf(set->value.num) && signbit(set->value.num)) {
            str_num = strdup("-Infinity");
        } else if ((long long)set->value.num == set->value.num) {
            if (asprintf(&str_num, "%lld", (long long)set->value.num) == -1) {
                str_num = NULL;
            }
        } else {
            if (asprintf(&str_num, "%03.1Lf", set->value.num) == -1) {
                str_num = NULL;
            }
        }
        if (!str_num) {
            LOGMEM;
            return;
        }
        llly_print(&out, "%s\n\n", str_num);
        free(str_num);
        break;
    case LLLYXP_SET_NODE_SET:
        llly_print(&out, "Node XPath set:\n");

        for (i = 0; i < set->used; ++i) {
            llly_print(&out, "%d. ", i + 1);
            switch (set->node_type[i]) {
            case LLLYXP_NODE_ROOT_ALL:
                llly_print(&out, "ROOT all\n\n");
                break;
            case LLLYXP_NODE_ROOT_CONFIG:
                llly_print(&out, "ROOT config\n\n");
                break;
            case LLLYXP_NODE_ROOT_STATE:
                llly_print(&out, "ROOT state\n\n");
                break;
            case LLLYXP_NODE_ROOT_NOTIF:
                llly_print(&out, "ROOT notification \"%s\"\n\n", set->value.nodes[i]->schema->name);
                break;
            case LLLYXP_NODE_ROOT_RPC:
                llly_print(&out, "ROOT rpc \"%s\"\n\n", set->value.nodes[i]->schema->name);
                break;
            case LLLYXP_NODE_ROOT_OUTPUT:
                llly_print(&out, "ROOT output \"%s\"\n\n", set->value.nodes[i]->schema->name);
                break;
            case LLLYXP_NODE_ELEM:
                llly_print(&out, "ELEM \"%s\"\n", set->value.nodes[i]->schema->name);
                xml_print_node(&out, 1, set->value.nodes[i], 1, LLLYP_FORMAT);
                llly_print(&out, "\n");
                break;
            case LLLYXP_NODE_TEXT:
                llly_print(&out, "TEXT \"%s\"\n\n", ((struct lllyd_node_leaf_list *)set->value.nodes[i])->value_str);
                break;
            case LLLYXP_NODE_ATTR:
                llly_print(&out, "ATTR \"%s\" = \"%s\"\n\n", set->value.attrs[i]->name, set->value.attrs[i]->value);
                break;
            }
        }
        break;
    }
}

#endif

int
lllyxp_set_cast(struct lllyxp_set *set, enum lllyxp_set_type target, const struct lllyd_node *cur_node,
              const struct lllys_module *local_mod, int options)
{
    long double num;
    char *str;

    if (!set || (set->type == target)) {
        return EXIT_SUCCESS;
    }

    /* it's not possible to convert anything into a node set */
    assert((target != LLLYXP_SET_NODE_SET) && ((set->type != LLLYXP_SET_SNODE_SET) || (target == LLLYXP_SET_EMPTY)));

    if (set->type == LLLYXP_SET_SNODE_SET) {
        set_free_content(set);
        return -1;
    }

    /* to STRING */
    if ((target == LLLYXP_SET_STRING) || ((target == LLLYXP_SET_NUMBER)
            && ((set->type == LLLYXP_SET_NODE_SET) || (set->type == LLLYXP_SET_EMPTY)))) {
        switch (set->type) {
        case LLLYXP_SET_NUMBER:
            if (isnan(set->val.num)) {
                set->val.str = strdup("NaN");
                LLLY_CHECK_ERR_RETURN(!set->val.str, LOGMEM(local_mod->ctx), -1);
            } else if ((set->val.num == 0) || (set->val.num == -0.0f)) {
                set->val.str = strdup("0");
                LLLY_CHECK_ERR_RETURN(!set->val.str, LOGMEM(local_mod->ctx), -1);
            } else if (isinf(set->val.num) && !signbit(set->val.num)) {
                set->val.str = strdup("Infinity");
                LLLY_CHECK_ERR_RETURN(!set->val.str, LOGMEM(local_mod->ctx), -1);
            } else if (isinf(set->val.num) && signbit(set->val.num)) {
                set->val.str = strdup("-Infinity");
                LLLY_CHECK_ERR_RETURN(!set->val.str, LOGMEM(local_mod->ctx), -1);
            } else if ((long long)set->val.num == set->val.num) {
                if (asprintf(&str, "%lld", (long long)set->val.num) == -1) {
                    LOGMEM(local_mod->ctx);
                    return -1;
                }
                set->val.str = str;
            } else {
                if (asprintf(&str, "%03.1Lf", set->val.num) == -1) {
                    LOGMEM(local_mod->ctx);
                    return -1;
                }
                set->val.str = str;
            }
            break;
        case LLLYXP_SET_BOOLEAN:
            if (set->val.bool) {
                set->val.str = strdup("true");
            } else {
                set->val.str = strdup("false");
            }
            LLLY_CHECK_ERR_RETURN(!set->val.str, LOGMEM(local_mod->ctx), -1);
            break;
        case LLLYXP_SET_NODE_SET:
            assert(set->used);

            /* we need the set sorted, it affects the result */
            assert(!set_sort(set, cur_node, options));

            str = cast_node_set_to_string(set, (struct lllyd_node *)cur_node, (struct lllys_module *)local_mod, options);
            if (!str) {
                return -1;
            }
            set_free_content(set);
            set->val.str = str;
            break;
        case LLLYXP_SET_EMPTY:
            set->val.str = strdup("");
            LLLY_CHECK_ERR_RETURN(!set->val.str, LOGMEM(local_mod->ctx), -1);
            break;
        default:
            LOGINT(local_mod ? local_mod->ctx : NULL);
            return -1;
        }
        set->type = LLLYXP_SET_STRING;
    }

    /* to NUMBER */
    if (target == LLLYXP_SET_NUMBER) {
        switch (set->type) {
        case LLLYXP_SET_STRING:
            num = cast_string_to_number(set->val.str);
            set_free_content(set);
            set->val.num = num;
            break;
        case LLLYXP_SET_BOOLEAN:
            if (set->val.bool) {
                set->val.num = 1;
            } else {
                set->val.num = 0;
            }
            break;
        default:
            LOGINT(local_mod ? local_mod->ctx : NULL);
            return -1;
        }
        set->type = LLLYXP_SET_NUMBER;
    }

    /* to BOOLEAN */
    if (target == LLLYXP_SET_BOOLEAN) {
        switch (set->type) {
        case LLLYXP_SET_NUMBER:
            if ((set->val.num == 0) || (set->val.num == -0.0f) || isnan(set->val.num)) {
                set->val.bool = 0;
            } else {
                set->val.bool = 1;
            }
            break;
        case LLLYXP_SET_STRING:
            if (set->val.str[0]) {
                set_free_content(set);
                set->val.bool = 1;
            } else {
                set_free_content(set);
                set->val.bool = 0;
            }
            break;
        case LLLYXP_SET_NODE_SET:
            set_free_content(set);

            assert(set->used);
            set->val.bool = 1;
            break;
        case LLLYXP_SET_EMPTY:
            set->val.bool = 0;
            break;
        default:
            LOGINT(local_mod ? local_mod->ctx : NULL);
            return -1;
        }
        set->type = LLLYXP_SET_BOOLEAN;
    }

    /* to EMPTY */
    if (target == LLLYXP_SET_EMPTY) {
        set_free_content(set);
        set->type = LLLYXP_SET_EMPTY;
    }

    return EXIT_SUCCESS;
}

int
lllyxp_atomize(const char *expr, const struct lllys_node *cur_snode, enum lllyxp_node_type cur_snode_type,
             struct lllyxp_set *set, int options, const struct lllys_node **ctx_snode)
{
    struct lllys_node *_ctx_snode;
    enum lllyxp_node_type ctx_snode_type;
    struct lllyxp_expr *exp;
    uint16_t exp_idx = 0;
    int rc = -1;

    exp = lllyxp_parse_expr(cur_snode->module->ctx, expr);
    if (!exp) {
        rc = -1;
        goto finish;
    }

    rc = reparse_or_expr(cur_snode->module->ctx, exp, &exp_idx);
    if (rc) {
        goto finish;
    } else if (exp->used > exp_idx) {
        LOGVAL(cur_snode->module->ctx, LLLYE_XPATH_INTOK, LLLY_VLOG_NONE, NULL, "Unknown", &exp->expr[exp->expr_pos[exp_idx]]);
        LOGVAL(cur_snode->module->ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Unparsed characters \"%s\" left at the end of an XPath expression.",
               &exp->expr[exp->expr_pos[exp_idx]]);
        rc = -1;
        goto finish;
    }

    print_expr_struct_debug(exp);

    if (options & LLLYXP_SNODE_WHEN) {
        /* for when the context node may need to be changed */
        resolve_when_ctx_snode(cur_snode, &_ctx_snode, &ctx_snode_type);
    } else {
        _ctx_snode = (struct lllys_node *)cur_snode;
        ctx_snode_type = cur_snode_type;
    }

    if (ctx_snode) {
        *ctx_snode = _ctx_snode;
    }

    exp_idx = 0;
    memset(set, 0, sizeof *set);
    set->type = LLLYXP_SET_SNODE_SET;
    set_snode_insert_node(set, _ctx_snode, ctx_snode_type);

    rc = eval_expr_select(exp, &exp_idx, 0, (struct lllyd_node *)_ctx_snode, lllys_node_module(_ctx_snode), set, options);
    if (rc == 2) {
        rc = EXIT_SUCCESS;
    }

finish:
    lllyxp_expr_free(exp);
    return rc;
}

int
lllyxp_node_atomize(const struct lllys_node *node, struct lllyxp_set *set, int set_ext_dep_flags)
{
    struct lllys_node *parent, *elem;
    const struct lllys_node *ctx_snode = NULL;
    struct lllyxp_set tmp_set;
    uint8_t must_size = 0;
    uint32_t i, j;
    int opts, ret = EXIT_SUCCESS;
    struct lllys_when *when = NULL;
    struct lllys_restr *must = NULL;
    char *path = NULL;

    memset(&tmp_set, 0, sizeof tmp_set);
    memset(set, 0, sizeof *set);

    /* check if we will be traversing RPC output */
    opts = 0;
    for (parent = (struct lllys_node *)node; parent && (parent->nodetype != LLLYS_OUTPUT); parent = lllys_parent(parent));
    if (parent) {
        opts |= LLLYXP_SNODE_OUTPUT;
    }

    switch (node->nodetype) {
    case LLLYS_CONTAINER:
        when = ((struct lllys_node_container *)node)->when;
        must = ((struct lllys_node_container *)node)->must;
        must_size = ((struct lllys_node_container *)node)->must_size;
        break;
    case LLLYS_CHOICE:
        when = ((struct lllys_node_choice *)node)->when;
        break;
    case LLLYS_LEAF:
        when = ((struct lllys_node_leaf *)node)->when;
        must = ((struct lllys_node_leaf *)node)->must;
        must_size = ((struct lllys_node_leaf *)node)->must_size;
        break;
    case LLLYS_LEAFLIST:
        when = ((struct lllys_node_leaflist *)node)->when;
        must = ((struct lllys_node_leaflist *)node)->must;
        must_size = ((struct lllys_node_leaflist *)node)->must_size;
        break;
    case LLLYS_LIST:
        when = ((struct lllys_node_list *)node)->when;
        must = ((struct lllys_node_list *)node)->must;
        must_size = ((struct lllys_node_list *)node)->must_size;
        break;
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        when = ((struct lllys_node_anydata *)node)->when;
        must = ((struct lllys_node_anydata *)node)->must;
        must_size = ((struct lllys_node_anydata *)node)->must_size;
        break;
    case LLLYS_CASE:
        when = ((struct lllys_node_case *)node)->when;
        break;
    case LLLYS_NOTIF:
        must = ((struct lllys_node_notif *)node)->must;
        must_size = ((struct lllys_node_notif *)node)->must_size;
        break;
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
        must = ((struct lllys_node_inout *)node)->must;
        must_size = ((struct lllys_node_inout *)node)->must_size;
        break;
    case LLLYS_USES:
        when = ((struct lllys_node_uses *)node)->when;
        break;
    case LLLYS_AUGMENT:
        when = ((struct lllys_node_augment *)node)->when;
        break;
    default:
        /* nothing to check */
        break;
    }

    if (set_ext_dep_flags) {
        /* find operation if in one, used later */
        for (parent = (struct lllys_node *)node;
             parent && !(parent->nodetype & (LLLYS_RPC | LLLYS_ACTION | LLLYS_NOTIF));
             parent = lllys_parent(parent));
    }

    /* check "when" */
    if (when) {
        if (lllyxp_atomize(when->cond, node, LLLYXP_NODE_ELEM, &tmp_set, LLLYXP_SNODE_WHEN | opts, &ctx_snode)) {
            free(tmp_set.val.snodes);
            if (ctx_snode) {
                path = lllys_path(ctx_snode, LLLYS_PATH_FIRST_PREFIX);
                LOGVAL(node->module->ctx, LLLYE_SPEC, LLLY_VLOG_LYS, node,
                       "Invalid when condition \"%s\" with context node \"%s\".", when->cond, path);
            } else {
                LOGVAL(node->module->ctx, LLLYE_SPEC, LLLY_VLOG_LYS, node, "Invalid when condition \"%s\".", when->cond);
            }
            ret = -1;
            goto finish;
        } else {
            if (set_ext_dep_flags) {
                for (j = 0; j < tmp_set.used; ++j) {
                    /* skip roots'n'stuff */
                    if (tmp_set.val.snodes[j].type == LLLYXP_NODE_ELEM) {
                        /* XPath expression cannot reference "lower" status than the node that has the definition */
                        if (lllyp_check_status(node->flags, lllys_node_module(node), node->name, tmp_set.val.snodes[j].snode->flags,
                                lllys_node_module(tmp_set.val.snodes[j].snode), tmp_set.val.snodes[j].snode->name, node)) {
                            ret = -1;
                            goto finish;
                        }

                        if (parent) {
                            for (elem = tmp_set.val.snodes[j].snode; elem && (elem != parent); elem = lllys_parent(elem));
                            if (!elem) {
                                /* not in node's RPC or notification subtree, set the correct dep flag */
                                if (tmp_set.val.snodes[j].snode->flags & LLLYS_CONFIG_W) {
                                    when->flags |= LLLYS_XPCONF_DEP;
                                    ((struct lllys_node *)node)->flags |= LLLYS_XPCONF_DEP;
                                } else {
                                    assert(tmp_set.val.snodes[j].snode->flags & LLLYS_CONFIG_R);
                                    when->flags |= LLLYS_XPSTATE_DEP;
                                    ((struct lllys_node *)node)->flags |= LLLYS_XPSTATE_DEP;
                                }
                            }
                        }
                    }
                }
            }
            set_snode_merge(set, &tmp_set);
            memset(&tmp_set, 0, sizeof tmp_set);
        }
    }

    /* check "must" */
    for (i = 0; i < must_size; ++i) {
        if (lllyxp_atomize(must[i].expr, node, LLLYXP_NODE_ELEM, &tmp_set, LLLYXP_SNODE_MUST | opts, &ctx_snode)) {
            free(tmp_set.val.snodes);
            if (ctx_snode) {
                path = lllys_path(ctx_snode, LLLYS_PATH_FIRST_PREFIX);
                LOGVAL(node->module->ctx, LLLYE_SPEC, LLLY_VLOG_LYS, node,
                       "Invalid must restriction \"%s\" with context node \"%s\".", must[i].expr, path);
            } else {
                LOGVAL(node->module->ctx, LLLYE_SPEC, LLLY_VLOG_LYS, node, "Invalid must restriction \"%s\".", must[i].expr);
            }
            ret = -1;
            goto finish;
        } else {
            if (set_ext_dep_flags) {
                for (j = 0; j < tmp_set.used; ++j) {
                    /* skip roots'n'stuff */
                    if (tmp_set.val.snodes[j].type == LLLYXP_NODE_ELEM) {
                        /* XPath expression cannot reference "lower" status than the node that has the definition */
                        if (lllyp_check_status(node->flags, lllys_node_module(node), node->name, tmp_set.val.snodes[j].snode->flags,
                                lllys_node_module(tmp_set.val.snodes[j].snode), tmp_set.val.snodes[j].snode->name, node)) {
                            ret = -1;
                            goto finish;
                        }

                        if (parent) {
                            for (elem = tmp_set.val.snodes[j].snode; elem && (elem != parent); elem = lllys_parent(elem));
                            if (!elem) {
                                /* not in node's RPC or notification subtree, set the correct dep flag */
                                if (tmp_set.val.snodes[j].snode->flags & LLLYS_CONFIG_W) {
                                    must[i].flags |= LLLYS_XPCONF_DEP;
                                    ((struct lllys_node *)node)->flags |= LLLYS_XPCONF_DEP;
                                } else if (tmp_set.val.snodes[j].snode->flags & LLLYS_CONFIG_R) {
                                    must[i].flags |= LLLYS_XPSTATE_DEP;
                                    ((struct lllys_node *)node)->flags |= LLLYS_XPSTATE_DEP;
                                } else {
                                    /* only possible if the node is in an unimplemented augment */
                                    elem = tmp_set.val.snodes[j].snode;
                                    while (elem && (elem->nodetype != LLLYS_AUGMENT)) {
                                        elem = elem->parent;
                                    }
                                    assert(elem && !lllys_node_module(elem)->implemented);
                                }
                            }
                        }
                    }
                }
            }
            set_snode_merge(set, &tmp_set);
            memset(&tmp_set, 0, sizeof tmp_set);
        }
    }

finish:
    if (ret) {
        free(set->val.snodes);
        memset(set, 0, sizeof *set);
    }
    free(path);
    return ret;
}

int
lllyxp_node_check_syntax(const struct lllys_node *node)
{
    uint8_t must_size = 0;
    uint16_t exp_idx;
    uint32_t i;
    struct lllys_when *when = NULL;
    struct lllys_restr *must = NULL;
    struct lllyxp_expr *expr;

    switch (node->nodetype) {
    case LLLYS_CONTAINER:
        when = ((struct lllys_node_container *)node)->when;
        must = ((struct lllys_node_container *)node)->must;
        must_size = ((struct lllys_node_container *)node)->must_size;
        break;
    case LLLYS_CHOICE:
        when = ((struct lllys_node_choice *)node)->when;
        break;
    case LLLYS_LEAF:
        when = ((struct lllys_node_leaf *)node)->when;
        must = ((struct lllys_node_leaf *)node)->must;
        must_size = ((struct lllys_node_leaf *)node)->must_size;
        break;
    case LLLYS_LEAFLIST:
        when = ((struct lllys_node_leaflist *)node)->when;
        must = ((struct lllys_node_leaflist *)node)->must;
        must_size = ((struct lllys_node_leaflist *)node)->must_size;
        break;
    case LLLYS_LIST:
        when = ((struct lllys_node_list *)node)->when;
        must = ((struct lllys_node_list *)node)->must;
        must_size = ((struct lllys_node_list *)node)->must_size;
        break;
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        when = ((struct lllys_node_anydata *)node)->when;
        must = ((struct lllys_node_anydata *)node)->must;
        must_size = ((struct lllys_node_anydata *)node)->must_size;
        break;
    case LLLYS_CASE:
        when = ((struct lllys_node_case *)node)->when;
        break;
    case LLLYS_NOTIF:
        must = ((struct lllys_node_notif *)node)->must;
        must_size = ((struct lllys_node_notif *)node)->must_size;
        break;
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
        must = ((struct lllys_node_inout *)node)->must;
        must_size = ((struct lllys_node_inout *)node)->must_size;
        break;
    case LLLYS_USES:
        when = ((struct lllys_node_uses *)node)->when;
        break;
    case LLLYS_AUGMENT:
        when = ((struct lllys_node_augment *)node)->when;
        break;
    default:
        /* nothing to check */
        break;
    }

    /* check "when" */
    if (when) {
        expr = lllyxp_parse_expr(node->module->ctx, when->cond);
        if (!expr) {
            return -1;
        }

        exp_idx = 0;
        if (reparse_or_expr(node->module->ctx, expr, &exp_idx)) {
            lllyxp_expr_free(expr);
            return -1;
        } else if (exp_idx != expr->used) {
            LOGVAL(node->module->ctx, LLLYE_XPATH_INTOK, LLLY_VLOG_NONE, NULL,
                   print_token(expr->tokens[exp_idx]), &expr->expr[expr->expr_pos[exp_idx]]);
            lllyxp_expr_free(expr);
            return -1;
        }
        lllyxp_expr_free(expr);
    }

    /* check "must" */
    for (i = 0; i < must_size; ++i) {
        expr = lllyxp_parse_expr(node->module->ctx, must[i].expr);
        if (!expr) {
            return -1;
        }

        exp_idx = 0;
        if (reparse_or_expr(node->module->ctx, expr, &exp_idx)) {
            lllyxp_expr_free(expr);
            return -1;
        } else if (exp_idx != expr->used) {
            LOGVAL(node->module->ctx, LLLYE_XPATH_INTOK, LLLY_VLOG_NONE, NULL,
                   print_token(expr->tokens[exp_idx]), &expr->expr[expr->expr_pos[exp_idx]]);
            lllyxp_expr_free(expr);
            return -1;
        }
        lllyxp_expr_free(expr);
    }

    return 0;
}
