/**
 * @file printer/yang.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief YANG printer for libyang data model structure
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "common.h"
#include "context.h"
#include "printer.h"
#include "tree_schema.h"

#define INDENT ""
#define LEVEL (level*2)

static void yang_print_snode(struct lllyout *out, int level, const struct lllys_node *node, int mask);
static void yang_print_extension_instances(struct lllyout *out, int level, const struct lllys_module *module,
                                           LLLYEXT_SUBSTMT substmt, uint8_t substmt_index,
                                           struct lllys_ext_instance **ext, unsigned int count);

static void
yang_encode(struct lllyout *out, const char *text, int len)
{
    int i, start_len;
    const char *start;
    char special = 0;

    if (!len) {
        return;
    }

    if (len < 0) {
        len = strlen(text);
    }

    start = text;
    start_len = 0;
    for (i = 0; i < len; ++i) {
        switch (text[i]) {
        case '\n':
        case '\t':
        case '\"':
        case '\\':
            special = text[i];
            break;
        default:
            ++start_len;
            break;
        }

        if (special) {
            llly_write(out, start, start_len);
            switch (special) {
            case '\n':
                llly_write(out, "\\n", 2);
                break;
            case '\t':
                llly_write(out, "\\t", 2);
                break;
            case '\"':
                llly_write(out, "\\\"", 2);
                break;
            case '\\':
                llly_write(out, "\\\\", 2);
                break;
            }

            start += start_len + 1;
            start_len = 0;

            special = 0;
        }
    }

    llly_write(out, start, start_len);
}

static void
yang_print_open(struct lllyout *out, int *flag)
{
    if (flag && !*flag) {
        *flag = 1;
        llly_print(out, " {\n");
    }
}

static void
yang_print_close(struct lllyout *out, int level, int flag)
{
    if (flag) {
        llly_print(out, "%*s}\n", LEVEL, INDENT);
    } else {
        llly_print(out, ";\n");
    }
}

static void
yang_print_text(struct lllyout *out, int level, const char *name, const char *text, int singleline, int closed)
{
    const char *s, *t;

    if (singleline) {
        llly_print(out, "%*s%s \"", LEVEL, INDENT, name);
    } else {
        llly_print(out, "%*s%s\n", LEVEL, INDENT, name);
        level++;

        llly_print(out, "%*s\"", LEVEL, INDENT);
    }
    t = text;
    while ((s = strchr(t, '\n'))) {
        yang_encode(out, t, s - t);
        llly_print(out, "\n");
        t = s + 1;
        if (*t != '\n') {
            llly_print(out, "%*s ", LEVEL, INDENT);
        }
    }

    yang_encode(out, t, strlen(t));
    if (closed) {
        llly_print(out, "\";\n");
    } else {
        llly_print(out, "\"");
    }
    level--;

}

static void
yang_print_substmt(struct lllyout *out, int level, LLLYEXT_SUBSTMT substmt, uint8_t substmt_index, const char *text,
                   const struct lllys_module *module, struct lllys_ext_instance **ext, unsigned int ext_size)
{
    int i = -1;

    if (!text) {
        /* nothing to print */
        return;
    }

    do {
        i = lllys_ext_iter(ext, ext_size, i + 1, substmt);
    } while (i != -1 && ext[i]->insubstmt_index != substmt_index);

    if (ext_substmt_info[substmt].flags & SUBST_FLAG_ID) {
        llly_print(out, "%*s%s %s%s", LEVEL, INDENT, ext_substmt_info[substmt].name, text, i == -1 ? ";\n" : "");
    } else {
        yang_print_text(out, level, ext_substmt_info[substmt].name, text,
                        (ext_substmt_info[substmt].flags & SUBST_FLAG_YIN) ? 0 : 1, i == -1 ? 1 : 0);
    }

    if (i != -1) {
        llly_print(out, " {\n");
        do {
            yang_print_extension_instances(out, level + 1, module, substmt, substmt_index, &ext[i], 1);
            do {
                i = lllys_ext_iter(ext, ext_size, i + 1, substmt);
            } while (i != -1 && ext[i]->insubstmt_index != substmt_index);
        } while (i != -1);
        llly_print(out, "%*s}\n", LEVEL, INDENT);
    }
}

static void
yang_print_iffeature(struct lllyout *out, int level, const struct lllys_module *module, struct lllys_iffeature *iffeature)
{
    llly_print(out, "%*sif-feature \"", LEVEL, INDENT);
    llly_print_iffeature(out, module, iffeature, 0);

    /* extensions */
    if (iffeature->ext_size) {
        llly_print(out, "\" {\n");
        yang_print_extension_instances(out, level + 1, module, LLLYEXT_SUBSTMT_SELF, 0, iffeature->ext, iffeature->ext_size);
        llly_print(out, "%*s}\n", LEVEL, INDENT);
    } else {
        llly_print(out, "\";\n");
    }
}

/*
 * Covers:
 * extension (instances), if-features, config, mandatory, status, description, reference
 */
#define SNODE_COMMON_EXT    0x01
#define SNODE_COMMON_IFF    0x02
#define SNODE_COMMON_CONFIG 0x04
#define SNODE_COMMON_MAND   0x08
#define SNODE_COMMON_STATUS 0x10
#define SNODE_COMMON_DSC    0x20
#define SNODE_COMMON_REF    0x40
static void
yang_print_snode_common(struct lllyout *out, int level, const struct lllys_node *node, const struct lllys_module *module,
                        int *flag, int mask)
{
    int i;
    const char *status = NULL;

    /* extensions */
    if ((mask & SNODE_COMMON_EXT) && node->ext_size) {
        yang_print_open(out, flag);
        yang_print_extension_instances(out, level, module, LLLYEXT_SUBSTMT_SELF, 0, node->ext, node->ext_size);
    }

    /* if-features */
    if (mask & SNODE_COMMON_IFF) {
        for (i = 0; i < node->iffeature_size; ++i) {
            yang_print_open(out, flag);
            yang_print_iffeature(out, level, module, &node->iffeature[i]);
        }
    }

    /* config */
    if (mask & SNODE_COMMON_CONFIG) {
        /* get info if there is an extension for the config statement */
        i = lllys_ext_iter(node->ext, node->ext_size, 0, LLLYEXT_SUBSTMT_CONFIG);

        if (lllys_parent(node)) {
            if ((node->flags & LLLYS_CONFIG_SET) || i != -1) {
                /* print config when it differs from the parent or if it has an extension instance ... */
                if (node->flags & LLLYS_CONFIG_W) {
                    yang_print_open(out, flag);
                    yang_print_substmt(out, level, LLLYEXT_SUBSTMT_CONFIG, 0, "true",
                                       module, node->ext, node->ext_size);
                } else if (node->flags & LLLYS_CONFIG_R) {
                    yang_print_open(out, flag);
                    yang_print_substmt(out, level, LLLYEXT_SUBSTMT_CONFIG, 0, "false",
                                       module, node->ext, node->ext_size);
                }
            }
        } else if (node->flags & LLLYS_CONFIG_R) {
            /* ... or it's a top-level state node */
            yang_print_open(out, flag);
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_CONFIG, 0, "false",
                               module, node->ext, node->ext_size);
        } else if (i != -1) {
            /* the config has an extension, so we have to print it */
            yang_print_open(out, flag);
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_CONFIG, 0, "true",
                               module, node->ext, node->ext_size);
        }
    }

    /* mandatory */
    if ((mask & SNODE_COMMON_MAND) && (node->nodetype & (LLLYS_LEAF | LLLYS_CHOICE | LLLYS_ANYDATA))) {
        if (node->flags & LLLYS_MAND_TRUE) {
            yang_print_open(out, flag);
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_MANDATORY, 0, "true",
                               module, node->ext, node->ext_size);
        } else if (node->flags & LLLYS_MAND_FALSE) {
            yang_print_open(out, flag);
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_MANDATORY, 0, "false",
                               module, node->ext, node->ext_size);
        }
    }

    /* status */
    if (mask & SNODE_COMMON_STATUS) {
        if (node->flags & LLLYS_STATUS_CURR) {
            yang_print_open(out, flag);
            status = "current";
        } else if (node->flags & LLLYS_STATUS_DEPRC) {
            yang_print_open(out, flag);
            status = "deprecated";
        } else if (node->flags & LLLYS_STATUS_OBSLT) {
            yang_print_open(out, flag);
            status = "obsolete";
        }
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_STATUS, 0, status, module, node->ext, node->ext_size);
    }

    /* description */
    if ((mask & SNODE_COMMON_DSC) && node->dsc) {
        yang_print_open(out, flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_DESCRIPTION, 0, node->dsc,
                           module, node->ext, node->ext_size);
    }

    /* reference */
    if ((mask & SNODE_COMMON_REF) && node->ref) {
        yang_print_open(out, flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_REFERENCE, 0, node->ref,
                           module, node->ext, node->ext_size);
    }
}

static void
yang_print_feature(struct lllyout *out, int level, const struct lllys_feature *feat)
{
    int flag = 0;

    llly_print(out, "%*sfeature %s", LEVEL, INDENT, feat->name);
    yang_print_snode_common(out, level + 1, (struct lllys_node *)feat, feat->module, &flag, SNODE_COMMON_EXT |
                            SNODE_COMMON_IFF | SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);
    yang_print_close(out, level, flag);
}

static void
yang_print_extension(struct lllyout *out, int level, const struct lllys_ext *ext)
{
    int flag = 0, flag2 = 0, i;

    llly_print(out, "%*sextension %s", LEVEL, INDENT, ext->name);
    level++;

    yang_print_snode_common(out, level, (struct lllys_node *)ext, ext->module, &flag,
                            SNODE_COMMON_EXT);

    if (ext->argument) {
        yang_print_open(out, &flag);

        llly_print(out, "%*sargument %s", LEVEL, INDENT, ext->argument);
        i = -1;
        while ((i = lllys_ext_iter(ext->ext, ext->ext_size, i + 1, LLLYEXT_SUBSTMT_ARGUMENT)) != -1) {
            yang_print_open(out, &flag2);
            yang_print_extension_instances(out, level + 1, ext->module, LLLYEXT_SUBSTMT_ARGUMENT, 0, &ext->ext[i], 1);
        }
        if ((ext->flags & LLLYS_YINELEM) || lllys_ext_iter(ext->ext, ext->ext_size, 0, LLLYEXT_SUBSTMT_YINELEM) != -1) {
            yang_print_open(out, &flag2);
            yang_print_substmt(out, level + 1, LLLYEXT_SUBSTMT_YINELEM, 0,
                               (ext->flags & LLLYS_YINELEM) ? "true" : "false", ext->module, ext->ext, ext->ext_size);
        }
        yang_print_close(out, level, flag2);
    }

    yang_print_snode_common(out, level, (struct lllys_node *)ext, ext->module, &flag,
                            SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);

    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_restr(struct lllyout *out, int level, const struct lllys_module *module, const struct lllys_restr *restr,
                 const char *name, const char *value)
{
    int flag = 0;

    llly_print(out, "%*s%s \"", LEVEL, INDENT, name);
    yang_encode(out, value, -1);
    llly_print(out, "\"");

    level++;
    if (restr->ext_size) {
        yang_print_open(out, &flag);
        yang_print_extension_instances(out, level, module, LLLYEXT_SUBSTMT_SELF, 0, restr->ext, restr->ext_size);
    }
    if (restr->expr[0] == 0x15) {
        /* special byte value in pattern's expression: 0x15 - invert-match, 0x06 - match */
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_MODIFIER, 0, "invert-match",
                           module, restr->ext, restr->ext_size);
    }
    if (restr->emsg != NULL) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_ERRMSG, 0, restr->emsg,
                           module, restr->ext, restr->ext_size);
    }
    if (restr->eapptag != NULL) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_ERRTAG, 0, restr->eapptag,
                           module, restr->ext, restr->ext_size);
    }
    if (restr->dsc != NULL) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_DESCRIPTION, 0, restr->dsc,
                           module, restr->ext, restr->ext_size);
    }
    if (restr->ref != NULL) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_REFERENCE, 0, restr->ref,
                           module, restr->ext, restr->ext_size);
    }
    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_when(struct lllyout *out, int level, const struct lllys_module *module, const struct lllys_when *when)
{
    int flag = 0;
    const char *str;

    str = transform_json2schema(module, when->cond);
    if (!str) {
        llly_print(out, "(!error!)");
        return;
    }

    llly_print(out, "%*swhen \"", LEVEL, INDENT);
    yang_encode(out, str, -1);
    llly_print(out, "\"");
    lllydict_remove(module->ctx, str);

    level++;

    if (when->ext_size) {
        /* extension is stored in lllys_when incompatible with lllys_node, so we cannot use yang_print_snode_common() */
        yang_print_open(out, &flag);
        yang_print_extension_instances(out, level, module, LLLYEXT_SUBSTMT_SELF, 0, when->ext, when->ext_size);
    }
    if (when->dsc != NULL) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_DESCRIPTION, 0, when->dsc,
                           module, when->ext, when->ext_size);
    }
    if (when->ref != NULL) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_REFERENCE, 0, when->ref,
                           module, when->ext, when->ext_size);
    }

    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_unsigned(struct lllyout *out, int level, LLLYEXT_SUBSTMT substmt, uint8_t substmt_index,
                    const struct lllys_module *module, struct lllys_ext_instance **ext, unsigned int ext_size,
                    unsigned int attr_value)
{
    char *str;

    if (asprintf(&str, "%u", attr_value) == -1) {
        LOGMEM(module->ctx);
        return;
    }
    yang_print_substmt(out, level, substmt, substmt_index, str, module, ext, ext_size);
    free(str);
}

static void
yang_print_signed(struct lllyout *out, int level, LLLYEXT_SUBSTMT substmt, uint8_t substmt_index,
                  const struct lllys_module *module, struct lllys_ext_instance **ext, unsigned int ext_size,
                  signed int attr_value)
{
    char *str;

    if (asprintf(&str, "%d", attr_value) == -1) {
        LOGMEM(module->ctx);
        return;
    }
    yang_print_substmt(out, level, substmt, substmt_index, str, module, ext, ext_size);
    free(str);
}

static void
yang_print_type(struct lllyout *out, int level, const struct lllys_module *module, const struct lllys_type *type)
{
    unsigned int i;
    int flag = 0, flag2;
    const char *str;
    char *s;
    struct lllys_module *mod;

    if (!lllys_type_is_local(type)) {
        llly_print(out, "%*stype %s:%s", LEVEL, INDENT,
                 transform_module_name2import_prefix(module, lllys_main_module(type->der->module)->name), type->der->name);
    } else {
        llly_print(out, "%*stype %s", LEVEL, INDENT, type->der->name);
    }
    level++;

    /* extensions */
    if (type->ext_size) {
        yang_print_open(out, &flag);
        yang_print_extension_instances(out, level, module, LLLYEXT_SUBSTMT_SELF, 0, type->ext, type->ext_size);
    }

    switch (type->base) {
    case LLLY_TYPE_BINARY:
        if (type->info.binary.length) {
            yang_print_open(out, &flag);
            yang_print_restr(out, level, module, type->info.binary.length, "length", type->info.binary.length->expr);
        }
        break;
    case LLLY_TYPE_BITS:
        for (i = 0; i < type->info.bits.count; ++i) {
            yang_print_open(out, &flag);
            llly_print(out, "%*sbit %s", LEVEL, INDENT, type->info.bits.bit[i].name);
            flag2 = 0;
            level++;
            yang_print_snode_common(out, level, (struct lllys_node *)&type->info.bits.bit[i], module, &flag2,
                                    SNODE_COMMON_EXT | SNODE_COMMON_IFF);
            if (!(type->info.bits.bit[i].flags & LLLYS_AUTOASSIGNED)) {
                yang_print_open(out, &flag2);
                yang_print_unsigned(out, level, LLLYEXT_SUBSTMT_POSITION, 0, module,
                                    type->info.bits.bit[i].ext, type->info.bits.bit[i].ext_size,
                                    type->info.bits.bit[i].pos);
            }
            yang_print_snode_common(out, level, (struct lllys_node *)&type->info.bits.bit[i], module, &flag2,
                                    SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);
            level--;
            yang_print_close(out, level, flag2);
        }
        break;
    case LLLY_TYPE_DEC64:
        if (!type->der->type.der) {
            yang_print_open(out, &flag);
            yang_print_unsigned(out, level, LLLYEXT_SUBSTMT_DIGITS, 0, module,
                                type->ext, type->ext_size, type->info.dec64.dig);
        }
        if (type->info.dec64.range != NULL) {
            yang_print_open(out, &flag);
            yang_print_restr(out, level, module, type->info.dec64.range, "range", type->info.dec64.range->expr);
        }
        break;
    case LLLY_TYPE_ENUM:
        for (i = 0; i < type->info.enums.count; i++) {
            yang_print_open(out, &flag);
            llly_print(out, "%*senum \"%s\"", LEVEL, INDENT, type->info.enums.enm[i].name);
            flag2 = 0;
            level++;
            yang_print_snode_common(out, level, (struct lllys_node *)&type->info.enums.enm[i], module, &flag2,
                                    SNODE_COMMON_EXT | SNODE_COMMON_IFF);
            if (!(type->info.enums.enm[i].flags & LLLYS_AUTOASSIGNED)) {
                yang_print_open(out, &flag2);
                yang_print_signed(out, level, LLLYEXT_SUBSTMT_VALUE, 0, module,
                                  type->info.enums.enm[i].ext, type->info.enums.enm[i].ext_size,
                                  type->info.enums.enm[i].value);
            }
            yang_print_snode_common(out, level, (struct lllys_node *)&type->info.enums.enm[i], module, &flag2,
                                    SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);
            level--;
            yang_print_close(out, level, flag2);
        }
        break;
    case LLLY_TYPE_IDENT:
        if (type->info.ident.count) {
            yang_print_open(out, &flag);
            for (i = 0; i < type->info.ident.count; ++i) {
                mod = lllys_main_module(type->info.ident.ref[i]->module);
                if (lllys_main_module(module) == mod) {
                    yang_print_substmt(out, level, LLLYEXT_SUBSTMT_BASE, 0, type->info.ident.ref[i]->name,
                                       module, type->info.ident.ref[i]->ext, type->info.ident.ref[i]->ext_size);
                } else {
                    if (asprintf(&s, "%s:%s", transform_module_name2import_prefix(module, mod->name),
                                 type->info.ident.ref[i]->name) == -1) {
                        LOGMEM(module->ctx);
                        return;
                    }
                    yang_print_substmt(out, level, LLLYEXT_SUBSTMT_BASE, 0, s,
                                       module, type->info.ident.ref[i]->ext, type->info.ident.ref[i]->ext_size);
                    free(s);
                }
            }
        }
        break;
    case LLLY_TYPE_INST:
        if (type->info.inst.req == 1) {
            yang_print_open(out, &flag);
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_REQINSTANCE, 0, "true", module, type->ext, type->ext_size);
        } else if (type->info.inst.req == -1) {
            yang_print_open(out, &flag);
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_REQINSTANCE, 0, "false", module, type->ext, type->ext_size);
        }
        break;
    case LLLY_TYPE_INT8:
    case LLLY_TYPE_INT16:
    case LLLY_TYPE_INT32:
    case LLLY_TYPE_INT64:
    case LLLY_TYPE_UINT8:
    case LLLY_TYPE_UINT16:
    case LLLY_TYPE_UINT32:
    case LLLY_TYPE_UINT64:
        if (type->info.num.range) {
            yang_print_open(out, &flag);
            yang_print_restr(out, level, module, type->info.num.range, "range", type->info.num.range->expr);
        }
        break;
    case LLLY_TYPE_LEAFREF:
        if (llly_strequal(type->der->name, "leafref", 0)) {
            yang_print_open(out, &flag);
            str = transform_json2schema(module, type->info.lref.path);
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_PATH, 0, str, module, type->ext, type->ext_size);
            lllydict_remove(module->ctx, str);
        }
        if (type->info.lref.req == 1) {
            yang_print_open(out, &flag);
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_REQINSTANCE, 0, "true", module, type->ext, type->ext_size);
        } else if (type->info.lref.req == -1) {
            yang_print_open(out, &flag);
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_REQINSTANCE, 0, "false", module, type->ext, type->ext_size);
        }
        break;
    case LLLY_TYPE_STRING:
        if (type->info.str.length) {
            yang_print_open(out, &flag);
            yang_print_restr(out, level, module, type->info.str.length, "length", type->info.str.length->expr);
        }
        for (i = 0; i < type->info.str.pat_count; i++) {
            yang_print_open(out, &flag);
            yang_print_restr(out, level, module, &type->info.str.patterns[i],
                             "pattern", &type->info.str.patterns[i].expr[1]);
        }
        break;
    case LLLY_TYPE_UNION:
        for (i = 0; i < type->info.uni.count; ++i) {
            yang_print_open(out, &flag);
            yang_print_type(out, level, module, &type->info.uni.types[i]);
        }
        break;
    default:
        /* other types do not have substatements */
        break;
    }

    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_must(struct lllyout *out, int level, const struct lllys_module *module, const struct lllys_restr *must)
{
    const char *str;

    str = transform_json2schema(module, must->expr);
    if (!str) {
        llly_print(out, "(!error!)");
        return;
    }
    yang_print_restr(out, level, module, must, "must", str);
    lllydict_remove(module->ctx, str);
}

static void
yang_print_unique(struct lllyout *out, int level, const struct lllys_module *module, const struct lllys_unique *uniq)
{
    int i;
    const char *str;

    llly_print(out, "%*sunique \"", LEVEL, INDENT);
    for (i = 0; i < uniq->expr_size; i++) {
        str = transform_json2schema(module, uniq->expr[i]);
        llly_print(out, "%s%s", str, i + 1 < uniq->expr_size ? " " : "");
        lllydict_remove(module->ctx, str);
    }
    llly_print(out, "\"");
}

static void
yang_print_refine(struct lllyout *out, int level, const struct lllys_module *module, const struct lllys_refine *refine)
{
    int i, flag = 0;
    const char *str;

    str = transform_json2schema(module, refine->target_name);
    llly_print(out, "%*srefine \"%s\"", LEVEL, INDENT, str);
    lllydict_remove(module->ctx, str);
    level++;

    yang_print_snode_common(out, level, (struct lllys_node *)refine, module, &flag, SNODE_COMMON_EXT | SNODE_COMMON_IFF);
    for (i = 0; i < refine->must_size; ++i) {
        yang_print_open(out, &flag);
        yang_print_must(out, level, module, &refine->must[i]);
    }
    if (refine->target_type == LLLYS_CONTAINER) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_PRESENCE, 0, refine->mod.presence,
                           module, refine->ext, refine->ext_size);
    }
    for (i = 0; i < refine->dflt_size; ++i) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_DEFAULT, i, refine->dflt[i], module, refine->ext, refine->ext_size);
    }
    if (refine->flags & LLLYS_CONFIG_W) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_CONFIG, 0, "true", module, refine->ext, refine->ext_size);
    } else if (refine->flags & LLLYS_CONFIG_R) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_CONFIG, 0, "false", module, refine->ext, refine->ext_size);
    }
    if (refine->flags & LLLYS_MAND_TRUE) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_MANDATORY, 0, "true", module, refine->ext, refine->ext_size);
    } else if (refine->flags & LLLYS_MAND_FALSE) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_MANDATORY, 0, "false", module, refine->ext, refine->ext_size);
    }
    if (refine->target_type & (LLLYS_LIST | LLLYS_LEAFLIST)) {
        if (refine->flags & LLLYS_RFN_MINSET) {
            yang_print_open(out, &flag);
            yang_print_unsigned(out, level, LLLYEXT_SUBSTMT_MIN, 0, module, refine->ext, refine->ext_size,
                                refine->mod.list.min);
        }
        if (refine->flags & LLLYS_RFN_MAXSET) {
            yang_print_open(out, &flag);
            if (refine->mod.list.max) {
                yang_print_unsigned(out, level, LLLYEXT_SUBSTMT_MAX, 0, module, refine->ext, refine->ext_size,
                                    refine->mod.list.max);
            } else {
                yang_print_substmt(out, level, LLLYEXT_SUBSTMT_MAX, 0, "unbounded", module, refine->ext, refine->ext_size);
            }
        }
    }
    yang_print_snode_common(out, level, (struct lllys_node *)refine, module, &flag, SNODE_COMMON_DSC | SNODE_COMMON_REF);

    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_deviation(struct lllyout *out, int level, const struct lllys_module *module,
                     const struct lllys_deviation *deviation)
{
    int i, j, p;
    const char *str;

    str = transform_json2schema(module, deviation->target_name);
    llly_print(out, "%*sdeviation \"%s\" {\n", LEVEL, INDENT, str);
    lllydict_remove(module->ctx, str);
    level++;

    if (deviation->ext_size) {
        yang_print_extension_instances(out, level, module, LLLYEXT_SUBSTMT_SELF, 0, deviation->ext, deviation->ext_size);
    }
    yang_print_substmt(out, level, LLLYEXT_SUBSTMT_DESCRIPTION, 0, deviation->dsc,
                       module, deviation->ext, deviation->ext_size);
    yang_print_substmt(out, level, LLLYEXT_SUBSTMT_REFERENCE, 0, deviation->ref,
                       module, deviation->ext, deviation->ext_size);

    for (i = 0; i < deviation->deviate_size; ++i) {
        llly_print(out, "%*sdeviate ", LEVEL, INDENT);
        if (deviation->deviate[i].mod == LLLY_DEVIATE_NO) {
            if (deviation->deviate[i].ext_size) {
                llly_print(out, "not-supported {\n");
            } else {
                llly_print(out, "not-supported;\n");
                continue;
            }
        } else if (deviation->deviate[i].mod == LLLY_DEVIATE_ADD) {
            llly_print(out, "add {\n");
        } else if (deviation->deviate[i].mod == LLLY_DEVIATE_RPL) {
            llly_print(out, "replace {\n");
        } else if (deviation->deviate[i].mod == LLLY_DEVIATE_DEL) {
            llly_print(out, "delete {\n");
        }
        level++;

        /* extensions */
        if (deviation->deviate[i].ext_size) {
            yang_print_extension_instances(out, level, module, LLLYEXT_SUBSTMT_SELF, 0,
                                           deviation->deviate[i].ext, deviation->deviate[i].ext_size);
        }

        /* type */
        if (deviation->deviate[i].type) {
            yang_print_type(out, level, module, deviation->deviate[i].type);
        }

        /* units */
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_UNITS, 0, deviation->deviate[i].units, module,
                           deviation->deviate[i].ext, deviation->deviate[i].ext_size);

        /* must */
        for (j = 0; j < deviation->deviate[i].must_size; ++j) {
            yang_print_must(out, level, module, &deviation->deviate[i].must[j]);
        }

        /* unique */

        for (j = 0; j < deviation->deviate[i].unique_size; ++j) {
            yang_print_unique(out, level, module, &deviation->deviate[i].unique[j]);
            /* unique's extensions */
            p = -1;
            do {
                p = lllys_ext_iter(deviation->deviate[i].ext, deviation->deviate[i].ext_size,
                                      p + 1, LLLYEXT_SUBSTMT_UNIQUE);
            } while (p != -1 && deviation->deviate[i].ext[p]->insubstmt_index != j);
            if (p != -1) {
                llly_print(out, " {\n");
                do {
                    yang_print_extension_instances(out, level + 1, module, LLLYEXT_SUBSTMT_UNIQUE, j,
                                                   &deviation->deviate[i].ext[p], 1);
                    do {
                        p = lllys_ext_iter(deviation->deviate[i].ext, deviation->deviate[i].ext_size,
                                              p + 1, LLLYEXT_SUBSTMT_UNIQUE);
                    } while (p != -1 && deviation->deviate[i].ext[p]->insubstmt_index != j);
                } while (p != -1);
                llly_print(out, "%*s}\n", LEVEL, INDENT);
            } else {
                llly_print(out, ";\n");
            }
        }

        /* default */
        for (j = 0; j < deviation->deviate[i].dflt_size; ++j) {
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_DEFAULT, j, deviation->deviate[i].dflt[j], module,
                               deviation->deviate[i].ext, deviation->deviate[i].ext_size);
        }

        /* config */
        if (deviation->deviate[i].flags & LLLYS_CONFIG_W) {
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_CONFIG, 0, "true", module,
                               deviation->deviate->ext, deviation->deviate[i].ext_size);
        } else if (deviation->deviate[i].flags & LLLYS_CONFIG_R) {
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_CONFIG, 0, "false", module,
                               deviation->deviate->ext, deviation->deviate[i].ext_size);
        }

        /* mandatory */
        if (deviation->deviate[i].flags & LLLYS_MAND_TRUE) {
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_MANDATORY, 0, "true", module,
                               deviation->deviate[i].ext, deviation->deviate[i].ext_size);
        } else if (deviation->deviate[i].flags & LLLYS_MAND_FALSE) {
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_MANDATORY, 0, "false", module,
                               deviation->deviate[i].ext, deviation->deviate[i].ext_size);
        }

        /* min-elements */
        if (deviation->deviate[i].min_set) {
            yang_print_unsigned(out, level, LLLYEXT_SUBSTMT_MIN, 0, module,
                                deviation->deviate[i].ext, deviation->deviate[i].ext_size,
                                deviation->deviate[i].min);
        }

        /* max-elements */
        if (deviation->deviate[i].max_set) {
            if (deviation->deviate[i].max) {
                yang_print_unsigned(out, level, LLLYEXT_SUBSTMT_MAX, 0, module,
                                    deviation->deviate[i].ext, deviation->deviate[i].ext_size,
                                    deviation->deviate[i].max);
            } else {
                yang_print_substmt(out, level, LLLYEXT_SUBSTMT_MAX, 0, "unbounded", module,
                                   deviation->deviate[i].ext, deviation->deviate[i].ext_size);
            }
        }

        level--;
        llly_print(out, "%*s}\n", LEVEL, INDENT);
    }

    level--;
    llly_print(out, "%*s}\n", LEVEL, INDENT);
}

static void
yang_print_augment(struct lllyout *out, int level, const struct lllys_node_augment *augment)
{
    struct lllys_node *sub;
    const char *str;

    str = transform_json2schema(augment->module, augment->target_name);
    llly_print(out, "%*saugment \"%s\" {\n", LEVEL, INDENT, str);
    lllydict_remove(augment->module->ctx, str);
    level++;

    yang_print_snode_common(out, level, (struct lllys_node *)augment, augment->module, NULL, SNODE_COMMON_EXT);
    if (augment->when) {
        yang_print_when(out, level, augment->module, augment->when);
    }
    yang_print_snode_common(out, level, (struct lllys_node *)augment, augment->module, NULL,
                            SNODE_COMMON_IFF | SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);

    LLLY_TREE_FOR(augment->child, sub) {
        /* only our augment */
        if (sub->parent != (struct lllys_node *)augment) {
            continue;
        }
        yang_print_snode(out, level, sub,
                         LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST |
                         LLLYS_USES | LLLYS_ANYDATA | LLLYS_CASE | LLLYS_ACTION | LLLYS_NOTIF);
    }

    level--;
    llly_print(out, "%*s}\n", LEVEL, INDENT);
}

static void
yang_print_typedef(struct lllyout *out, int level, const struct lllys_module *module, const struct lllys_tpdf *tpdf)
{
    const char *dflt;

    llly_print(out, "%*stypedef %s {\n", LEVEL, INDENT, tpdf->name);
    level++;

    yang_print_snode_common(out, level, (struct lllys_node *)tpdf, module, NULL, SNODE_COMMON_EXT);
    yang_print_type(out, level, module, &tpdf->type);
    if (tpdf->units != NULL) {
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_UNITS, 0, tpdf->units, module, tpdf->ext, tpdf->ext_size);
    }
    if (tpdf->dflt != NULL) {
        if (tpdf->flags & LLLYS_DFLTJSON) {
            assert(strchr(tpdf->dflt, ':'));
            if (!strncmp(tpdf->dflt, module->name, strchr(tpdf->dflt, ':') - tpdf->dflt)) {
                /* local module */
                dflt = lllydict_insert(module->ctx, strchr(tpdf->dflt, ':') + 1, 0);
            } else {
                dflt = transform_json2schema(module, tpdf->dflt);
            }
        } else {
            dflt = tpdf->dflt;
        }
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_DEFAULT, 0, dflt, module, tpdf->ext, tpdf->ext_size);
        if (tpdf->flags & LLLYS_DFLTJSON) {
            lllydict_remove(module->ctx, dflt);
        }
    }
    yang_print_snode_common(out, level, (struct lllys_node *)tpdf, module, NULL,
                            SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);

    level--;
    llly_print(out, "%*s}\n", LEVEL, INDENT);
}

static void
yang_print_identity(struct lllyout *out, int level, const struct lllys_ident *ident)
{
    int flag = 0, i;
    struct lllys_module *mod;
    char *str;

    llly_print(out, "%*sidentity %s", LEVEL, INDENT, ident->name);
    level++;

    yang_print_snode_common(out, level, (struct lllys_node *)ident, ident->module, &flag,
                            SNODE_COMMON_EXT | SNODE_COMMON_IFF);

    for (i = 0; i < ident->base_size; i++) {
        yang_print_open(out, &flag);
        mod = lllys_main_module(ident->base[i]->module);
        if (lllys_main_module(ident->module) == mod) {
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_BASE, i, ident->base[i]->name,
                               ident->module, ident->ext, ident->ext_size);
        } else {
            if (asprintf(&str, "%s:%s", transform_module_name2import_prefix(ident->module, mod->name), ident->base[i]->name) == -1) {
                LOGMEM(ident->module->ctx);
                return;
            }
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_BASE, i, str,
                               ident->module, ident->ext, ident->ext_size);
            free(str);
        }
    }

    yang_print_snode_common(out, level, (struct lllys_node *)ident, ident->module, &flag,
                            SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);

    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_container(struct lllyout *out, int level, const struct lllys_node *node)
{
    int i, flag = 0;
    struct lllys_node *sub;
    struct lllys_node_container *cont = (struct lllys_node_container *)node;

    llly_print(out, "%*scontainer %s", LEVEL, INDENT, node->name);
    level++;

    yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_EXT);
    if (cont->when) {
        yang_print_open(out, &flag);
        yang_print_when(out, level, node->module, cont->when);
    }
    for (i = 0; i < cont->iffeature_size; i++) {
        yang_print_open(out, &flag);
        yang_print_iffeature(out, level, node->module, &cont->iffeature[i]);
    }
    for (i = 0; i < cont->must_size; i++) {
        yang_print_open(out, &flag);
        yang_print_must(out, level, node->module, &cont->must[i]);
    }
    if (cont->presence != NULL) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_PRESENCE, 0, cont->presence,
                           node->module, node->ext, node->ext_size);
    }
    yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_CONFIG |
                            SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);
    for (i = 0; i < cont->tpdf_size; i++) {
        yang_print_open(out, &flag);
        yang_print_typedef(out, level, node->module, &cont->tpdf[i]);
    }
    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_GROUPING);
    }
    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST |
                         LLLYS_USES | LLLYS_ANYDATA );
    }
    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_ACTION);
    }
    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_NOTIF);
    }

    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_case(struct lllyout *out, int level, const struct lllys_node *node)
{
    int flag = 0;
    struct lllys_node *sub;
    struct lllys_node_case *cas = (struct lllys_node_case *)node;

    if (!(node->flags & LLLYS_IMPLICIT)) {
        llly_print(out, "%*scase %s", LEVEL, INDENT, cas->name);
        level++;

        yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_EXT);
        if (cas->when) {
            yang_print_open(out, &flag);
            yang_print_when(out, level, node->module, cas->when);
        }
        yang_print_snode_common(out, level, node, node->module, &flag,
                                SNODE_COMMON_IFF | SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);
    } else {
        flag = 1;
    }

    /* print children */
    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub,
                         LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST |
                         LLLYS_USES | LLLYS_ANYDATA);
    }

    if (node->flags & LLLYS_IMPLICIT) {
        /* do not print anything about the case, it was implicitely added by libyang */
        return;
    }

    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_choice(struct lllyout *out, int level, const struct lllys_node *node)
{
    int i, flag = 0;
    struct lllys_node *sub;
    struct lllys_node_choice *choice = (struct lllys_node_choice *)node;

    llly_print(out, "%*schoice %s", LEVEL, INDENT, node->name);
    level++;

    yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_EXT);
    if (choice->when) {
        yang_print_open(out, &flag);
        yang_print_when(out, level, node->module, choice->when);
    }
    for (i = 0; i < choice->iffeature_size; i++) {
        yang_print_open(out, &flag);
        yang_print_iffeature(out, level, node->module, &choice->iffeature[i]);
    }
    if (choice->dflt != NULL) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_DEFAULT, 0, choice->dflt->name,
                           node->module, node->ext, node->ext_size);
    }
    yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_CONFIG | SNODE_COMMON_MAND |
                            SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);

    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub,
                         LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_ANYDATA | LLLYS_CASE);
    }

    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_leaf(struct lllyout *out, int level, const struct lllys_node *node)
{
    int i;
    struct lllys_node_leaf *leaf = (struct lllys_node_leaf *)node;
    const char *dflt;

    llly_print(out, "%*sleaf %s {\n", LEVEL, INDENT, node->name);
    level++;

    yang_print_snode_common(out, level, node, node->module, NULL, SNODE_COMMON_EXT);
    if (leaf->when) {
        yang_print_when(out, level, node->module, leaf->when);
    }
    for (i = 0; i < leaf->iffeature_size; i++) {
        yang_print_iffeature(out, level, node->module, &leaf->iffeature[i]);
    }
    yang_print_type(out, level, node->module, &leaf->type);
    yang_print_substmt(out, level, LLLYEXT_SUBSTMT_UNITS, 0, leaf->units,
                       node->module, node->ext, node->ext_size);
    for (i = 0; i < leaf->must_size; i++) {
        yang_print_must(out, level, node->module, &leaf->must[i]);
    }
    if (leaf->dflt) {
        if (leaf->flags & LLLYS_DFLTJSON) {
            assert(strchr(leaf->dflt, ':'));
            if (!strncmp(leaf->dflt, lllys_node_module(node)->name, strchr(leaf->dflt, ':') - leaf->dflt)) {
                /* local module */
                dflt = lllydict_insert(node->module->ctx, strchr(leaf->dflt, ':') + 1, 0);
            } else {
                dflt = transform_json2schema(node->module, leaf->dflt);
            }
        } else {
            dflt = leaf->dflt;
        }
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_DEFAULT, 0, dflt,
                           node->module, node->ext, node->ext_size);
        if (leaf->flags & LLLYS_DFLTJSON) {
            lllydict_remove(node->module->ctx, dflt);
        }
    }
    yang_print_snode_common(out, level, node, node->module, NULL, SNODE_COMMON_CONFIG | SNODE_COMMON_MAND |
                            SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);
    level--;
    llly_print(out, "%*s}\n", LEVEL, INDENT);
}

static void
yang_print_anydata(struct lllyout *out, int level, const struct lllys_node *node)
{
    int i, flag = 0;
    struct lllys_node_anydata *any = (struct lllys_node_anydata *)node;

    if (!lllys_parent(node) && !strcmp(node->name, "config") && !strcmp(node->module->name, "ietf-netconf")) {
        /* node added by libyang, not actually in the model */
        return;
    }

    llly_print(out, "%*s%s %s", LEVEL, INDENT, any->nodetype == LLLYS_ANYXML ? "anyxml" : "anydata", any->name);
    level++;

    yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_EXT);
    if (any->when) {
        yang_print_open(out, &flag);
        yang_print_when(out, level, node->module, any->when);
    }
    for (i = 0; i < any->iffeature_size; i++) {
        yang_print_open(out, &flag);
        yang_print_iffeature(out, level, node->module, &any->iffeature[i]);
    }
    for (i = 0; i < any->must_size; i++) {
        yang_print_open(out, &flag);
        yang_print_must(out, level, node->module, &any->must[i]);
    }
    yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_CONFIG | SNODE_COMMON_MAND |
                            SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);
    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_leaflist(struct lllyout *out, int level, const struct lllys_node *node)
{
    int i;
    struct lllys_node_leaflist *llist = (struct lllys_node_leaflist *)node;
    const char *dflt;

    llly_print(out, "%*sleaf-list %s {\n", LEVEL, INDENT, node->name);
    level++;
    yang_print_snode_common(out, level, node, node->module, NULL, SNODE_COMMON_EXT);
    if (llist->when) {
        yang_print_when(out, level, llist->module, llist->when);
    }
    for (i = 0; i < llist->iffeature_size; i++) {
        yang_print_iffeature(out, level, node->module, &llist->iffeature[i]);
    }
    yang_print_type(out, level, node->module, &llist->type);
    yang_print_substmt(out, level, LLLYEXT_SUBSTMT_UNITS, 0, llist->units,
                       node->module, node->ext, node->ext_size);
    for (i = 0; i < llist->must_size; i++) {
        yang_print_must(out, level, node->module, &llist->must[i]);
    }
    for (i = 0; i < llist->dflt_size; ++i) {
        if (llist->flags & LLLYS_DFLTJSON) {
            assert(strchr(llist->dflt[i], ':'));
            if (!strncmp(llist->dflt[i], lllys_node_module(node)->name, strchr(llist->dflt[i], ':') - llist->dflt[i])) {
                /* local module */
                dflt = lllydict_insert(node->module->ctx, strchr(llist->dflt[i], ':') + 1, 0);
            } else {
                dflt = transform_json2schema(node->module, llist->dflt[i]);
            }
        } else {
            dflt = llist->dflt[i];
        }
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_DEFAULT, i, dflt,
                           node->module, node->ext, node->ext_size);
        if (llist->flags & LLLYS_DFLTJSON) {
            lllydict_remove(node->module->ctx, dflt);
        }
    }
    yang_print_snode_common(out, level, node, node->module, NULL, SNODE_COMMON_CONFIG);
    if (llist->min > 0) {
        yang_print_unsigned(out, level, LLLYEXT_SUBSTMT_MIN, 0, node->module, node->ext, node->ext_size, llist->min);
    }
    if (llist->max > 0) {
        yang_print_unsigned(out, level, LLLYEXT_SUBSTMT_MAX, 0, node->module, node->ext, node->ext_size, llist->max);
    }
    if (llist->flags & LLLYS_USERORDERED) {
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_ORDEREDBY, 0, "user",
                           node->module, node->ext, node->ext_size);
    } else if (lllys_ext_iter(node->ext, node->ext_size, 0, LLLYEXT_SUBSTMT_ORDEREDBY) != -1) {
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_ORDEREDBY, 0, "system",
                           node->module, node->ext, node->ext_size);
    }
    yang_print_snode_common(out, level, node, node->module, NULL,
                            SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);
    level--;
    llly_print(out, "%*s}\n", LEVEL, INDENT);
}

static void
yang_print_list(struct lllyout *out, int level, const struct lllys_node *node)
{
    int i, p, flag = 0;
    struct lllys_node *sub;
    struct lllys_node_list *list = (struct lllys_node_list *)node;

    llly_print(out, "%*slist %s", LEVEL, INDENT, node->name);
    level++;
    yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_EXT);
    if (list->when) {
        yang_print_open(out, &flag);
        yang_print_when(out, level, list->module, list->when);
    }
    for (i = 0; i < list->iffeature_size; i++) {
        yang_print_open(out, &flag);
        yang_print_iffeature(out, level, node->module, &list->iffeature[i]);
    }
    for (i = 0; i < list->must_size; i++) {
        yang_print_open(out, &flag);
        yang_print_must(out, level, list->module, &list->must[i]);
    }
    if (list->keys_size) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_KEY, 0, list->keys_str,
                           node->module, node->ext, node->ext_size);
    }
    for (i = 0; i < list->unique_size; i++) {
        yang_print_open(out, &flag);
        yang_print_unique(out, level, node->module, &list->unique[i]);
        /* unique's extensions */
        p = -1;
        do {
            p = lllys_ext_iter(list->ext, list->ext_size, p + 1, LLLYEXT_SUBSTMT_UNIQUE);
        } while (p != -1 && list->ext[p]->insubstmt_index != i);
        if (p != -1) {
            llly_print(out, " {\n");
            do {
                yang_print_extension_instances(out, level + 1, list->module, LLLYEXT_SUBSTMT_UNIQUE, i, &list->ext[p], 1);
                do {
                    p = lllys_ext_iter(list->ext, list->ext_size, p + 1, LLLYEXT_SUBSTMT_UNIQUE);
                } while (p != -1 && list->ext[p]->insubstmt_index != i);
            } while (p != -1);
            llly_print(out, "%*s}\n", LEVEL, INDENT);
        } else {
            llly_print(out, ";\n");
        }
    }
    yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_CONFIG);
    if (list->min > 0) {
        yang_print_open(out, &flag);
        yang_print_unsigned(out, level, LLLYEXT_SUBSTMT_MIN, 0, node->module, node->ext, node->ext_size, list->min);
    }
    if (list->max > 0) {
        yang_print_open(out, &flag);
        yang_print_unsigned(out, level, LLLYEXT_SUBSTMT_MAX, 0, node->module, node->ext, node->ext_size, list->max);
    }
    if (list->flags & LLLYS_USERORDERED) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_ORDEREDBY, 0, "user",
                           node->module, node->ext, node->ext_size);
    } else if (lllys_ext_iter(node->ext, node->ext_size, 0, LLLYEXT_SUBSTMT_ORDEREDBY) != -1) {
        yang_print_open(out, &flag);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_ORDEREDBY, 0, "system",
                           node->module, node->ext, node->ext_size);
    }
    yang_print_snode_common(out, level, node, node->module, &flag,
                            SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);
    for (i = 0; i < list->tpdf_size; i++) {
        yang_print_open(out, &flag);
        yang_print_typedef(out, level, list->module, &list->tpdf[i]);
    }

    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_GROUPING);
    }

    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST |
                         LLLYS_USES | LLLYS_ANYDATA);
    }

    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_ACTION);
    }

    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_NOTIF);
    }

    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_grouping(struct lllyout *out, int level, const struct lllys_node *node)
{
    int i, flag = 0;
    struct lllys_node *sub;
    struct lllys_node_grp *grp = (struct lllys_node_grp *)node;

    llly_print(out, "%*sgrouping %s", LEVEL, INDENT, node->name);
    level++;

    yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_EXT | SNODE_COMMON_STATUS |
                            SNODE_COMMON_DSC | SNODE_COMMON_REF);

    for (i = 0; i < grp->tpdf_size; i++) {
        yang_print_open(out, &flag);
        yang_print_typedef(out, level, node->module, &grp->tpdf[i]);
    }

    LLLY_TREE_FOR(node->child, sub) {
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_GROUPING);
    }

    LLLY_TREE_FOR(node->child, sub) {
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST |
                         LLLYS_USES | LLLYS_ANYDATA);
    }

    LLLY_TREE_FOR(node->child, sub) {
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_ACTION);
    }

    LLLY_TREE_FOR(node->child, sub) {
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_NOTIF);
    }

    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_uses(struct lllyout *out, int level, const struct lllys_node *node)
{
    int i, flag = 0;
    struct lllys_node_uses *uses = (struct lllys_node_uses *)node;
    struct lllys_module *mod;

    llly_print(out, "%*suses ", LEVEL, INDENT);
    if (node->child) {
        mod = lllys_node_module(node->child);
        if (lllys_node_module(node) != mod) {
            llly_print(out, "%s:", transform_module_name2import_prefix(node->module, mod->name));
        }
    }
    llly_print(out, "%s", uses->name);
    level++;

    yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_EXT);
    if (uses->when) {
        yang_print_open(out, &flag);
        yang_print_when(out, level, node->module, uses->when);
    }
    yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_IFF |
                            SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);
    for (i = 0; i < uses->refine_size; i++) {
        yang_print_open(out, &flag);
        yang_print_refine(out, level, node->module, &uses->refine[i]);
    }
    for (i = 0; i < uses->augment_size; i++) {
        yang_print_open(out, &flag);
        yang_print_augment(out, level, &uses->augment[i]);
    }

    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_input_output(struct lllyout *out, int level, const struct lllys_node *node)
{
    int i;
    struct lllys_node *sub;
    struct lllys_node_inout *inout = (struct lllys_node_inout *)node;

    llly_print(out, "%*s%s {\n", LEVEL, INDENT, (inout->nodetype == LLLYS_INPUT ? "input" : "output"));
    level++;

    if (node->ext_size) {
        yang_print_extension_instances(out, level, node->module, LLLYEXT_SUBSTMT_SELF, 0, node->ext, node->ext_size);
    }
    for (i = 0; i < inout->must_size; i++) {
        yang_print_must(out, level, node->module, &inout->must[i]);
    }
    for (i = 0; i < inout->tpdf_size; i++) {
        yang_print_typedef(out, level, node->module, &inout->tpdf[i]);
    }
    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_snode(out, level, sub, LLLYS_GROUPING);
    }
    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_snode(out, level, sub, LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST |
                         LLLYS_USES | LLLYS_ANYDATA);
    }

    level--;
    llly_print(out, "%*s}\n", LEVEL, INDENT);
}

static void
yang_print_rpc_action(struct lllyout *out, int level, const struct lllys_node *node)
{
    int i, flag = 0;
    struct lllys_node *sub;
    struct lllys_node_rpc_action *rpc = (struct lllys_node_rpc_action *)node;

    llly_print(out, "%*s%s %s", LEVEL, INDENT, (node->nodetype == LLLYS_RPC ? "rpc" : "action"), node->name);

    level++;
    yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_EXT | SNODE_COMMON_IFF |
                            SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);

    for (i = 0; i < rpc->tpdf_size; i++) {
        yang_print_open(out, &flag);
        yang_print_typedef(out, level, node->module, &rpc->tpdf[i]);
    }

    LLLY_TREE_FOR(node->child, sub) {
        /* augments and implicit nodes */
        if ((sub->parent != node) || ((sub->nodetype & (LLLYS_INPUT | LLLYS_OUTPUT) && (sub->flags & LLLYS_IMPLICIT)))) {
            continue;
        }
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_INPUT | LLLYS_OUTPUT | LLLYS_GROUPING);
    }

    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_notif(struct lllyout *out, int level, const struct lllys_node *node)
{
    int i, flag = 0;
    struct lllys_node *sub;
    struct lllys_node_notif *notif = (struct lllys_node_notif *)node;

    llly_print(out, "%*snotification %s", LEVEL, INDENT, node->name);

    level++;
    yang_print_snode_common(out, level, node, node->module, &flag, SNODE_COMMON_EXT | SNODE_COMMON_IFF);
    for (i = 0; i < notif->must_size; i++) {
        yang_print_open(out, &flag);
        yang_print_must(out, level, node->module, &notif->must[i]);
    }
    yang_print_snode_common(out, level, node, node->module, &flag,
                            SNODE_COMMON_STATUS | SNODE_COMMON_DSC | SNODE_COMMON_REF);
    for (i = 0; i < notif->tpdf_size; i++) {
        yang_print_open(out, &flag);
        yang_print_typedef(out, level, node->module, &notif->tpdf[i]);
    }
    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub, LLLYS_GROUPING);
    }
    LLLY_TREE_FOR(node->child, sub) {
        /* augments */
        if (sub->parent != node) {
            continue;
        }
        yang_print_open(out, &flag);
        yang_print_snode(out, level, sub,
                         LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST |
                         LLLYS_USES | LLLYS_ANYDATA);
    }

    level--;
    yang_print_close(out, level, flag);
}

static void
yang_print_snode(struct lllyout *out, int level, const struct lllys_node *node, int mask)
{
    if (node->nodetype & mask) {
        if ((node->nodetype & (LLLYS_INPUT | LLLYS_OUTPUT)) && (node->flags & LLLYS_IMPLICIT)) {
            /* implicit input/output node is not supposed to be printed */
            return;
        } else if (!node->parent ||
                (node->parent->nodetype == LLLYS_AUGMENT && node != node->parent->child) ||
                (node->parent->nodetype != LLLYS_AUGMENT && node->prev->next)) {
            /* do not print the blank line before the first data-def node */
            llly_print(out, "\n");
        }
    }

    switch (node->nodetype & mask) {
    case LLLYS_CONTAINER:
        yang_print_container(out, level, node);
        break;
    case LLLYS_CHOICE:
        yang_print_choice(out, level, node);
        break;
    case LLLYS_LEAF:
        yang_print_leaf(out, level, node);
        break;
    case LLLYS_LEAFLIST:
        yang_print_leaflist(out, level, node);
        break;
    case LLLYS_LIST:
        yang_print_list(out, level, node);
        break;
    case LLLYS_USES:
        yang_print_uses(out, level, node);
        break;
    case LLLYS_GROUPING:
        yang_print_grouping(out, level, node);
        break;
    case LLLYS_ANYXML:
    case LLLYS_ANYDATA:
        yang_print_anydata(out, level, node);
        break;
    case LLLYS_CASE:
        yang_print_case(out, level, node);
        break;
    case LLLYS_RPC:
    case LLLYS_ACTION:
        yang_print_rpc_action(out, level, node);
        break;
    case LLLYS_INPUT:
    case LLLYS_OUTPUT:
        yang_print_input_output(out, level, node);
        break;
    case LLLYS_NOTIF:
        yang_print_notif(out, level, node);
        break;
    default:
        break;
    }
}

static void
yang_print_revision(struct lllyout *out, int level, const struct lllys_module *module, const struct lllys_revision *rev)
{
    if (rev->dsc || rev->ref || rev->ext_size) {
        llly_print(out, "%*srevision %s {\n", LEVEL, INDENT, rev->date);
        yang_print_extension_instances(out, level + 1, module, LLLYEXT_SUBSTMT_SELF, 0, rev->ext, rev->ext_size);
        yang_print_substmt(out, level + 1, LLLYEXT_SUBSTMT_DESCRIPTION, 0, rev->dsc, module, rev->ext, rev->ext_size);
        yang_print_substmt(out, level + 1, LLLYEXT_SUBSTMT_REFERENCE, 0, rev->ref, module, rev->ext, rev->ext_size);
        llly_print(out, "%*s}\n", LEVEL, INDENT);
    } else {
        llly_print(out, "%*srevision %s;\n", LEVEL, INDENT, rev->date);
    }
}

static int
yang_print_model_(struct lllyout *out, int level, const struct lllys_module *module)
{
    unsigned int i;
    int p;

    struct lllys_node *node;

    /* (sub)module-header-stmts */
    if (module->type) {
        llly_print(out, "%*ssubmodule %s {%s\n", LEVEL, INDENT, module->name,
                 (module->deviated == 1 ? " // DEVIATED" : ""));
        level++;
        if (module->version || lllys_ext_iter(module->ext, module->ext_size, 0, LLLYEXT_SUBSTMT_VERSION) != -1) {
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_VERSION, 0, module->version == LLLYS_VERSION_1_1 ? "1.1" : "1",
                               module, module->ext, module->ext_size);
        }
        llly_print(out, "%*sbelongs-to %s {\n", LEVEL, INDENT, ((struct lllys_submodule *)module)->belongsto->name);
        p = -1;
        while ((p = lllys_ext_iter(module->ext, module->ext_size, p + 1, LLLYEXT_SUBSTMT_BELONGSTO)) != -1) {
            yang_print_extension_instances(out, level + 1, module, LLLYEXT_SUBSTMT_BELONGSTO, 0, &module->ext[p], 1);
        }
        yang_print_substmt(out, level + 1, LLLYEXT_SUBSTMT_PREFIX, 0, module->prefix,
                           module, module->ext, module->ext_size);
        llly_print(out, "%*s}\n", LEVEL, INDENT);
    } else {
        llly_print(out, "%*smodule %s {%s\n", LEVEL, INDENT, module->name,
                 (module->deviated == 1 ? " // DEVIATED" : ""));
        level++;
        if (module->version) {
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_VERSION, 0, module->version == LLLYS_VERSION_1_1 ? "1.1" : "1",
                               module, module->ext, module->ext_size);
        }
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_NAMESPACE, 0, module->ns,
                           module, module->ext, module->ext_size);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_PREFIX, 0, module->prefix,
                           module, module->ext, module->ext_size);
    }

    /* linkage-stmts */
    for (i = 0; i < module->imp_size; i++) {
        llly_print(out, "\n%*simport %s {\n", LEVEL, INDENT, module->imp[i].module->name);
        level++;
        yang_print_extension_instances(out, level, module, LLLYEXT_SUBSTMT_SELF, 0,
                                       module->imp[i].ext, module->imp[i].ext_size);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_PREFIX, 0, module->imp[i].prefix,
                           module, module->imp[i].ext, module->imp[i].ext_size);
        if (module->imp[i].rev[0]) {
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_REVISIONDATE, 0, module->imp[i].rev,
                               module, module->imp[i].ext, module->imp[i].ext_size);
        }
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_DESCRIPTION, 0, module->imp[i].dsc,
                           module, module->imp[i].ext, module->imp[i].ext_size);
        yang_print_substmt(out, level, LLLYEXT_SUBSTMT_REFERENCE, 0, module->imp[i].ref,
                           module, module->imp[i].ext, module->imp[i].ext_size);
        level--;
        llly_print(out, "%*s}\n", LEVEL, INDENT);
    }
    for (i = 0; i < module->inc_size; i++) {
        if (module->inc[i].rev[0] || module->inc[i].dsc || module->inc[i].ref || module->inc[i].ext_size) {
            llly_print(out, "\n%*sinclude %s {\n", LEVEL, INDENT, module->inc[i].submodule->name);
            level++;
            yang_print_extension_instances(out, level, module, LLLYEXT_SUBSTMT_SELF, 0,
                                           module->inc[i].ext, module->inc[i].ext_size);
            if (module->inc[i].rev[0]) {
                yang_print_substmt(out, level, LLLYEXT_SUBSTMT_REVISIONDATE, 0, module->inc[i].rev,
                                   module, module->inc[i].ext, module->inc[i].ext_size);
            }
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_DESCRIPTION, 0, module->inc[i].dsc,
                               module, module->inc[i].ext, module->inc[i].ext_size);
            yang_print_substmt(out, level, LLLYEXT_SUBSTMT_REFERENCE, 0, module->inc[i].ref,
                               module, module->inc[i].ext, module->inc[i].ext_size);
            level--;
            llly_print(out, "%*s}\n", LEVEL, INDENT);
        } else {
            llly_print(out, "\n%*sinclude \"%s\";\n", LEVEL, INDENT, module->inc[i].submodule->name);
        }
    }

    /* meta-stmts */
    if (module->org || module->contact || module->dsc || module->ref) {
        llly_print(out, "\n");
    }
    yang_print_substmt(out, level, LLLYEXT_SUBSTMT_ORGANIZATION, 0, module->org,
                       module, module->ext, module->ext_size);
    yang_print_substmt(out, level, LLLYEXT_SUBSTMT_CONTACT, 0, module->contact,
                       module, module->ext, module->ext_size);
    yang_print_substmt(out, level, LLLYEXT_SUBSTMT_DESCRIPTION, 0, module->dsc,
                       module, module->ext, module->ext_size);
    yang_print_substmt(out, level, LLLYEXT_SUBSTMT_REFERENCE, 0, module->ref,
                       module, module->ext, module->ext_size);

    /* revision-stmts */
    if (module->rev_size) {
        llly_print(out, "\n");
    }
    for (i = 0; i < module->rev_size; i++) {
        yang_print_revision(out, level, module, &module->rev[i]);
    }

    /* body-stmts */
    for (i = 0; i < module->extensions_size; i++) {
        llly_print(out, "\n");
        yang_print_extension(out, level, &module->extensions[i]);
    }
    if (module->ext_size) {
        llly_print(out, "\n");
        yang_print_extension_instances(out, level, module, LLLYEXT_SUBSTMT_SELF, 0, module->ext, module->ext_size);
    }

    for (i = 0; i < module->features_size; i++) {
        llly_print(out, "\n");
        yang_print_feature(out, level, &module->features[i]);
    }

    for (i = 0; i < module->ident_size; i++) {
        llly_print(out, "\n");
        yang_print_identity(out, level, &module->ident[i]);
    }

    for (i = 0; i < module->tpdf_size; i++) {
        llly_print(out, "\n");
        yang_print_typedef(out, level, module, &module->tpdf[i]);
    }

    LLLY_TREE_FOR(lllys_main_module(module)->data, node) {
        if (node->module != module) {
            /* data from submodules */
            continue;
        }
        yang_print_snode(out, level, node, LLLYS_GROUPING);
    }

    LLLY_TREE_FOR(lllys_main_module(module)->data, node) {
        if (node->module != module) {
            /* data from submodules */
            continue;
        }
        yang_print_snode(out, level, node, LLLYS_CHOICE | LLLYS_CONTAINER | LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST |
                             LLLYS_USES | LLLYS_ANYDATA);
    }

    for (i = 0; i < module->augment_size; i++) {
        llly_print(out, "\n");
        yang_print_augment(out, level, &module->augment[i]);
    }

    LLLY_TREE_FOR(lllys_main_module(module)->data, node) {
        if (node->module != module) {
            /* data from submodules */
            continue;
        }
        yang_print_snode(out, level, node, LLLYS_RPC | LLLYS_ACTION);
    }

    LLLY_TREE_FOR(lllys_main_module(module)->data, node) {
        if (node->module != module) {
            /* data from submodules */
            continue;
        }
        yang_print_snode(out, level, node, LLLYS_NOTIF);
    }

    for (i = 0; i < module->deviation_size; ++i) {
        llly_print(out, "\n");
        yang_print_deviation(out, level, module, &module->deviation[i]);
    }

    level--;
    llly_print(out, "%*s}\n", LEVEL, INDENT);
    llly_print_flush(out);

    return EXIT_SUCCESS;
}

int
yang_print_model(struct lllyout *out, const struct lllys_module *module)
{
    return yang_print_model_(out, 0, module);
}

static void
yang_print_extcomplex_bool(struct lllyout *out, int level, const struct lllys_module *module,
                           struct lllys_ext_instance_complex *ext, LLLY_STMT stmt,
                           const char *true_val, const char *false_val, int *content)
{
    struct lllyext_substmt *info;
    uint8_t *val;

    val = lllys_ext_complex_get_substmt(stmt, ext, &info);
    if (!val || !(*val)) {
        return;
    }

    yang_print_open(out, content);
    if (*val == 1) {
        yang_print_substmt(out, level, (LLLYEXT_SUBSTMT)stmt, 0, true_val, module, ext->ext, ext->ext_size);
    } else if (*val == 2) {
        yang_print_substmt(out, level, (LLLYEXT_SUBSTMT)stmt, 0, false_val, module, ext->ext, ext->ext_size);
    } else {
        LOGINT(module->ctx);
    }
}

static void
yang_print_extcomplex_str(struct lllyout *out, int level, const struct lllys_module *module,
                          struct lllys_ext_instance_complex *ext, LLLY_STMT stmt, int *content)
{
    struct lllyext_substmt *info;
    const char **str;
    int c;

    str = lllys_ext_complex_get_substmt(stmt, ext, &info);
    if (!str || !(*str)) {
        return;
    }
    if (info->cardinality >= LLLY_STMT_CARD_SOME) {
        /* we have array */
        for (str = (const char **)(*str), c = 0; *str; str++, c++) {
            yang_print_open(out, content);
            yang_print_substmt(out, level, (LLLYEXT_SUBSTMT)stmt, c, *str, module, ext->ext, ext->ext_size);
        }
    } else {
        yang_print_open(out, content);
        yang_print_substmt(out, level, (LLLYEXT_SUBSTMT)stmt, 0, *str, module, ext->ext, ext->ext_size);
    }
}

/* val1 is supposed to be the default value */
static void
yang_print_extcomplex_flags(struct lllyout *out, int level, const struct lllys_module *module,
                            struct lllys_ext_instance_complex *ext, LLLY_STMT stmt,
                            const char *val1_str, const char *val2_str, uint16_t val1, uint16_t val2,
                            int *content)
{
    const char *str;
    uint16_t *flags;

    flags = lllys_ext_complex_get_substmt(stmt, ext, NULL);
    if (!flags) {
        return;
    }

    if (val1 & *flags) {
        str = val1_str;
    } else if (val2 & *flags) {
        str = val2_str;
    } else if (lllys_ext_iter(ext->ext, ext->ext_size, 0, (LLLYEXT_SUBSTMT)stmt) != -1) {
        /* flag not set, but since there are some extension, we are going to print the default value */
        str = val1_str;
    } else {
        return;
    }

    yang_print_open(out, content);
    yang_print_substmt(out, level, (LLLYEXT_SUBSTMT)stmt, 0, str, module, ext->ext, ext->ext_size);
}

static void
yang_print_extension_instances(struct lllyout *out, int level, const struct lllys_module *module,
                               LLLYEXT_SUBSTMT substmt, uint8_t substmt_index,
                               struct lllys_ext_instance **ext, unsigned int count)
{
    unsigned int u, x;
    struct lllys_module *mod;
    const char *prefix = NULL, *str;
    char *ext_name;
    int content, content2, i, j, c;
    struct lllyext_substmt *info;
    uint16_t *flags;
    void **pp, *p;
    struct lllys_node *siter;

#define YANG_PRINT_EXTCOMPLEX_SNODE(STMT)                                                     \
    pp = lllys_ext_complex_get_substmt(STMT, (struct lllys_ext_instance_complex *)ext[u], NULL);  \
    if (!pp || !(*pp)) { break; }                                                             \
    LLLY_TREE_FOR((struct lllys_node*)(*pp), siter) {                                             \
        if (lllys_snode2stmt(siter->nodetype) == STMT) {                                        \
            yang_print_open(out, &content);                                                   \
            yang_print_snode(out, level, siter, LLLYS_ANY);                                     \
        }                                                                                     \
    }
#define YANG_PRINT_EXTCOMPLEX_STRUCT(STMT, TYPE, FUNC)                                        \
    pp = lllys_ext_complex_get_substmt(STMT, (struct lllys_ext_instance_complex *)ext[u], NULL);  \
    if (!pp || !(*pp)) { break; }                                                             \
    if (info[i].cardinality >= LLLY_STMT_CARD_SOME) { /* process array */                       \
        for (pp = *pp; *pp; pp++) {                                                           \
            yang_print_open(out, &content);                                                   \
            FUNC(out, level, (TYPE *)(*pp));                                                  \
        }                                                                                     \
    } else { /* single item */                                                                \
        yang_print_open(out, &content);                                                       \
        FUNC(out, level, (TYPE *)(*pp));                                                      \
    }
#define YANG_PRINT_EXTCOMPLEX_STRUCT_M(STMT, TYPE, FUNC, ARGS...)                             \
    pp = lllys_ext_complex_get_substmt(STMT, (struct lllys_ext_instance_complex *)ext[u], NULL);  \
    if (!pp || !(*pp)) { break; }                                                             \
    if (info[i].cardinality >= LLLY_STMT_CARD_SOME) { /* process array */                       \
        for (pp = *pp; *pp; pp++) {                                                           \
            yang_print_open(out, &content);                                                   \
            FUNC(out, level, module, (TYPE *)(*pp), ##ARGS);                                  \
        }                                                                                     \
    } else { /* single item */                                                                \
        yang_print_open(out, &content);                                                       \
        FUNC(out, level, module, (TYPE *)(*pp), ##ARGS);                                      \
    }
#define YANG_PRINT_EXTCOMPLEX_INT(STMT, TYPE, SIGN)                                \
    p = &((struct lllys_ext_instance_complex*)ext[u])->content[info[i].offset];      \
    if (!p || !*(TYPE**)p) { break; }                                              \
    if (info[i].cardinality >= LLLY_STMT_CARD_SOME) { /* we have array */            \
        for (c = 0; (*(TYPE***)p)[c]; c++) {                                       \
            yang_print_open(out, &content);                                        \
            yang_print_##SIGN(out, level, STMT, c, module,                         \
                                ext[u]->ext, ext[u]->ext_size, *(*(TYPE***)p)[c]); \
        }                                                                          \
    } else {                                                                       \
        yang_print_open(out, &content);                                            \
        yang_print_##SIGN(out, level, STMT, 0, module,                             \
                            ext[u]->ext, ext[u]->ext_size, (**(TYPE**)p));         \
    }

    for (u = 0; u < count; u++) {
        if (ext[u]->flags & LLLYEXT_OPT_INHERIT) {
            /* ignore the inherited extensions which were not explicitely instantiated in the module */
            continue;
        } else if (ext[u]->insubstmt != substmt || ext[u]->insubstmt_index != substmt_index) {
            /* do not print the other substatement than the required */
            continue;
        } else if (ext[u]->def->module == module->ctx->models.list[0] &&
                 (!strcmp(ext[u]->arg_value, "operation") ||
                  !strcmp(ext[u]->arg_value, "select") ||
                  !strcmp(ext[u]->arg_value, "type"))) {
            /* hack for NETCONF's edit-config's operation and filter's attributes
             * - the annotation definition is only internal, do not print it */
            continue;
        }

        mod = lllys_main_module(ext[u]->def->module);
        if (mod == lllys_main_module(module)) {
            prefix = module->prefix;
        } else {
            for (x = 0; x < module->imp_size; x++) {
                if (mod == module->imp[x].module) {
                    prefix = module->imp[x].prefix;
                    break;
                }
            }
        }

        /* extension - generic part */
        if (ext[u]->arg_value) {
            ext_name = malloc(strlen(prefix) + 1 + strlen(ext[u]->def->name) + 1);
            sprintf(ext_name, "%s:%s", prefix, ext[u]->def->name);
            yang_print_text(out, level, ext_name, ext[u]->arg_value, 1, 0);
            free(ext_name);
        } else {
            llly_print(out, "%*s%s:%s", LEVEL, INDENT, prefix, ext[u]->def->name);
        }

        /* extensions in extension instance */
        content = 0;
        if (ext[u]->ext_size) {
            yang_print_open(out, &content);
            yang_print_extension_instances(out, level + 1, module, LLLYEXT_SUBSTMT_SELF, 0,
                                           ext[u]->ext, ext[u]->ext_size);
        }

        /* extension - type-specific part */
        switch(ext[u]->ext_type) {
        case LLLYEXT_FLAG:
            /* flag extension - nothing special */
            break;
        case LLLYEXT_COMPLEX:
            info = ((struct lllys_ext_instance_complex*)ext[u])->substmt; /* shortcut */
            if (!info) {
                /* no content */
                break;
            }
            level++;
            for (i = 0; info[i].stmt; i++) {
                switch(info[i].stmt) {
                case LLLY_STMT_DESCRIPTION:
                case LLLY_STMT_REFERENCE:
                case LLLY_STMT_UNITS:
                case LLLY_STMT_DEFAULT:
                case LLLY_STMT_ERRTAG:
                case LLLY_STMT_ERRMSG:
                case LLLY_STMT_PREFIX:
                case LLLY_STMT_NAMESPACE:
                case LLLY_STMT_PRESENCE:
                case LLLY_STMT_REVISIONDATE:
                case LLLY_STMT_KEY:
                case LLLY_STMT_BASE:
                case LLLY_STMT_CONTACT:
                case LLLY_STMT_ORGANIZATION:
                case LLLY_STMT_PATH:
                    yang_print_extcomplex_str(out, level, module, (struct lllys_ext_instance_complex*)ext[u],
                                              info[i].stmt, &content);
                    break;
                case LLLY_STMT_ARGUMENT:
                    pp = lllys_ext_complex_get_substmt(LLLY_STMT_ARGUMENT, (struct lllys_ext_instance_complex*)ext[u], NULL);
                    if (!pp || !(*pp)) {
                        break;
                    }
                    yang_print_open(out, &content);
                    if (info->cardinality >= LLLY_STMT_CARD_SOME) {
                        /* we have array */
                        for (c = 0; ((const char***)pp)[0][c]; c++) {
                            content2 = 0;
                            llly_print(out, "%*sargument %s", LEVEL, INDENT, ((const char ***)pp)[0][c]);
                            j = -1;
                            while ((j = lllys_ext_iter(ext[u]->ext, ext[u]->ext_size, j + 1, LLLYEXT_SUBSTMT_ARGUMENT)) != -1) {
                                if (ext[u]->ext[j]->insubstmt_index != c) {
                                    continue;
                                }
                                yang_print_open(out, &content2);
                                yang_print_extension_instances(out, level + 1, module, LLLYEXT_SUBSTMT_ARGUMENT, c,
                                                               &ext[u]->ext[j], 1);
                            }

                            if (((uint8_t *)pp[1])[c] == 1) {
                                yang_print_open(out, &content2);
                                yang_print_substmt(out, level + 1, LLLYEXT_SUBSTMT_YINELEM, c,
                                                   (((uint8_t *)pp[1])[c] == 1) ? "true" : "false", module, ext[u]->ext, ext[u]->ext_size);
                            } else {
                                j = -1;
                                while ((j = lllys_ext_iter(ext[u]->ext, ext[u]->ext_size, j + 1, LLLYEXT_SUBSTMT_YINELEM)) != -1) {
                                    if (ext[u]->ext[j]->insubstmt_index == c) {
                                        yang_print_open(out, &content2);
                                        yang_print_substmt(out, level + 1, LLLYEXT_SUBSTMT_YINELEM, c, (((uint8_t *)pp[1])[c] == 1) ? "true" : "false",
                                                           module, ext[u]->ext + j, ext[u]->ext_size - j);
                                        break;
                                    }
                                }
                            }
                            yang_print_close(out, level, content2);
                        }
                    } else {
                        content2 = 0;
                        llly_print(out, "%*sargument %s", LEVEL, INDENT, (const char *)pp[0]);
                        j = -1;
                        while ((j = lllys_ext_iter(ext[u]->ext, ext[u]->ext_size, j + 1, LLLYEXT_SUBSTMT_ARGUMENT)) != -1) {
                            yang_print_open(out, &content2);
                            yang_print_extension_instances(out, level + 1, module, LLLYEXT_SUBSTMT_ARGUMENT, 0,
                                                           &ext[u]->ext[j], 1);
                        }
                        if (*(uint8_t*)(pp + 1) == 1 || lllys_ext_iter(ext[u]->ext, ext[u]->ext_size, 0, LLLYEXT_SUBSTMT_YINELEM) != -1) {
                            yang_print_open(out, &content2);
                            yang_print_substmt(out, level + 1, LLLYEXT_SUBSTMT_YINELEM, 0,
                                               (*(uint8_t*)(pp + 1) == 1) ? "true" : "false", module, ext[u]->ext, ext[u]->ext_size);
                        }
                        yang_print_close(out, level, content2);
                    }
                    break;
                case LLLY_STMT_BELONGSTO:
                    pp = lllys_ext_complex_get_substmt(LLLY_STMT_BELONGSTO, (struct lllys_ext_instance_complex*)ext[u], NULL);
                    if (!pp || !(*pp)) {
                        break;
                    }
                    if (info->cardinality >= LLLY_STMT_CARD_SOME) {
                        /* we have array */
                        for (c = 0; ((const char***)pp)[0][c]; c++) {
                            yang_print_open(out, &content);
                            llly_print(out, "%*sbelongs-to %s {\n", LEVEL, INDENT, ((const char ***)pp)[0][c]);
                            j = -1;
                            while ((j = lllys_ext_iter(ext[u]->ext, ext[u]->ext_size, j + 1, LLLYEXT_SUBSTMT_BELONGSTO)) != -1) {
                                yang_print_extension_instances(out, level + 1, module, LLLYEXT_SUBSTMT_BELONGSTO, c,
                                                               &ext[u]->ext[j], 1);
                            }
                            yang_print_substmt(out, level + 1, LLLYEXT_SUBSTMT_PREFIX, c, ((const char ***)pp)[1][c],
                                               module, ext[u]->ext, ext[u]->ext_size);
                            llly_print(out, "%*s}\n", LEVEL, INDENT);
                        }
                    } else {
                        yang_print_open(out, &content);
                        llly_print(out, "%*sbelongs-to %s {\n", LEVEL, INDENT, (const char *)pp[0]);
                        j = -1;
                        while ((j = lllys_ext_iter(ext[u]->ext, ext[u]->ext_size, j + 1, LLLYEXT_SUBSTMT_BELONGSTO)) != -1) {
                            yang_print_extension_instances(out, level + 1, module, LLLYEXT_SUBSTMT_BELONGSTO, 0,
                                                           &ext[u]->ext[j], 1);
                        }
                        yang_print_substmt(out, level + 1, LLLYEXT_SUBSTMT_PREFIX, 0, (const char *)pp[1],
                                           module, ext[u]->ext, ext[u]->ext_size);
                        llly_print(out, "%*s}\n", LEVEL, INDENT);
                    }
                    break;
                case LLLY_STMT_TYPE:
                    YANG_PRINT_EXTCOMPLEX_STRUCT_M(LLLY_STMT_TYPE, struct lllys_type, yang_print_type);
                    break;
                case LLLY_STMT_TYPEDEF:
                    YANG_PRINT_EXTCOMPLEX_STRUCT_M(LLLY_STMT_TYPEDEF, struct lllys_tpdf, yang_print_typedef);
                    break;
                case LLLY_STMT_IFFEATURE:
                    YANG_PRINT_EXTCOMPLEX_STRUCT_M(LLLY_STMT_IFFEATURE, struct lllys_iffeature, yang_print_iffeature);
                    break;
                case LLLY_STMT_STATUS:
                    flags = lllys_ext_complex_get_substmt(LLLY_STMT_STATUS, (struct lllys_ext_instance_complex *)ext[u], NULL);
                    if (!flags || !(*flags)) {
                        break;
                    }

                    if (*flags & LLLYS_STATUS_CURR) {
                        yang_print_open(out, &content);
                        str = "current";
                    } else if (*flags & LLLYS_STATUS_DEPRC) {
                        yang_print_open(out, &content);
                        str = "deprecated";
                    } else if (*flags & LLLYS_STATUS_OBSLT) {
                        yang_print_open(out, &content);
                        str = "obsolete";
                    } else {
                        /* no status flag */
                        break;
                    }
                    yang_print_substmt(out, level, LLLYEXT_SUBSTMT_STATUS, 0, str, module, ext[u]->ext, ext[u]->ext_size);
                    break;
                case LLLY_STMT_CONFIG:
                    yang_print_extcomplex_flags(out, level, module, (struct lllys_ext_instance_complex*)ext[u],
                                                LLLY_STMT_CONFIG, "true", "false",
                                                LLLYS_CONFIG_W | LLLYS_CONFIG_SET, LLLYS_CONFIG_R | LLLYS_CONFIG_SET, &content);
                    break;
                case LLLY_STMT_MANDATORY:
                    yang_print_extcomplex_flags(out, level, module, (struct lllys_ext_instance_complex*)ext[u],
                                                LLLY_STMT_MANDATORY, "false", "true", LLLYS_MAND_FALSE, LLLYS_MAND_TRUE,
                                                &content);
                    break;
                case LLLY_STMT_ORDEREDBY:
                    yang_print_extcomplex_flags(out, level, module, (struct lllys_ext_instance_complex*)ext[u],
                                                LLLY_STMT_ORDEREDBY, "system", "user", 0, LLLYS_USERORDERED, &content);
                    break;
                case LLLY_STMT_REQINSTANCE:
                    yang_print_extcomplex_bool(out, level, module, (struct lllys_ext_instance_complex*)ext[u],
                                               info[i].stmt, "true", "false", &content);
                    break;
                case LLLY_STMT_MODIFIER:
                    yang_print_extcomplex_bool(out, level, module, (struct lllys_ext_instance_complex*)ext[u],
                                               LLLY_STMT_MODIFIER, "invert-match", NULL, &content);
                    break;
                case LLLY_STMT_DIGITS:
                    p = &((struct lllys_ext_instance_complex*)ext[u])->content[info[i].offset];
                    if (!p) {
                        break;
                    }
                    if (info->cardinality >= LLLY_STMT_CARD_SOME && *(uint8_t**)p) { /* we have array */
                        for (c = 0; (*(uint8_t**)p)[c]; c++) {
                            yang_print_open(out, &content);
                            yang_print_unsigned(out, level, LLLYEXT_SUBSTMT_DIGITS, c, module,
                                                ext[u]->ext, ext[u]->ext_size, (*(uint8_t**)p)[c]);
                        }
                    } else if ((*(uint8_t*)p)) {
                        yang_print_open(out, &content);
                        yang_print_unsigned(out, level, LLLYEXT_SUBSTMT_DIGITS, 0, module,
                                            ext[u]->ext, ext[u]->ext_size, (*(uint8_t*)p));
                    }
                    break;
                case LLLY_STMT_MAX:
                    YANG_PRINT_EXTCOMPLEX_INT(LLLYEXT_SUBSTMT_MAX, uint32_t, unsigned);
                    break;
                case LLLY_STMT_MIN:
                    YANG_PRINT_EXTCOMPLEX_INT(LLLYEXT_SUBSTMT_MIN, uint32_t, unsigned);
                    break;
                case LLLY_STMT_POSITION:
                    YANG_PRINT_EXTCOMPLEX_INT(LLLYEXT_SUBSTMT_POSITION, uint32_t, unsigned);
                    break;
                case LLLY_STMT_VALUE:
                    YANG_PRINT_EXTCOMPLEX_INT(LLLYEXT_SUBSTMT_VALUE, int32_t, signed);
                    break;
                case LLLY_STMT_UNIQUE:
                    pp = lllys_ext_complex_get_substmt(LLLY_STMT_UNIQUE, (struct lllys_ext_instance_complex *)ext[u], NULL);
                    if (!pp || !(*pp)) {
                        break;
                    }
                    if (info[i].cardinality >= LLLY_STMT_CARD_SOME) { /* process array */
                        for (pp = *pp, c = 0; *pp; pp++, c++) {
                            yang_print_open(out, &content);
                            yang_print_unique(out, level, module, (struct lllys_unique*)(*pp));
                            /* unique's extensions */
                            j = -1; content2 = 0;
                            do {
                                j = lllys_ext_iter(ext[u]->ext, ext[u]->ext_size, j + 1, LLLYEXT_SUBSTMT_UNIQUE);
                            } while (j != -1 && ext[u]->ext[j]->insubstmt_index != c);
                            if (j != -1) {
                                yang_print_open(out, &content2);
                                do {
                                    yang_print_extension_instances(out, level + 1, module, LLLYEXT_SUBSTMT_UNIQUE, c,
                                                                   &ext[u]->ext[j], 1);
                                    do {
                                        j = lllys_ext_iter(ext[u]->ext, ext[u]->ext_size, j + 1, LLLYEXT_SUBSTMT_UNIQUE);
                                    } while (j != -1 && ext[u]->ext[j]->insubstmt_index != c);
                                } while (j != -1);
                            }
                            yang_print_close(out, level, content2);
                        }
                    } else { /* single item */
                        yang_print_open(out, &content);
                        yang_print_unique(out, level, module, (struct lllys_unique*)(*pp));
                        /* unique's extensions */
                        j = -1; content2 = 0;
                        while ((j = lllys_ext_iter(ext[u]->ext, ext[u]->ext_size, j + 1, LLLYEXT_SUBSTMT_UNIQUE)) != -1) {
                            yang_print_open(out, &content2);
                            yang_print_extension_instances(out, level + 1, module, LLLYEXT_SUBSTMT_UNIQUE, 0,
                                                           &ext[u]->ext[j], 1);
                        }
                        yang_print_close(out, level, content2);
                    }
                    break;
                case LLLY_STMT_MODULE:
                    YANG_PRINT_EXTCOMPLEX_STRUCT(LLLY_STMT_MODULE, struct lllys_module, yang_print_model_);
                    break;
                case LLLY_STMT_ACTION:
                case LLLY_STMT_ANYDATA:
                case LLLY_STMT_ANYXML:
                case LLLY_STMT_CASE:
                case LLLY_STMT_CHOICE:
                case LLLY_STMT_CONTAINER:
                case LLLY_STMT_GROUPING:
                case LLLY_STMT_INPUT:
                case LLLY_STMT_OUTPUT:
                case LLLY_STMT_LEAF:
                case LLLY_STMT_LEAFLIST:
                case LLLY_STMT_LIST:
                case LLLY_STMT_NOTIFICATION:
                case LLLY_STMT_USES:
                    YANG_PRINT_EXTCOMPLEX_SNODE(info[i].stmt);
                    break;
                case LLLY_STMT_LENGTH:
                    YANG_PRINT_EXTCOMPLEX_STRUCT_M(LLLY_STMT_LENGTH, struct lllys_restr, yang_print_restr,
                                                   "length", ((struct lllys_restr *)(*pp))->expr);
                    break;
                case LLLY_STMT_MUST:
                    YANG_PRINT_EXTCOMPLEX_STRUCT_M(LLLY_STMT_MUST, struct lllys_restr, yang_print_must);
                    break;
                case LLLY_STMT_PATTERN:
                    YANG_PRINT_EXTCOMPLEX_STRUCT_M(LLLY_STMT_PATTERN, struct lllys_restr, yang_print_restr,
                                                   "pattern", &((struct lllys_restr *)(*pp))->expr[1]);
                    break;
                case LLLY_STMT_RANGE:
                    YANG_PRINT_EXTCOMPLEX_STRUCT_M(LLLY_STMT_RANGE, struct lllys_restr, yang_print_restr,
                                                   "range", ((struct lllys_restr *)(*pp))->expr);
                    break;
                case LLLY_STMT_WHEN:
                    YANG_PRINT_EXTCOMPLEX_STRUCT_M(LLLY_STMT_WHEN, struct lllys_when, yang_print_when);
                    break;
                case LLLY_STMT_REVISION:
                    YANG_PRINT_EXTCOMPLEX_STRUCT_M(LLLY_STMT_REVISION, struct lllys_revision, yang_print_revision);
                    break;
                default:
                    /* TODO */
                    break;
                }
            }
            level--;
            break;
        }

        /* close extension */
        yang_print_close(out, level, content);
    }
#undef YANG_PRINT_EXTCOMPLEX_STRUCT
#undef YANG_PRINT_EXTCOMPLEX_STRUCT_M
#undef YANG_PRINT_EXTCOMPLEX_INT
}
