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

int
lyv_keys(const struct lyd_node *list)
{
    struct lyd_node *child;
    struct lys_node_list *schema = (struct lys_node_list *)list->schema; /* shortcut */
    int i;

    for (i = 0, child = list->child; i < schema->keys_size; i++, child = child->next) {
        if (!child || child->schema != (struct lys_node *)schema->keys[i]) {
            /* key not found on the correct place */
            LOGVAL(LYE_MISSELEM, LY_VLOG_LYD, list, schema->keys[i]->name, schema->name);
            for ( ; child; child = child->next) {
                if (child->schema == (struct lys_node *)schema->keys[i]) {
                    LOGVAL(LYE_SPEC, LY_VLOG_LYD, child, "Invalid position of the key element.");
                    break;
                }
            }
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}

int
lyv_data_context(const struct lyd_node *node, int options, struct unres_data *unres)
{
    const struct lys_node *siter = NULL;

    assert(node);
    assert(unres);

    /* check if the node instance is enabled by if-feature */
    if (lys_is_disabled(node->schema, 2)) {
        LOGVAL(LYE_INELEM, LY_VLOG_LYD, node, node->schema->name);
        return EXIT_FAILURE;
    }

    /* check leafref/instance-identifier */
    if ((node->schema->nodetype & (LYS_LEAF | LYS_LEAFLIST)) &&
            !(options & (LYD_OPT_EDIT | LYD_OPT_GET | LYD_OPT_GETCONFIG))) {
        /* remove possible unres flags from type */
        ((struct lyd_node_leaf_list *)node)->value_type &= LY_DATA_TYPE_MASK;

        /* if leafref or instance-identifier, store the node for later resolving */
        if (((struct lyd_node_leaf_list *)node)->value_type == LY_TYPE_LEAFREF &&
                !((struct lyd_node_leaf_list *)node)->value.leafref) {
            if (unres_data_add(unres, (struct lyd_node *)node, UNRES_LEAFREF)) {
                return EXIT_FAILURE;
            }
        } else if (((struct lyd_node_leaf_list *)node)->value_type == LY_TYPE_INST) {
            if (unres_data_add(unres, (struct lyd_node *)node, UNRES_INSTID)) {
                return EXIT_FAILURE;
            }
        }
    }

    /* check all relevant when conditions */
    if ((!(options & LYD_OPT_TYPEMASK) || (options & LYD_OPT_CONFIG)) && (node->when_status & LYD_WHEN)) {
        if (unres_data_add(unres, (struct lyd_node *)node, UNRES_WHEN)) {
            return EXIT_FAILURE;
        }
    }

    /* check for (non-)presence of status data in edit-config data */
    if ((options & (LYD_OPT_EDIT | LYD_OPT_GETCONFIG | LYD_OPT_CONFIG)) && (node->schema->flags & LYS_CONFIG_R)) {
        LOGVAL(LYE_INELEM, LY_VLOG_LYD, node, node->schema->name);
        return EXIT_FAILURE;
    }

    /* check elements order in case of RPC's input and output */
    if (node->validity && lyp_is_rpc_action(node->schema)) {
        if ((node->prev != node) && node->prev->next) {
            for (siter = lys_getnext(node->schema, lys_parent(node->schema), node->schema->module, 0);
                    siter;
                    siter = lys_getnext(siter, lys_parent(siter), siter->module, 0)) {
                if (siter == node->prev->schema) {
                    /* data predecessor has the schema node after
                     * the schema node of the data node being checked */
                    LOGVAL(LYE_INORDER, LY_VLOG_LYD, node, node->schema->name, siter->name);
                    return EXIT_FAILURE;
                }
            }

        }
    }

    return EXIT_SUCCESS;
}

int
lyv_data_content(struct lyd_node *node, int options, struct unres_data *unres)
{
    const struct lys_node *schema, *siter;
    struct lyd_node *diter, *start = NULL;
    struct lys_ident *ident;
    struct lys_tpdf *tpdf;
    struct lys_type *type;
    struct lyd_node_leaf_list *leaf;
    int i, j;
    uint8_t iff_size;
    struct lys_iffeature *iff;
    const char *id, *idname;

    assert(node);
    assert(node->schema);
    assert(unres);

    schema = node->schema; /* shortcut */

    if (node->validity & LYD_VAL_MAND) {
        /* check presence and correct order of all keys in case of list */
        if (schema->nodetype == LYS_LIST && !(options & (LYD_OPT_GET | LYD_OPT_GETCONFIG))) {
            if (lyv_keys(node)) {
                return EXIT_FAILURE;
            }
        }

        /* check for mandatory children */
        if ((schema->nodetype & (LYS_CONTAINER | LYS_LIST))
                && !(options & (LYD_OPT_EDIT | LYD_OPT_GET | LYD_OPT_GETCONFIG | LYD_OPT_ACTION))) {
            if (ly_check_mandatory(node, NULL, (options & LYD_OPT_TYPEMASK) ? 0 : 1, (options & LYD_OPT_RPCREPLY) ? 1 : 0)) {
                return EXIT_FAILURE;
            }
        } else if (schema->nodetype & (LYS_CONTAINER | LYS_LEAF | LYS_ANYXML)) {
            /* check number of instances (similar to list uniqueness) for non-list nodes */

            /* find duplicity */
            start = lyd_first_sibling(node);
            for (diter = start; diter; diter = diter->next) {
                if (diter->schema == schema && diter != node) {
                    LOGVAL(LYE_TOOMANY, LY_VLOG_LYD, node, schema->name,
                           lys_parent(schema) ? lys_parent(schema)->name : "data tree");
                    return EXIT_FAILURE;
                }
            }
        }

        /* remove the flag */
        node->validity &= ~LYD_VAL_MAND;
    }

    if ((schema->nodetype & (LYS_LIST | LYS_LEAFLIST)) && (node->validity & LYD_VAL_UNIQUE)) {
        /* get the first list/leaflist instance sibling */
        if (options & (LYD_OPT_GET | LYD_OPT_GETCONFIG)) {
            /* skip key uniqueness check in case of get/get-config data */
            start = NULL;
        } else {
            diter = start ? start : lyd_first_sibling(node);
            start = NULL;
            while (diter) {
                if (diter == node) {
                    diter = diter->next;
                    continue;
                }

                if (diter->schema == node->schema) {
                    /* the same list instance */
                    start = diter;
                    break;
                }
                diter = diter->next;
            }
        }

        /* check uniqueness of the list/leaflist instances (compare values) */
        for (diter = start; diter; diter = diter->next) {
            if (diter->schema != node->schema || diter == node || diter->validity) { /* skip comparison that will be done in future when checking diter as node */
                continue;
            }
            if (lyd_list_equal(diter, node, 1)) { /* comparing keys and unique combinations */
                return EXIT_FAILURE;
            }
        }

        /* remove the flag */
        node->validity &= ~LYD_VAL_UNIQUE;
    }

    if (node->validity) {
        /* status - of the node's schema node itself and all its parents that
         * cannot have their own instance (like a choice statement) */
        siter = node->schema;
        do {
            if (((siter->flags & LYS_STATUS_MASK) == LYS_STATUS_OBSLT) && (options & LYD_OPT_OBSOLETE)) {
                LOGVAL(LYE_OBSDATA, LY_VLOG_LYD, node, schema->name);
                return EXIT_FAILURE;
            }
            siter = lys_parent(siter);
        } while (siter && !(siter->nodetype & (LYS_CONTAINER | LYS_LEAF | LYS_LEAFLIST | LYS_LIST | LYS_ANYXML)));

        /* status of the identity value */
        if (schema->nodetype & (LYS_LEAF | LYS_LEAFLIST)) {
            if (options & LYD_OPT_OBSOLETE) {
                /* check that we are not instantiating obsolete type */
                tpdf = ((struct lys_node_leaf *)node->schema)->type.der;
                while (tpdf) {
                    if ((tpdf->flags & LYS_STATUS_MASK) == LYS_STATUS_OBSLT) {
                        LOGVAL(LYE_OBSTYPE, LY_VLOG_LYD, node, schema->name, tpdf->name);
                        return EXIT_FAILURE;
                    }
                    tpdf = tpdf->type.der;
                }
            }
            if (((struct lyd_node_leaf_list *)node)->value_type == LY_TYPE_IDENT) {
                ident = ((struct lyd_node_leaf_list *)node)->value.ident;
                if (lyp_check_status(schema->flags, schema->module, schema->name,
                                 ident->flags, ident->module, ident->name, NULL)) {
                    LOGPATH(LY_VLOG_LYD, node);
                    return EXIT_FAILURE;
                }
            }
        }
    }

    if (schema->nodetype & (LYS_LEAF | LYS_LEAFLIST)) {
        /* since feature can be enabled/disabled, do this check despite the validity flag,
         * - check if the type value (enum, bit, identity) is disabled via feature  */
        leaf = (struct lyd_node_leaf_list *)node;
        switch (leaf->value_type) {
        case LY_TYPE_BITS:
            id = "Bit";
            /* get the count of bits */
            for (type = &((struct lys_node_leaf *)leaf->schema)->type; !type->info.bits.count; type = &type->der->type);
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
        case LY_TYPE_ENUM:
            id = "Enum";
            idname = leaf->value_str;
            iff_size = leaf->value.enm->iffeature_size;
            iff = leaf->value.enm->iffeature;
            break;
        case LY_TYPE_IDENT:
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
                    LOGVAL(LYE_INVAL, LY_VLOG_LYD, node, leaf->value_str, schema->name);
                    LOGVAL(LYE_SPEC, LY_VLOG_LYD, node, "%s \"%s\" is disabled by its if-feature condition.",
                           id, idname);
                    return EXIT_FAILURE;
                }
            }
            if (leaf->value_type == LY_TYPE_BITS) {
                goto nextbit;
            }
        }
    }

    /* check must conditions */
    if (resolve_applies_must(node) && unres_data_add(unres, node, UNRES_MUST) == -1) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
