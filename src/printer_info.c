/**
 * @file printer/info.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief INFO printer for libyang data model structure
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

#include "common.h"
#include "printer.h"
#include "tree_schema.h"
#include "resolve.h"

#define INDENT_LEN 11

static void
info_print_text(struct lllyout *out, const char *text, const char *label)
{
    const char *ptr1, *ptr2;
    int first = 1;

    llly_print(out, "%-*s", INDENT_LEN, label);

    if (text) {
        ptr1 = text;
        while (1) {
            ptr2 = strchr(ptr1, '\n');
            if (!ptr2) {
                if (first) {
                    llly_print(out, "%s\n", ptr1);
                    first = 0;
                } else {
                    llly_print(out, "%*s%s\n", INDENT_LEN, "", ptr1);
                }
                break;
            }
            ++ptr2;
            if (first) {
                llly_print(out, "%.*s", (int)(ptr2-ptr1), ptr1);
                first = 0;
            } else {
                llly_print(out, "%*s%.*s", INDENT_LEN, "", (int)(ptr2-ptr1), ptr1);
            }
            ptr1 = ptr2;
        }
    }

    if (first) {
        llly_print(out, "\n");
    }
}

static void
info_print_snode(struct lllyout *out, const struct lllys_node *parent, const struct lllys_node *node, const char *label)
{
    assert(strlen(label) < INDENT_LEN-1);

    llly_print(out, "%-*s", INDENT_LEN, label);

    if (node) {
        if (node->name) {
            llly_print(out, "%s \"", strnodetype(node->nodetype));
            if (parent != lllys_parent(node)) {
                llly_print(out, "%s:", node->module->prefix);
            }
            llly_print(out, "%s\"\n", node->name);
        } else {
            llly_print(out, "%s\n", (node->nodetype == LLLYS_INPUT ? "input" : "output"));
        }
        node = node->next;
        for (; node; node = node->next) {
            if (node->name) {
                llly_print(out, "%*s%s \"", INDENT_LEN, "", strnodetype(node->nodetype));
                if (parent != lllys_parent(node)) {
                    llly_print(out, "%s:", node->module->prefix);
                }
                llly_print(out, "%s\"\n", node->name);
            } else {
                llly_print(out, "%*s%s\n", INDENT_LEN, "", (node->nodetype == LLLYS_INPUT ? "input" : "output"));
            }
        }
    } else {
        llly_print(out, "\n");
    }
}

static void
info_print_flags(struct lllyout *out, uint16_t flags, uint16_t mask, int is_list)
{
    if (mask & LLLYS_CONFIG_MASK) {
        llly_print(out, "%-*s", INDENT_LEN, "Config: ");
        if (flags & LLLYS_CONFIG_R) {
            llly_print(out, "read-only\n");
        } else {
            llly_print(out, "read-write\n");
        }
    }

    if (mask & LLLYS_STATUS_MASK) {
        llly_print(out, "%-*s", INDENT_LEN, "Status: ");

        if (flags & LLLYS_STATUS_DEPRC) {
            llly_print(out, "deprecated\n");
        } else if (flags & LLLYS_STATUS_OBSLT) {
            llly_print(out, "obsolete\n");
        } else {
            llly_print(out, "current\n");
        }
    }

    if (mask & LLLYS_MAND_MASK) {
        llly_print(out, "%-*s", INDENT_LEN, "Mandatory: ");

        if (flags & LLLYS_MAND_TRUE) {
            llly_print(out, "yes\n");
        } else {
            llly_print(out, "no\n");
        }
    }

    if (is_list && (mask & LLLYS_USERORDERED)) {
        llly_print(out, "%-*s", INDENT_LEN, "Order: ");

        if (flags & LLLYS_USERORDERED) {
            llly_print(out, "user-ordered\n");
        } else {
            llly_print(out, "system-ordered\n");
        }
    }

    if (!is_list && (mask & LLLYS_FENABLED)) {
        llly_print(out, "%-*s", INDENT_LEN, "Enabled: ");

        if (flags & LLLYS_FENABLED) {
            llly_print(out, "yes\n");
        } else {
            llly_print(out, "no\n");
        }
    }
}

static void
info_print_if_feature(struct lllyout *out, const struct lllys_module *module,
                      struct lllys_iffeature *iffeature, uint8_t iffeature_size)
{
    int i;

    llly_print(out, "%-*s", INDENT_LEN, "If-feats: ");

    if (iffeature_size) {
        llly_print_iffeature(out, module, &iffeature[0], 1);
        llly_print(out, "\n");
        for (i = 1; i < iffeature_size; ++i) {
            llly_print(out, "%*s", INDENT_LEN, "");
            llly_print_iffeature(out, module, &iffeature[i], 1);
            llly_print(out, "\n");
        }
    } else {
        llly_print(out, "\n");
    }
}

static void
info_print_when(struct lllyout *out, const struct lllys_when *when)
{
    llly_print(out, "%-*s", INDENT_LEN, "When: ");
    if (when) {
        llly_print(out, "%s\n", when->cond);
    } else {
        llly_print(out, "\n");
    }
}

static void
info_print_must(struct lllyout *out, const struct lllys_restr *must, uint8_t must_size)
{
    int i;

    llly_print(out, "%-*s", INDENT_LEN, "Must: ");

    if (must_size) {
        llly_print(out, "%s\n", must[0].expr);
        for (i = 1; i < must_size; ++i) {
            llly_print(out, "%*s%s\n", INDENT_LEN, "", must[i].expr);
        }
    } else {
        llly_print(out, "\n");
    }
}

static void
info_print_typedef(struct lllyout *out, const struct lllys_tpdf *tpdf, uint8_t tpdf_size)
{
    int i;

    llly_print(out, "%-*s", INDENT_LEN, "Typedefs: ");

    if (tpdf_size) {
        llly_print(out, "%s\n", tpdf[0].name);
        for (i = 1; i < tpdf_size; ++i) {
            llly_print(out, "%*s%s", INDENT_LEN, "", tpdf[i].name);
        }
    } else {
        llly_print(out, "\n");
    }
}

static void
info_print_typedef_with_include(struct lllyout *out, const struct lllys_module *mod)
{
    int i, j, first = 1;

    llly_print(out, "%-*s", INDENT_LEN, "Typedefs: ");

    if (mod->tpdf_size) {
        llly_print(out, "%s\n", mod->tpdf[0].name);
        first = 0;

        for (i = 1; i < mod->tpdf_size; ++i) {
            llly_print(out, "%*s%s\n", INDENT_LEN, "", mod->tpdf[i].name);
        }
    }

    for (i = 0; i < mod->inc_size; ++i) {
        if (mod->inc[i].submodule->tpdf_size) {
            if (first) {
                llly_print(out, "%s (%s)\n", mod->inc[i].submodule->tpdf[0].name, mod->inc[i].submodule->name);
                j = 1;
            } else {
                j = 0;
            }
            first = 0;

            for (; j < mod->inc[i].submodule->tpdf_size; ++j) {
                llly_print(out, "%*s%s (%s)\n", INDENT_LEN, "", mod->inc[i].submodule->tpdf[j].name, mod->inc[i].submodule->name);
            }
        }
    }

    if (first) {
        llly_print(out, "\n");
    }
}

static void
info_print_type_detail_(struct lllyout *out, const struct lllys_type *type, int uni)
{
    unsigned int i;
    struct lllys_type *orig;

    if (uni) {
        llly_print(out, "  ");
    }

    switch (type->base) {
    case LLLY_TYPE_BINARY:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "binary");
        if (!uni) {
            info_print_text(out, (type->info.binary.length ? type->info.binary.length->expr : NULL), "Length: ");
        }
        break;
    case LLLY_TYPE_BITS:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "bits");

        assert(type->info.bits.count);
        if (!uni) {
            llly_print(out, "%-*s%u %s\n", INDENT_LEN, "Bits: ", type->info.bits.bit[0].pos, type->info.bits.bit[0].name);
            for (i = 1; i < type->info.bits.count; ++i) {
                llly_print(out, "%*s%u %s\n", INDENT_LEN, "", type->info.bits.bit[i].pos, type->info.bits.bit[i].name);
            }
        }

        break;
    case LLLY_TYPE_BOOL:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "boolean");
        break;
    case LLLY_TYPE_DEC64:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "decimal64");
        if (!uni) {
            info_print_text(out, (type->info.dec64.range ? type->info.dec64.range->expr : NULL), "Range: ");
            assert(type->info.dec64.dig);
            llly_print(out, "%-*s%u%s\n", INDENT_LEN, "Frac dig: ", type->info.dec64.dig,
                     type->der->type.der ? " (derived)" : "");
        }
        break;
    case LLLY_TYPE_EMPTY:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "empty");
        break;
    case LLLY_TYPE_ENUM:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "enumeration");

        for (orig = (struct lllys_type *)type; !orig->info.enums.count; orig = &orig->der->type);

        if (!uni) {
            llly_print(out, "%-*s%s (%d)\n", INDENT_LEN, "Values: ",
                     orig->info.enums.enm[0].name, orig->info.enums.enm[0].value);
            for (i = 1; i < orig->info.enums.count; ++i) {
                llly_print(out, "%*s%s (%d)\n", INDENT_LEN, "",
                         orig->info.enums.enm[i].name, orig->info.enums.enm[i].value);
            }
        }

        break;
    case LLLY_TYPE_IDENT:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "identityref");
        if (!uni && type->info.ident.count) {
            llly_print(out, "%-*s%s\n", INDENT_LEN, "Idents:   ", type->info.ident.ref[0]->name);
            for (i = 1; i < type->info.ident.count; ++i) {
                llly_print(out, "%*s%s\n", INDENT_LEN, "", type->info.ident.ref[i]->name);
            }
        }
        break;
    case LLLY_TYPE_INST:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "instance-identifier");
        if (!uni) {
            llly_print(out, "%-*s%s\n", INDENT_LEN, "Required: ", (type->info.inst.req < 1 ? "no" : "yes"));
        }
        break;
    case LLLY_TYPE_INT8:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "int8");
        goto int_range;
    case LLLY_TYPE_INT16:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "int16");
        goto int_range;
    case LLLY_TYPE_INT32:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "int32");
        goto int_range;
    case LLLY_TYPE_INT64:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "int64");
        goto int_range;
    case LLLY_TYPE_UINT8:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "uint8");
        goto int_range;
    case LLLY_TYPE_UINT16:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "uint16");
        goto int_range;
    case LLLY_TYPE_UINT32:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "uint32");
        goto int_range;
    case LLLY_TYPE_UINT64:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "uint64");

int_range:
        if (!uni) {
            info_print_text(out, (type->info.num.range ? type->info.num.range->expr : NULL), "Range: ");
        }
        break;
    case LLLY_TYPE_LEAFREF:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "leafref");
        if (!uni) {
            info_print_text(out, type->info.lref.path, "Path: ");
        }
        break;
    case LLLY_TYPE_STRING:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "string");
        if (!uni) {
            info_print_text(out, (type->info.str.length ? type->info.str.length->expr : NULL), "Length: ");

            llly_print(out, "%-*s", INDENT_LEN, "Pattern: ");
            if (type->info.str.pat_count) {
                llly_print(out, "%s%s\n", &type->info.str.patterns[0].expr[1],
                         type->info.str.patterns[0].expr[0] == 0x15 ? " (invert-match)" : "");
                for (i = 1; i < type->info.str.pat_count; ++i) {
                    llly_print(out, "%*s%s%s\n", INDENT_LEN, "", &type->info.str.patterns[i].expr[1],
                             type->info.str.patterns[i].expr[0] == 0x15 ? " (invert-match)" : "");
                }
            } else {
                llly_print(out, "\n");
            }
        }

        break;
    case LLLY_TYPE_UNION:
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "union");

        if (!uni) {
            for (i = 0; i < type->info.uni.count; ++i) {
                info_print_type_detail_(out, &type->info.uni.types[i], 1);
            }
        }
        break;
    default:
        /* unused outside libyang, we never should be here */
        LOGINT(type->parent->module->ctx);
        llly_print(out, "%-*s%s\n", INDENT_LEN, "Base type: ", "UNKNOWN");
        break;
    }

    if (uni) {
        llly_print(out, "  ");
    }
    llly_print(out, "%-*s", INDENT_LEN, "Superior: ");
    if (type->der) {
        if (!lllys_type_is_local(type)) {
            llly_print(out, "%s:", type->der->module->name);
        }
        llly_print(out, "%s\n", type->der->name);
    } else {
        llly_print(out, "\n");
    }
}

static void
info_print_type_detail(struct lllyout *out, const struct lllys_type *type, int * UNUSED(first))
{
    return info_print_type_detail_(out, type, 0);
}

static void
info_print_list_constr(struct lllyout *out, uint32_t min, uint32_t max)
{
    llly_print(out, "%-*s%u..", INDENT_LEN, "Elements: ", min);
    if (max) {
        llly_print(out, "%u\n", max);
    } else {
        llly_print(out, "unbounded\n");
    }
}

static void
info_print_unique(struct lllyout *out, const struct lllys_unique *unique, uint8_t unique_size)
{
    int i, j;

    llly_print(out, "%-*s", INDENT_LEN, "Unique: ");

    if (unique_size) {
        llly_print(out, "%s\n", unique[0].expr[0]);
        for (i = 0; i < unique_size; ++i) {
            for (j = (!i ? 1 : 0); j < unique[i].expr_size; ++j) {
                llly_print(out, "%*s%s\n", INDENT_LEN, "", unique[i].expr[j]);
            }
        }
    } else {
        llly_print(out, "\n");
    }
}

static void
info_print_revision(struct lllyout *out, const struct lllys_revision *rev, uint8_t rev_size)
{
    int i;

    llly_print(out, "%-*s", INDENT_LEN, "Revisions: ");

    if (rev_size) {
        llly_print(out, "%s\n", rev[0].date);
        for (i = 1; i < rev_size; ++i) {
            llly_print(out, "%*s%s\n", INDENT_LEN, "", rev[i].date);
        }
    } else {
        llly_print(out, "\n");
    }
}

static void
info_print_import_with_include(struct lllyout *out, const struct lllys_module *mod)
{
    int first = 1, i, j;

    llly_print(out, "%-*s", INDENT_LEN, "Imports: ");
    if (mod->imp_size) {
        llly_print(out, "%s:%s\n", mod->imp[0].prefix, mod->imp[0].module->name);
        i = 1;
        first = 0;

        for (; i < mod->imp_size; ++i) {
            llly_print(out, "%*s%s:%s\n", INDENT_LEN, "", mod->imp[i].prefix, mod->imp[i].module->name);
        }
    }

    for (j = 0; j < mod->inc_size; ++j) {
        if (mod->inc[j].submodule->imp_size) {
            if (first) {
                llly_print(out, "%s:%s (%s)\n",
                        mod->inc[j].submodule->imp[0].prefix, mod->inc[j].submodule->imp[0].module->name, mod->inc[j].submodule->name);
                i = 1;
            } else {
                i = 0;
            }
            first = 0;

            for (; i < mod->inc[j].submodule->imp_size; ++i) {
                llly_print(out, "%*s%s:%s (%s)\n", INDENT_LEN, "",
                        mod->inc[j].submodule->imp[i].prefix, mod->inc[j].submodule->imp[i].module->name, mod->inc[j].submodule->name);
            }
        }
    }

    if (first) {
        llly_print(out, "\n");
    }
}

static void
info_print_include(struct lllyout *out, const struct lllys_module *mod)
{
    int first = 1, i;

    llly_print(out, "%-*s", INDENT_LEN, "Includes: ");
    if (mod->inc_size) {
        llly_print(out, "%s\n", mod->inc[0].submodule->name);
        i = 1;
        first = 0;

        for (; i < mod->inc_size; ++i) {
            llly_print(out, "%*s%s\n", INDENT_LEN, "", mod->inc[i].submodule->name);
        }
    }

    if (first) {
        llly_print(out, "\n");
    }
}

static void
info_print_augment(struct lllyout *out, const struct lllys_module *mod)
{
    int first = 1, i;

    llly_print(out, "%-*s", INDENT_LEN, "Augments: ");
    if (mod->augment_size) {
        llly_print(out, "\"%s\"\n", mod->augment[0].target_name);
        i = 1;
        first = 0;

        for (; i < mod->augment_size; ++i) {
            llly_print(out, "%*s\"%s\"\n", INDENT_LEN, "", mod->augment[i].target_name);
        }
    }

    if (first) {
        llly_print(out, "\n");
    }
}

static void
info_print_deviation(struct lllyout *out, const struct lllys_module *mod)
{
    int first = 1, i;

    llly_print(out, "%-*s", INDENT_LEN, "Deviation: ");
    if (mod->deviation_size) {
        llly_print(out, "\"%s\"\n", mod->deviation[0].target_name);
        i = 1;
        first = 0;

        for (; i < mod->deviation_size; ++i) {
            llly_print(out, "%*s\"%s\"\n", INDENT_LEN, "", mod->deviation[i].target_name);
        }
    }

    if (first) {
        llly_print(out, "\n");
    }
}

static void
info_print_ident_with_include(struct lllyout *out, const struct lllys_module *mod)
{
    int first = 1, i, j;

    llly_print(out, "%-*s", INDENT_LEN, "Idents: ");
    if (mod->ident_size) {
        llly_print(out, "%s\n", mod->ident[0].name);
        i = 1;
        first = 0;

        for (; i < (signed)mod->ident_size; ++i) {
            llly_print(out, "%*s%s\n", INDENT_LEN, "", mod->ident[i].name);
        }
    }

    for (j = 0; j < mod->inc_size; ++j) {
        if (mod->inc[j].submodule->ident_size) {
            if (first) {
                llly_print(out, "%s (%s)\n", mod->inc[j].submodule->ident[0].name, mod->inc[j].submodule->name);
                i = 1;
            } else {
                i = 0;
            }
            first = 0;

            for (; i < (signed)mod->inc[j].submodule->ident_size; ++i) {
                llly_print(out, "%*s%s (%s)\n", INDENT_LEN, "", mod->inc[j].submodule->ident[i].name, mod->inc[j].submodule->name);
            }
        }
    }

    if (first) {
        llly_print(out, "\n");
    }
}

static void
info_print_features_with_include(struct lllyout *out, const struct lllys_module *mod)
{
    int first = 1, i, j;

    llly_print(out, "%-*s", INDENT_LEN, "Features: ");
    if (mod->features_size) {
        llly_print(out, "%s\n", mod->features[0].name);
        i = 1;
        first = 0;

        for (; i < mod->features_size; ++i) {
            llly_print(out, "%*s%s\n", INDENT_LEN, "", mod->features[i].name);
        }
    }

    for (j = 0; j < mod->inc_size; ++j) {
        if (mod->inc[j].submodule->features_size) {
            if (first) {
                llly_print(out, "%s (%s)\n", mod->inc[j].submodule->features[0].name, mod->inc[j].submodule->name);
                i = 1;
            } else {
                i = 0;
            }
            first = 0;

            for (; i < mod->inc[j].submodule->features_size; ++i) {
                llly_print(out, "%*s%s (%s)\n", INDENT_LEN, "", mod->inc[j].submodule->features[i].name, mod->inc[j].submodule->name);
            }
        }
    }

    if (first) {
        llly_print(out, "\n");
    }
}

static void
info_print_data_mainmod_with_include(struct lllyout *out, const struct lllys_module *mod)
{
    int first = 1, from_include;
    struct lllys_node *node;
    const struct lllys_module *mainmod = lllys_main_module(mod);

    llly_print(out, "%-*s", INDENT_LEN, "Data: ");

    if (mainmod->data) {
        LLLY_TREE_FOR(mainmod->data, node) {
            if (node->module != mod) {
                if (mainmod != mod) {
                    continue;
                } else {
                    from_include = 1;
                }
            } else {
                from_include = 0;
            }

            if (!lllys_parent(node) && !strcmp(node->name, "config") && !strcmp(node->module->name, "ietf-netconf")) {
                /* node added by libyang, not actually in the model */
                continue;
            }

            if (first) {
                llly_print(out, "%s \"%s\"%s%s%s\n", strnodetype(node->nodetype), node->name, (from_include ? " (" : ""),
                                                   (from_include ? node->module->name : ""), (from_include ? ")" : ""));
                first = 0;
            } else {
                llly_print(out, "%*s%s \"%s\"%s%s%s\n", INDENT_LEN, "", strnodetype(node->nodetype), node->name,
                        (from_include ? " (" : ""), (from_include ? node->module->name : ""), (from_include ? ")" : ""));
            }
        }
    }

    if (first) {
        llly_print(out, "\n");
    }
}

static void
info_print_typedef_detail(struct lllyout *outf, const struct lllys_tpdf *tpdf, int * UNUSED(first))
{
    llly_print(outf, "%-*s%s\n", INDENT_LEN, "Typedef: ", tpdf->name);
    llly_print(outf, "%-*s%s\n", INDENT_LEN, "Module: ", tpdf->module->name);
    info_print_text(outf, tpdf->dsc, "Desc: ");
    info_print_text(outf, tpdf->ref, "Reference: ");
    info_print_flags(outf, tpdf->flags, LLLYS_STATUS_MASK, 0);
    info_print_type_detail_(outf, &tpdf->type, 0);
    info_print_text(outf, tpdf->units, "Units: ");
    info_print_text(outf, tpdf->dflt, "Default: ");
}

static void
info_print_ident_detail(struct lllyout *out, const struct lllys_ident *ident, int * UNUSED(first))
{
    unsigned int i;

    llly_print(out, "%-*s%s\n", INDENT_LEN, "Identity: ", ident->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", ident->module->name);
    info_print_text(out, ident->dsc, "Desc: ");
    info_print_text(out, ident->ref, "Reference: ");
    info_print_flags(out, ident->flags, LLLYS_STATUS_MASK, 0);

    llly_print(out, "%-*s", INDENT_LEN, "Base: ");
    for (i = 0; i < ident->base_size; i++) {
        llly_print(out, "%*s%s\n", i ? INDENT_LEN : 0, "", ident->base[i]->name);
    }
    if (!i) {
        llly_print(out, "\n");
    }

    llly_print(out, "%-*s", INDENT_LEN, "Derived: ");
    if (ident->der) {
        for (i = 0; i < ident->der->number; i++) {
            llly_print(out, "%*s%s\n", i ? INDENT_LEN : 0, "", ((struct lllys_ident *)ident->der->set.g[i])->name);
        }
        if (!i) {
            llly_print(out, "\n");
        }
    }
}

static void
info_print_feature_detail(struct lllyout *out, const struct lllys_feature *feat, int * UNUSED(first))
{
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Feature: ", feat->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", feat->module->name);
    info_print_text(out, feat->dsc, "Desc: ");
    info_print_text(out, feat->ref, "Reference: ");
    info_print_flags(out, feat->flags, LLLYS_STATUS_MASK | LLLYS_FENABLED, 0);
    info_print_if_feature(out, feat->module, feat->iffeature, feat->iffeature_size);
}

static void
info_print_module(struct lllyout *out, const struct lllys_module *module)
{
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", module->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Namespace: ", module->ns);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Prefix: ", module->prefix);
    info_print_text(out, module->dsc, "Desc: ");
    info_print_text(out, module->ref, "Reference: ");
    info_print_text(out, module->org, "Org: ");
    info_print_text(out, module->contact, "Contact: ");
    llly_print(out, "%-*s%s\n", INDENT_LEN, "YANG ver: ", (module->version == LLLYS_VERSION_1_1 ? "1.1" : "1.0"));
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Deviated: ", (module->deviated ? "yes" : "no"));
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Implement: ", (module->implemented ? "yes" : "no"));
    info_print_text(out, module->filepath, "URI: file://");

    info_print_revision(out, module->rev, module->rev_size);
    info_print_include(out, module);
    info_print_import_with_include(out, module);
    info_print_typedef_with_include(out, module);
    info_print_ident_with_include(out, module);
    info_print_features_with_include(out, module);
    info_print_augment(out, module);
    info_print_deviation(out, module);

    info_print_data_mainmod_with_include(out, module);
}

static void
info_print_submodule(struct lllyout *out, const struct lllys_submodule *module)
{
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Submodule: ", module->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Parent: ", module->belongsto->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Prefix: ", module->prefix);
    info_print_text(out, module->dsc, "Desc: ");
    info_print_text(out, module->ref, "Reference: ");
    info_print_text(out, module->org, "Org: ");
    info_print_text(out, module->contact, "Contact: ");

    llly_print(out, "%-*s%s\n", INDENT_LEN, "YANG ver: ", (module->version == 2 ? "1.1" : "1.0"));
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Deviated: ", (module->deviated ? "yes" : "no"));
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Implement: ", (module->implemented ? "yes" : "no"));

    info_print_text(out, module->filepath, "URI: file://");

    info_print_revision(out, module->rev, module->rev_size);
    info_print_include(out, (struct lllys_module *)module);
    info_print_import_with_include(out, (struct lllys_module *)module);
    info_print_typedef_with_include(out, (struct lllys_module *)module);
    info_print_ident_with_include(out, (struct lllys_module *)module);
    info_print_features_with_include(out, (struct lllys_module *)module);
    info_print_augment(out, (struct lllys_module *)module);
    info_print_deviation(out, (struct lllys_module *)module);

    info_print_data_mainmod_with_include(out, (struct lllys_module *)module);
}

static void
info_print_container(struct lllyout *out, const struct lllys_node *node, int *UNUSED(first))
{
    struct lllys_node_container *cont = (struct lllys_node_container *)node;

    llly_print(out, "%-*s%s\n", INDENT_LEN, "Container: ", cont->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", cont->module->name);
    info_print_text(out, cont->dsc, "Desc: ");
    info_print_text(out, cont->ref, "Reference: ");
    info_print_flags(out, cont->flags, LLLYS_CONFIG_MASK | LLLYS_STATUS_MASK, 0);
    info_print_text(out, cont->presence, "Presence: ");
    info_print_if_feature(out, cont->module, cont->iffeature, cont->iffeature_size);
    info_print_when(out, cont->when);
    info_print_must(out, cont->must, cont->must_size);
    info_print_typedef(out, cont->tpdf, cont->tpdf_size);

    info_print_snode(out, (struct lllys_node *)cont, cont->child, "Children:");
}

static void
info_print_choice(struct lllyout *out, const struct lllys_node *node, int *UNUSED(first))
{
    struct lllys_node_choice *choice = (struct lllys_node_choice *)node;

    llly_print(out, "%-*s%s\n", INDENT_LEN, "Choice: ", choice->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", choice->module->name);
    info_print_text(out, choice->dsc, "Desc: ");
    info_print_text(out, choice->ref, "Reference: ");
    info_print_flags(out, choice->flags, LLLYS_CONFIG_MASK | LLLYS_STATUS_MASK | LLLYS_MAND_MASK, 0);
    llly_print(out, "%-*s", INDENT_LEN, "Default: ");
    if (choice->dflt) {
        llly_print(out, "%s\n", choice->dflt->name);
    } else {
        llly_print(out, "\n");
    }
    info_print_if_feature(out, choice->module, choice->iffeature, choice->iffeature_size);
    info_print_when(out, choice->when);

    info_print_snode(out, (struct lllys_node *)choice, choice->child, "Cases:");
}

static void
info_print_leaf(struct lllyout *out, const struct lllys_node *node, int *UNUSED(first))
{
    struct lllys_node_leaf *leaf = (struct lllys_node_leaf *)node;

    llly_print(out, "%-*s%s\n", INDENT_LEN, "Leaf: ", leaf->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", leaf->module->name);
    info_print_text(out, leaf->dsc, "Desc: ");
    info_print_text(out, leaf->ref, "Reference: ");
    info_print_flags(out, leaf->flags, LLLYS_CONFIG_MASK | LLLYS_STATUS_MASK | LLLYS_MAND_MASK, 0);
    info_print_text(out, leaf->type.der->name, "Type: ");
    info_print_text(out, leaf->units, "Units: ");
    info_print_text(out, leaf->dflt, "Default: ");
    info_print_if_feature(out, leaf->module, leaf->iffeature, leaf->iffeature_size);
    info_print_when(out, leaf->when);
    info_print_must(out, leaf->must, leaf->must_size);
}

static void
info_print_leaflist(struct lllyout *out, const struct lllys_node *node, int *UNUSED(first))
{
    struct lllys_node_leaflist *llist = (struct lllys_node_leaflist *)node;

    llly_print(out, "%-*s%s\n", INDENT_LEN, "Leaflist: ", llist->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", llist->module->name);
    info_print_text(out, llist->dsc, "Desc: ");
    info_print_text(out, llist->ref, "Reference: ");
    info_print_flags(out, llist->flags, LLLYS_CONFIG_MASK | LLLYS_STATUS_MASK | LLLYS_USERORDERED, 1);
    info_print_text(out, llist->type.der->name, "Type: ");
    info_print_text(out, llist->units, "Units: ");
    info_print_list_constr(out, llist->min, llist->max);
    info_print_if_feature(out, llist->module, llist->iffeature, llist->iffeature_size);
    info_print_when(out, llist->when);
    info_print_must(out, llist->must, llist->must_size);
}

static void
info_print_list(struct lllyout *out, const struct lllys_node *node, int *UNUSED(first))
{
    struct lllys_node_list *list = (struct lllys_node_list *)node;

    llly_print(out, "%-*s%s\n", INDENT_LEN, "List: ", list->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", list->module->name);
    info_print_text(out, list->dsc, "Desc: ");
    info_print_text(out, list->ref, "Reference: ");
    info_print_flags(out, list->flags, LLLYS_CONFIG_MASK | LLLYS_STATUS_MASK | LLLYS_USERORDERED, 1);
    info_print_list_constr(out, list->min, list->max);
    info_print_if_feature(out, list->module, list->iffeature, list->iffeature_size);
    info_print_when(out, list->when);
    info_print_must(out, list->must, list->must_size);
    info_print_text(out, list->keys_str, "Keys: ");
    info_print_unique(out, list->unique, list->unique_size);
    info_print_typedef(out, list->tpdf, list->tpdf_size);

    info_print_snode(out, (struct lllys_node *)list, list->child, "Children:");
}

static void
info_print_anydata(struct lllyout *out, const struct lllys_node *node, int *UNUSED(first))
{
    struct lllys_node_anydata *any = (struct lllys_node_anydata *)node;

    llly_print(out, "%-*s%s\n", INDENT_LEN, "%s: ", any->nodetype == LLLYS_ANYXML ? "Anyxml" : "Anydata", any->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", any->module->name);
    info_print_text(out, any->dsc, "Desc: ");
    info_print_text(out, any->ref, "Reference: ");
    info_print_flags(out, any->flags, LLLYS_CONFIG_MASK | LLLYS_STATUS_MASK | LLLYS_MAND_MASK, 0);
    info_print_if_feature(out, any->module, any->iffeature, any->iffeature_size);
    info_print_when(out, any->when);
    info_print_must(out, any->must, any->must_size);
}

static void
info_print_grouping(struct lllyout *out, const struct lllys_node *node, int * UNUSED(first))
{
    struct lllys_node_grp *group = (struct lllys_node_grp *)node;

    llly_print(out, "%-*s%s\n", INDENT_LEN, "Grouping: ", group->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", group->module->name);
    info_print_text(out, group->dsc, "Desc: ");
    info_print_text(out, group->ref, "Reference: ");
    info_print_flags(out, group->flags, LLLYS_STATUS_MASK, 0);
    info_print_typedef(out, group->tpdf, group->tpdf_size);

    info_print_snode(out, (struct lllys_node *)group, group->child, "Children:");
}

static void
info_print_case(struct lllyout *out, const struct lllys_node *node, int *UNUSED(first))
{
    struct lllys_node_case *cas = (struct lllys_node_case *)node;

    llly_print(out, "%-*s%s\n", INDENT_LEN, "Case: ", cas->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", cas->module->name);
    info_print_text(out, cas->dsc, "Desc: ");
    info_print_text(out, cas->ref, "Reference: ");
    info_print_flags(out, cas->flags, LLLYS_CONFIG_MASK | LLLYS_STATUS_MASK, 0);
    info_print_if_feature(out, cas->module, cas->iffeature, cas->iffeature_size);
    info_print_when(out, cas->when);

    info_print_snode(out, (struct lllys_node *)cas, cas->child, "Children:");
}

static void
info_print_input(struct lllyout *out, const struct lllys_node *node, int *UNUSED(first))
{
    struct lllys_node_inout *input = (struct lllys_node_inout *)node;

    assert(lllys_parent(node) && lllys_parent(node)->nodetype == LLLYS_RPC);

    llly_print(out, "%-*s%s\n", INDENT_LEN, "Input of: ", lllys_parent(node)->name);
    info_print_typedef(out, input->tpdf, input->tpdf_size);
    info_print_must(out, input->must, input->must_size);

    info_print_snode(out, (struct lllys_node *)input, input->child, "Children:");
}

static void
info_print_output(struct lllyout *out, const struct lllys_node *node, int *UNUSED(first))
{
    struct lllys_node_inout *output = (struct lllys_node_inout *)node;

    assert(lllys_parent(node) && lllys_parent(node)->nodetype == LLLYS_RPC);

    llly_print(out, "%-*s%s\n", INDENT_LEN, "Output of: ", lllys_parent(node)->name);
    info_print_typedef(out, output->tpdf, output->tpdf_size);
    info_print_must(out, output->must, output->must_size);

    info_print_snode(out, (struct lllys_node *)output, output->child, "Children:");
}

static void
info_print_notif(struct lllyout *out, const struct lllys_node *node, int *UNUSED(first))
{
    struct lllys_node_notif *ntf = (struct lllys_node_notif *)node;

    llly_print(out, "%-*s%s\n", INDENT_LEN, "Notif: ", ntf->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", ntf->module->name);
    info_print_text(out, ntf->dsc, "Desc: ");
    info_print_text(out, ntf->ref, "Reference: ");
    info_print_flags(out, ntf->flags, LLLYS_STATUS_MASK, 0);
    info_print_if_feature(out, ntf->module, ntf->iffeature, ntf->iffeature_size);
    info_print_typedef(out, ntf->tpdf, ntf->tpdf_size);
    info_print_must(out, ntf->must, ntf->must_size);

    info_print_snode(out, (struct lllys_node *)ntf, ntf->child, "Params:");
}

static void
info_print_rpc(struct lllyout *out, const struct lllys_node *node, int *UNUSED(first))
{
    struct lllys_node_rpc_action *rpc = (struct lllys_node_rpc_action *)node;

    llly_print(out, "%-*s%s\n", INDENT_LEN, "RPC: ", rpc->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", rpc->module->name);
    info_print_text(out, rpc->dsc, "Desc: ");
    info_print_text(out, rpc->ref, "Reference: ");
    info_print_flags(out, rpc->flags, LLLYS_STATUS_MASK, 0);
    info_print_if_feature(out, rpc->module, rpc->iffeature, rpc->iffeature_size);
    info_print_typedef(out, rpc->tpdf, rpc->tpdf_size);

    info_print_snode(out, (struct lllys_node *)rpc, rpc->child, "Data:");
}

static void
info_print_action(struct lllyout *out, const struct lllys_node *node, int *UNUSED(first))
{
    struct lllys_node_rpc_action *act = (struct lllys_node_rpc_action *)node;

    llly_print(out, "%-*s%s\n", INDENT_LEN, "Action: ", act->name);
    llly_print(out, "%-*s%s\n", INDENT_LEN, "Module: ", act->module->name);
    info_print_text(out, act->dsc, "Desc: ");
    info_print_text(out, act->ref, "Reference: ");
    info_print_flags(out, act->flags, LLLYS_STATUS_MASK, 0);
    info_print_if_feature(out, act->module, act->iffeature, act->iffeature_size);
    info_print_typedef(out, act->tpdf, act->tpdf_size);

    info_print_snode(out, (struct lllys_node *)act, act->child, "Data:");
}

int
info_print_model(struct lllyout *out, const struct lllys_module *module, const char *target_schema_path)
{
    int rc = EXIT_SUCCESS;

    if (!target_schema_path) {
        if (module->type == 0) {
            info_print_module(out, module);
        } else {
            info_print_submodule(out, (struct lllys_submodule *)module);
        }
    } else {
        rc = lllys_print_target(out, module, target_schema_path,
                              info_print_typedef_detail,
                              info_print_ident_detail,
                              info_print_feature_detail,
                              info_print_type_detail,
                              info_print_grouping,
                              info_print_container,
                              info_print_choice,
                              info_print_leaf,
                              info_print_leaflist,
                              info_print_list,
                              info_print_anydata,
                              info_print_case,
                              info_print_notif,
                              info_print_rpc,
                              info_print_action,
                              info_print_input,
                              info_print_output);
    }
    llly_print_flush(out);

    return rc;
}
