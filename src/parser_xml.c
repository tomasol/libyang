/**
 * @file parser_xml.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief XML data parser for libyang
 *
 * Copyright (c) 2015 - 2017 CESNET, z.s.p.o.
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
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "libyang.h"
#include "common.h"
#include "context.h"
#include "parser.h"
#include "tree_internal.h"
#include "validation.h"
#include "xml_internal.h"

/* does not log */
static struct lllys_node *
xml_data_search_schemanode(struct lllyxml_elem *xml, struct lllys_node *start, int options)
{
    struct lllys_node *result, *aux;

    LLLY_TREE_FOR(start, result) {
        /* skip groupings ... */
        if (result->nodetype == LLLYS_GROUPING) {
            continue;
        /* ... and output in case of RPC ... */
        } else if (result->nodetype == LLLYS_OUTPUT && (options & LLLYD_OPT_RPC)) {
            continue;
        /* ... and input in case of RPC reply */
        } else if (result->nodetype == LLLYS_INPUT && (options & LLLYD_OPT_RPCREPLY)) {
            continue;
        }

        /* go into cases, choices, uses and in RPCs into input and output */
        if (result->nodetype & (LLLYS_CHOICE | LLLYS_CASE | LLLYS_USES | LLLYS_INPUT | LLLYS_OUTPUT)) {
            aux = xml_data_search_schemanode(xml, result->child, options);
            if (aux) {
                /* we have matching result */
                return aux;
            }
            /* else, continue with next schema node */
            continue;
        }

        /* match data nodes */
        if (llly_strequal(result->name, xml->name, 1)) {
            /* names matches, what about namespaces? */
            if (llly_strequal(lllys_main_module(result->module)->ns, xml->ns->value, 1)) {
                /* we have matching result */
                return result;
            }
            /* else, continue with next schema node */
            continue;
        }
    }

    /* no match */
    return NULL;
}

/* logs directly */
static int
xml_get_value(struct lllyd_node *node, struct lllyxml_elem *xml, int editbits, int trusted)
{
    struct lllyd_node_leaf_list *leaf = (struct lllyd_node_leaf_list *)node;

    assert(node && (node->schema->nodetype & (LLLYS_LEAFLIST | LLLYS_LEAF)) && xml);

    leaf->value_str = lllydict_insert(node->schema->module->ctx, xml->content, 0);

    if ((editbits & 0x20) && (node->schema->nodetype & LLLYS_LEAF) && (!leaf->value_str || !leaf->value_str[0])) {
        /* we have edit-config leaf/leaf-list with delete operation and no (empty) value,
         * this is not a bug since the node is just used as a kind of selection node */
        leaf->value_type = LLLY_TYPE_UNKNOWN;
        return EXIT_SUCCESS;
    }

    /* the value is here converted to a JSON format if needed in case of LLLY_TYPE_IDENT and LLLY_TYPE_INST or to a
     * canonical form of the value */
    if (!lllyp_parse_value(&((struct lllys_node_leaf *)leaf->schema)->type, &leaf->value_str, xml, leaf, NULL, NULL, 1, 0, trusted)) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/* logs directly */
static int
xml_parse_data(struct llly_ctx *ctx, struct lllyxml_elem *xml, struct lllyd_node *parent, struct lllyd_node *first_sibling,
               struct lllyd_node *prev, int options, struct unres_data *unres, struct lllyd_node **result,
               struct lllyd_node **act_notif, const char *yang_data_name)
{
    const struct lllys_module *mod = NULL;
    struct lllyd_node *diter, *dlast;
    struct lllys_node *schema = NULL, *target;
    const struct lllys_node *ext_node;
    struct lllys_node_augment *aug;
    struct lllyd_attr *dattr, *dattr_iter;
    struct lllyxml_attr *attr;
    struct lllyxml_elem *child, *next;
    int i, j, havechildren, r, editbits = 0, filterflag = 0, found;
    uint8_t pos;
    int ret = 0;
    const char *str = NULL;
    char *msg;

    assert(xml);
    assert(result);
    *result = NULL;

    if (xml->flags & LLLYXML_ELEM_MIXED) {
        if (options & LLLYD_OPT_STRICT) {
            LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_XML, xml, "XML element with mixed content");
            return -1;
        } else {
            return 0;
        }
    }

    if (!xml->ns || !xml->ns->value) {
        if (options & LLLYD_OPT_STRICT) {
            LOGVAL(ctx, LLLYE_XML_MISS, LLLY_VLOG_XML, xml, "element's", "namespace");
            return -1;
        } else {
            return 0;
        }
    }

    /* find schema node */
    if (!parent) {
        mod = llly_ctx_get_module_by_ns(ctx, xml->ns->value, NULL, 0);
        if (ctx->data_clb) {
            if (!mod) {
                mod = ctx->data_clb(ctx, NULL, xml->ns->value, 0, ctx->data_clb_data);
            } else if (!mod->implemented) {
                mod = ctx->data_clb(ctx, mod->name, mod->ns, LLLY_MODCLB_NOT_IMPLEMENTED, ctx->data_clb_data);
            }
        }

        /* get the proper schema node */
        if (mod && mod->implemented && !mod->disabled) {
            if (options & LLLYD_OPT_DATA_TEMPLATE) {
                if (yang_data_name) {
                    ext_node = lllyp_get_yang_data_template(mod, yang_data_name, strlen(yang_data_name));
                    if (ext_node) {
                        schema = *((struct lllys_node **) lllys_ext_complex_get_substmt(LLLY_STMT_CONTAINER, (struct lllys_ext_instance_complex *)ext_node, NULL));
                        schema = xml_data_search_schemanode(xml, schema, options);
                    }
                }
            } else {
                schema = xml_data_search_schemanode(xml, mod->data, options);
                if (!schema) {
                    /* it still can be the specific case of this module containing an augment of another module
                    * top-level choice or top-level choice's case, bleh */
                    for (j = 0; j < mod->augment_size; ++j) {
                        aug = &mod->augment[j];
                        target = aug->target;
                        if (target->nodetype & (LLLYS_CHOICE | LLLYS_CASE)) {
                            /* 1) okay, the target is choice or case */
                            while (target && (target->nodetype & (LLLYS_CHOICE | LLLYS_CASE | LLLYS_USES))) {
                                target = lllys_parent(target);
                            }
                            /* 2) now, the data node will be top-level, there are only non-data schema nodes */
                            if (!target) {
                                while ((schema = (struct lllys_node *) lllys_getnext(schema, (struct lllys_node *) aug, NULL, 0))) {
                                    /* 3) alright, even the name matches, we found our schema node */
                                    if (llly_strequal(schema->name, xml->name, 1)) {
                                        break;
                                    }
                                }
                            }
                        }

                        if (schema) {
                            break;
                        }
                    }
                }
            }
        }
    } else {
        /* parsing some internal node, we start with parent's schema pointer */
        schema = xml_data_search_schemanode(xml, parent->schema->child, options);

        if (ctx->data_clb) {
            if (schema && !lllys_node_module(schema)->implemented) {
                ctx->data_clb(ctx, lllys_node_module(schema)->name, lllys_node_module(schema)->ns,
                              LLLY_MODCLB_NOT_IMPLEMENTED, ctx->data_clb_data);
            } else if (!schema) {
                if (ctx->data_clb(ctx, NULL, xml->ns->value, 0, ctx->data_clb_data)) {
                    /* context was updated, so try to find the schema node again */
                    schema = xml_data_search_schemanode(xml, parent->schema->child, options);
                }
            }
        }
    }

    mod = lllys_node_module(schema);
    if (!mod || !mod->implemented || mod->disabled) {
        if (options & LLLYD_OPT_STRICT) {
            LOGVAL(ctx, LLLYE_INELEM, (parent ? LLLY_VLOG_LYD : LLLY_VLOG_STR), (parent ? (void *)parent : (void *)"/") , xml->name);
            return -1;
        } else {
            return 0;
        }
    }

    /* create the element structure */
    switch (schema->nodetype) {
    case LLLYS_CONTAINER:
    case LLLYS_LIST:
    case LLLYS_NOTIF:
    case LLLYS_RPC:
    case LLLYS_ACTION:
        for (i = 0; xml->content && xml->content[i]; ++i) {
            if (!is_xmlws(xml->content[i])) {
                msg = malloc(22 + strlen(xml->content) + 1);
                LLLY_CHECK_ERR_RETURN(!msg, LOGMEM(ctx), -1);
                sprintf(msg, "node with text data \"%s\"", xml->content);
                LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_XML, xml, msg);
                free(msg);
                return -1;
            }
        }
        *result = calloc(1, sizeof **result);
        havechildren = 1;
        break;
    case LLLYS_LEAF:
    case LLLYS_LEAFLIST:
        *result = calloc(1, sizeof(struct lllyd_node_leaf_list));
        havechildren = 0;
        break;
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        *result = calloc(1, sizeof(struct lllyd_node_anydata));
        havechildren = 0;
        break;
    default:
        LOGINT(ctx);
        return -1;
    }
    LLLY_CHECK_ERR_RETURN(!(*result), LOGMEM(ctx), -1);

    (*result)->prev = *result;
    (*result)->schema = schema;
    (*result)->parent = parent;
    diter = NULL;
    if (schema->nodetype == LLLYS_LEAF && lllys_is_key((struct lllys_node_leaf *)schema, &pos)) {
        /* it is key and we need to insert it into a correct place (a key must have a parent list) */
        assert(parent);
        for (i = 0, diter = parent->child;
                diter && i < pos && diter->schema->nodetype == LLLYS_LEAF && lllys_is_key((struct lllys_node_leaf *)diter->schema, NULL);
                i++, diter = diter->next);
        if (diter) {
            /* out of order insertion - insert list's key to the correct position, before the diter */
            if (options & LLLYD_OPT_STRICT) {
                LOGVAL(ctx, LLLYE_INORDER, LLLY_VLOG_LYD, *result, schema->name, diter->schema->name);
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "Invalid position of the key \"%s\" in a list \"%s\".",
                       schema->name, parent->schema->name);
                free(*result);
                *result = NULL;
                return -1;
            } else {
                LOGWRN(ctx, "Invalid position of the key \"%s\" in a list \"%s\".", schema->name, parent->schema->name)
            }
            if (parent->child == diter) {
                parent->child = *result;
                /* update first_sibling */
                first_sibling = *result;
            }
            if (diter->prev->next) {
                diter->prev->next = *result;
            }
            (*result)->prev = diter->prev;
            diter->prev = *result;
            (*result)->next = diter;
        }
    }
    if (!diter) {
        /* simplified (faster) insert as the last node */
        if (parent && !parent->child) {
            parent->child = *result;
        }
        if (prev) {
            (*result)->prev = prev;
            prev->next = *result;

            /* fix the "last" pointer */
            first_sibling->prev = *result;
        } else {
            (*result)->prev = *result;
            first_sibling = *result;
        }
    }
    (*result)->validity = llly_new_node_validity((*result)->schema);
    if (resolve_applies_when(schema, 0, NULL)) {
        (*result)->when_status = LLLYD_WHEN;
    }

    /* process attributes */
    for (attr = xml->attr; attr; attr = attr->next) {
        if (attr->type != LLLYXML_ATTR_STD) {
            continue;
        } else if (!attr->ns) {
            if ((*result)->schema->nodetype == LLLYS_ANYXML &&
                    llly_strequal((*result)->schema->name, "filter", 0) &&
                    (llly_strequal((*result)->schema->module->name, "ietf-netconf", 0) ||
                    llly_strequal((*result)->schema->module->name, "notifications", 0))) {
                /* NETCONF filter's attributes, which we implement as non-standard annotations,
                 * they are unqualified (no namespace), but we know that we have internally defined
                 * them in the ietf-netconf module */
                str = LLLY_NSNC;
                filterflag = 1;
            } else {
                /* garbage */
                goto attr_error;
            }
        } else {
            str = attr->ns->value;
        }

        r = lllyp_fill_attr(ctx, *result, str, NULL, attr->name, attr->value, xml, options, &dattr);
        if (r == -1) {
            goto unlink_node_error;
        } else if (r == 1) {
attr_error:
            if (options & LLLYD_OPT_STRICT) {
                LOGVAL(ctx, LLLYE_INATTR, LLLY_VLOG_LYD, *result, attr->name);
                goto unlink_node_error;
            }

            LOGWRN(ctx, "Unknown \"%s:%s\" metadata with value \"%s\", ignoring.",
                   (attr->ns ? attr->ns->prefix : "<none>"), attr->name, attr->value);
            continue;
        }

        /* special case of xpath in the value, we want to convert it to JSON */
        if (filterflag && !strcmp(attr->name, "select")) {
            dattr->value.string = transform_xml2json(ctx, dattr->value_str, xml, 0, 0);
            if (!dattr->value.string) {
                /* problem with resolving value as xpath */
                dattr->value.string = dattr->value_str;
                goto unlink_node_error;
            }
            lllydict_remove(ctx, dattr->value_str);
            dattr->value_str = dattr->value.string;
        }

        /* insert into the data node */
        if (!(*result)->attr) {
            (*result)->attr = dattr;
        } else {
            for (dattr_iter = (*result)->attr; dattr_iter->next; dattr_iter = dattr_iter->next);
            dattr_iter->next = dattr;
        }
        continue;
    }

    /* check insert attribute and its values */
    if (options & LLLYD_OPT_EDIT) {
        if (lllyp_check_edit_attr(ctx, (*result)->attr, *result, &editbits)) {
            goto unlink_node_error;
        }

    /* check correct filter extension attributes */
    } else if (filterflag) {
        found = 0; /* 0 - nothing, 1 - type subtree, 2 - type xpath, 3 - select, 4 - type xpath + select */
        LLLY_TREE_FOR((*result)->attr, dattr_iter) {
            if (!strcmp(dattr_iter->name, "type")) {
                if ((found == 1) || (found == 2) || (found == 4)) {
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYD, (*result), "type", xml->name);
                    goto unlink_node_error;
                }
                switch (dattr_iter->value.enm->value) {
                case 0:
                    /* subtree */
                    if (found == 3) {
                        LOGVAL(ctx, LLLYE_INATTR, LLLY_VLOG_LYD, (*result), dattr_iter->name);
                        goto unlink_node_error;
                    }

                    assert(!found);
                    found = 1;
                    break;
                case 1:
                    /* xpath */
                    if (found == 3) {
                        found = 4;
                    } else {
                        assert(!found);
                        found = 2;
                    }
                    break;
                default:
                    LOGINT(ctx);
                    goto unlink_node_error;
                }
            } else if (!strcmp(dattr_iter->name, "select")) {
                switch (found) {
                case 0:
                    found = 3;
                    break;
                case 1:
                    LOGVAL(ctx, LLLYE_INATTR, LLLY_VLOG_LYD, (*result), dattr_iter->name);
                    goto unlink_node_error;
                case 2:
                    found = 4;
                    break;
                case 3:
                case 4:
                    LOGVAL(ctx, LLLYE_TOOMANY, LLLY_VLOG_LYD, (*result), "select", xml->name);
                    goto unlink_node_error;
                default:
                    LOGINT(ctx);
                    goto unlink_node_error;
                }
            }
        }

        /* check if what we found is correct */
        switch (found) {
        case 1:
        case 4:
            /* ok */
            break;
        case 2:
            LOGVAL(ctx, LLLYE_MISSATTR, LLLY_VLOG_LYD, (*result), "select", xml->name);
            goto unlink_node_error;
        case 3:
            LOGVAL(ctx, LLLYE_MISSATTR, LLLY_VLOG_LYD, (*result), "type", xml->name);
            goto unlink_node_error;
        default:
            LOGINT(ctx);
            goto unlink_node_error;
        }
    }

    /* type specific processing */
    if (schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) {
        /* type detection and assigning the value */
        if (xml_get_value(*result, xml, editbits, options & LLLYD_OPT_TRUSTED)) {
            goto unlink_node_error;
        }
    } else if (schema->nodetype & LLLYS_ANYDATA) {
        /* store children values */
        if (xml->child) {
            child = xml->child;
            /* manually unlink all siblings and correct namespaces */
            xml->child = NULL;
            LLLY_TREE_FOR(child, next) {
                next->parent = NULL;
                lllyxml_correct_elem_ns(ctx, next, 1, 1);
            }

            ((struct lllyd_node_anydata *)*result)->value_type = LLLYD_ANYDATA_XML;
            ((struct lllyd_node_anydata *)*result)->value.xml = child;
        } else {
            ((struct lllyd_node_anydata *)*result)->value_type = LLLYD_ANYDATA_CONSTSTRING;
            ((struct lllyd_node_anydata *)*result)->value.str = lllydict_insert(ctx, xml->content, 0);
        }
    } else if (schema->nodetype & (LLLYS_RPC | LLLYS_ACTION)) {
        if (!(options & LLLYD_OPT_RPC) || *act_notif) {
            LOGVAL(ctx, LLLYE_INELEM, LLLY_VLOG_LYD, (*result), schema->name);
            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "Unexpected %s node \"%s\".",
                   (schema->nodetype == LLLYS_RPC ? "rpc" : "action"), schema->name);
            goto unlink_node_error;
        }
        *act_notif = *result;
    } else if (schema->nodetype == LLLYS_NOTIF) {
        if (!(options & LLLYD_OPT_NOTIF) || *act_notif) {
            LOGVAL(ctx, LLLYE_INELEM, LLLY_VLOG_LYD, (*result), schema->name);
            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "Unexpected notification node \"%s\".", schema->name);
            goto unlink_node_error;
        }
        *act_notif = *result;
    }

#ifdef LLLY_ENABLED_CACHE
    /* calculate the hash and insert it into parent (list with keys is handled when its keys are inserted) */
    if (((*result)->schema->nodetype != LLLYS_LIST) || !((struct lllys_node_list *)(*result)->schema)->keys_size) {
        lllyd_hash(*result);
        lllyd_insert_hash(*result);
    }
#endif

    /* first part of validation checks */
    if (lllyv_data_context(*result, options, unres)) {
        goto error;
    }

    /* process children */
    if (havechildren && xml->child) {
        diter = dlast = NULL;
        LLLY_TREE_FOR_SAFE(xml->child, next, child) {
            r = xml_parse_data(ctx, child, *result, (*result)->child, dlast, options, unres, &diter, act_notif, yang_data_name);
            if (r) {
                goto error;
            } else if (options & LLLYD_OPT_DESTRUCT) {
                lllyxml_free(ctx, child);
            }
            if (diter && !diter->next) {
                /* the child was parsed/created and it was placed as the last child. The child can be inserted
                 * out of order (not as the last one) in case it is a list's key present out of the correct order */
                dlast = diter;
            }
        }
    }

    /* if we have empty non-presence container, we keep it, but mark it as default */
    if (schema->nodetype == LLLYS_CONTAINER && !(*result)->child &&
            !(*result)->attr && !((struct lllys_node_container *)schema)->presence) {
        (*result)->dflt = 1;
    }

    /* rest of validation checks */
    if (lllyv_data_content(*result, options, unres) ||
            lllyv_multicases(*result, NULL, prev ? &first_sibling : NULL, 0, NULL)) {
        goto error;
    }

    /* validation successful */
    if ((*result)->schema->nodetype & (LLLYS_LIST | LLLYS_LEAFLIST)) {
        /* postpone checking when there will be all list/leaflist instances */
        (*result)->validity |= LLLYD_VAL_DUP;
    }

    return ret;

unlink_node_error:
    lllyd_unlink_internal(*result, 2);
error:
    /* cleanup */
    for (i = unres->count - 1; i >= 0; i--) {
        /* remove unres items connected with the node being removed */
        if (unres->node[i] == *result) {
            unres_data_del(unres, i);
        }
    }
    lllyd_free(*result);
    *result = NULL;
    return -1;
}

API struct lllyd_node *
lllyd_parse_xml(struct llly_ctx *ctx, struct lllyxml_elem **root, int options, ...)
{
    FUN_IN;

    va_list ap;
    int r;
    struct unres_data *unres = NULL;
    const struct lllyd_node *rpc_act = NULL, *data_tree = NULL;
    struct lllyd_node *result = NULL, *iter, *last, *reply_parent = NULL, *reply_top = NULL, *act_notif = NULL;
    struct lllyxml_elem *xmlstart, *xmlelem, *xmlaux, *xmlfree = NULL;
    const char *yang_data_name = NULL;

    if (!ctx || !root) {
        LOGARG;
        return NULL;
    }

    if (lllyp_data_check_options(ctx, options, __func__)) {
        return NULL;
    }

    if (!(*root) && !(options & LLLYD_OPT_RPCREPLY)) {
        /* empty tree */
        if (options & (LLLYD_OPT_RPC | LLLYD_OPT_NOTIF)) {
            /* error, top level node identify RPC and Notification */
            LOGERR(ctx, LLLY_EINVAL, "%s: *root identifies RPC/Notification so it cannot be NULL.", __func__);
            return NULL;
        } else if (!(options & LLLYD_OPT_RPCREPLY)) {
            /* others - no work is needed, just check for missing mandatory nodes */
            lllyd_validate(&result, options, ctx);
            return result;
        }
        /* continue with empty RPC reply, for which we need RPC */
    }

    unres = calloc(1, sizeof *unres);
    LLLY_CHECK_ERR_RETURN(!unres, LOGMEM(ctx), NULL);

    va_start(ap, options);
    if (options & LLLYD_OPT_RPCREPLY) {
        rpc_act = va_arg(ap, const struct lllyd_node *);
        if (!rpc_act || rpc_act->parent || !(rpc_act->schema->nodetype & (LLLYS_RPC | LLLYS_LIST | LLLYS_CONTAINER))) {
            LOGERR(ctx, LLLY_EINVAL, "%s: invalid variable parameter (const struct lllyd_node *rpc_act).", __func__);
            goto error;
        }
        if (rpc_act->schema->nodetype == LLLYS_RPC) {
            /* RPC request */
            reply_top = reply_parent = _lyd_new(NULL, rpc_act->schema, 0);
        } else {
            /* action request */
            reply_top = lllyd_dup(rpc_act, 1);
            LLLY_TREE_DFS_BEGIN(reply_top, iter, reply_parent) {
                if (reply_parent->schema->nodetype == LLLYS_ACTION) {
                    break;
                }
                LLLY_TREE_DFS_END(reply_top, iter, reply_parent);
            }
            if (!reply_parent) {
                LOGERR(ctx, LLLY_EINVAL, "%s: invalid variable parameter (const struct lllyd_node *rpc_act).", __func__);
                lllyd_free_withsiblings(reply_top);
                goto error;
            }
            lllyd_free_withsiblings(reply_parent->child);
        }
    }
    if (options & (LLLYD_OPT_RPC | LLLYD_OPT_NOTIF | LLLYD_OPT_RPCREPLY)) {
        data_tree = va_arg(ap, const struct lllyd_node *);
        if (data_tree) {
            if (options & LLLYD_OPT_NOEXTDEPS) {
                LOGERR(ctx, LLLY_EINVAL, "%s: invalid parameter (variable arg const struct lllyd_node *data_tree and LLLYD_OPT_NOEXTDEPS set).",
                       __func__);
                goto error;
            }

            LLLY_TREE_FOR((struct lllyd_node *)data_tree, iter) {
                if (iter->parent) {
                    /* a sibling is not top-level */
                    LOGERR(ctx, LLLY_EINVAL, "%s: invalid variable parameter (const struct lllyd_node *data_tree).", __func__);
                    goto error;
                }
            }

            /* move it to the beginning */
            for (; data_tree->prev->next; data_tree = data_tree->prev);

            /* LLLYD_OPT_NOSIBLINGS cannot be set in this case */
            if (options & LLLYD_OPT_NOSIBLINGS) {
                LOGERR(ctx, LLLY_EINVAL, "%s: invalid parameter (variable arg const struct lllyd_node *data_tree with LLLYD_OPT_NOSIBLINGS).", __func__);
                goto error;
            }
        }
    }
    if (options & LLLYD_OPT_DATA_TEMPLATE) {
        yang_data_name = va_arg(ap, const char *);
    }

    if ((*root) && !(options & LLLYD_OPT_NOSIBLINGS)) {
        /* locate the first root to process */
        if ((*root)->parent) {
            xmlstart = (*root)->parent->child;
        } else {
            xmlstart = *root;
            while(xmlstart->prev->next) {
                xmlstart = xmlstart->prev;
            }
        }
    } else {
        xmlstart = *root;
    }

    if ((options & LLLYD_OPT_RPC)
            && !strcmp(xmlstart->name, "action") && !strcmp(xmlstart->ns->value, LLLY_NSYANG)) {
        /* it's an action, not a simple RPC */
        xmlstart = xmlstart->child;
        if (options & LLLYD_OPT_DESTRUCT) {
            /* free it later */
            xmlfree = xmlstart->parent;
        }
    }

    iter = last = NULL;
    LLLY_TREE_FOR_SAFE(xmlstart, xmlaux, xmlelem) {
        r = xml_parse_data(ctx, xmlelem, reply_parent, result, last, options, unres, &iter, &act_notif, yang_data_name);
        if (r) {
            if (reply_top) {
                result = reply_top;
            }
            goto error;
        } else if (options & LLLYD_OPT_DESTRUCT) {
            lllyxml_free(ctx, xmlelem);
            *root = xmlaux;
        }
        if (iter) {
            last = iter;
            if ((options & LLLYD_OPT_DATA_ADD_YANGLIB) && iter->schema->module == ctx->models.list[ctx->internal_module_count - 1]) {
                /* ietf-yang-library data present, so ignore the option to add them */
                options &= ~LLLYD_OPT_DATA_ADD_YANGLIB;
            }
        }
        if (!result) {
            result = iter;
        }

        if (options & LLLYD_OPT_NOSIBLINGS) {
            /* stop after the first processed root */
            break;
        }
    }

    if (reply_top) {
        result = reply_top;
    }

    if ((options & LLLYD_OPT_RPCREPLY) && (rpc_act->schema->nodetype != LLLYS_RPC)) {
        /* action reply */
        act_notif = reply_parent;
    } else if ((options & (LLLYD_OPT_RPC | LLLYD_OPT_NOTIF)) && !act_notif) {
        LOGVAL(ctx, LLLYE_INELEM, (result ? LLLY_VLOG_LYD : LLLY_VLOG_NONE), result, (options & LLLYD_OPT_RPC ? "action" : "notification"));
        goto error;
    }

    /* add missing ietf-yang-library if requested */
    if (options & LLLYD_OPT_DATA_ADD_YANGLIB) {
        if (!result) {
            result = llly_ctx_info(ctx);
        } else if (lllyd_merge(result, llly_ctx_info(ctx), LLLYD_OPT_DESTRUCT | LLLYD_OPT_EXPLICIT)) {
            LOGERR(ctx, LLLY_EINT, "Adding ietf-yang-library data failed.");
            goto error;
        }
    }

    /* check for uniqueness of top-level lists/leaflists because
     * only the inner instances were tested in lllyv_data_content() */
    LLLY_TREE_FOR(result, iter) {
        if (!(iter->schema->nodetype & (LLLYS_LIST | LLLYS_LEAFLIST)) || !(iter->validity & LLLYD_VAL_DUP)) {
            continue;
        }

        if (lllyv_data_dup(iter, result)) {
            goto error;
        }
    }

    /* add default values, resolve unres and check for mandatory nodes in final tree */
    if (lllyd_defaults_add_unres(&result, options, ctx, NULL, 0, data_tree, act_notif, unres, 1)) {
        goto error;
    }
    if (!(options & (LLLYD_OPT_TRUSTED | LLLYD_OPT_NOTIF_FILTER))
            && lllyd_check_mandatory_tree((act_notif ? act_notif : result), ctx, NULL, 0, options)) {
        goto error;
    }

    if (xmlfree) {
        lllyxml_free(ctx, xmlfree);
    }
    free(unres->node);
    free(unres->type);
    free(unres);
    va_end(ap);
    return result;

error:
    lllyd_free_withsiblings(result);
    if (xmlfree) {
        lllyxml_free(ctx, xmlfree);
    }
    free(unres->node);
    free(unres->type);
    free(unres);
    va_end(ap);
    return NULL;
}
