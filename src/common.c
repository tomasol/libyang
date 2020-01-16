/**
 * @file common.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief common libyang routines implementations
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

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"
#include "parser.h"
#include "xpath.h"
#include "context.h"

THREAD_LOCAL enum int_log_opts log_opt;
THREAD_LOCAL int8_t llly_errno_glob;

API LLLY_ERR *
llly_errno_glob_address(void)
{
    FUN_IN;

    return (LLLY_ERR *)&llly_errno_glob;
}

API LLLY_VECODE
llly_vecode(const struct llly_ctx *ctx)
{
    FUN_IN;

    struct llly_err_item *i;

    i = llly_err_first(ctx);
    if (i) {
        return i->prev->vecode;
    }

    return 0;
}

API const char *
llly_errmsg(const struct llly_ctx *ctx)
{
    FUN_IN;

    struct llly_err_item *i;

    i = llly_err_first(ctx);
    if (i) {
        return i->prev->msg;
    }

    return NULL;
}

API const char *
llly_errpath(const struct llly_ctx *ctx)
{
    FUN_IN;

    struct llly_err_item *i;

    i = llly_err_first(ctx);
    if (i) {
        return i->prev->path;
    }

    return NULL;
}

API const char *
llly_errapptag(const struct llly_ctx *ctx)
{
    FUN_IN;

    struct llly_err_item *i;

    i = llly_err_first(ctx);
    if (i) {
        return i->prev->apptag;
    }

    return NULL;
}

API struct llly_err_item *
llly_err_first(const struct llly_ctx *ctx)
{
    FUN_IN;

    if (!ctx) {
        return NULL;
    }

    return pthread_getspecific(ctx->errlist_key);
}

void
llly_err_free(void *ptr)
{
    struct llly_err_item *i, *next;

    /* clean the error list */
    for (i = (struct llly_err_item *)ptr; i; i = next) {
        next = i->next;
        free(i->msg);
        free(i->path);
        free(i->apptag);
        free(i);
    }
}

API void
llly_err_clean(struct llly_ctx *ctx, struct llly_err_item *eitem)
{
    FUN_IN;

    struct llly_err_item *i, *first;

    first = llly_err_first(ctx);
    if (first == eitem) {
        eitem = NULL;
    }
    if (eitem) {
        /* disconnect the error */
        for (i = first; i && (i->next != eitem); i = i->next);
        assert(i);
        i->next = NULL;
        first->prev = i;
        /* free this err and newer */
        llly_err_free(eitem);
        /* update errno */
        llly_errno = i->no;
    } else {
        /* free all err */
        llly_err_free(first);
        pthread_setspecific(ctx->errlist_key, NULL);
        /* also clean errno */
        llly_errno = LLLY_SUCCESS;
    }
}

#ifndef  __USE_GNU

char *
get_current_dir_name(void)
{
    char tmp[PATH_MAX];
    char *retval;

    if (getcwd(tmp, sizeof(tmp))) {
        retval = strdup(tmp);
        LLLY_CHECK_ERR_RETURN(!retval, LOGMEM(NULL), NULL);
        return retval;
    }
    return NULL;
}

#endif

const char *
strpbrk_backwards(const char *s, const char *accept, unsigned int s_len)
{
    const char *sc;

    for (; *s != '\0' && s_len; --s, --s_len) {
        for (sc = accept; *sc != '\0'; ++sc) {
            if (*s == *sc) {
                return s;
            }
        }
    }
    return s;
}

char *
strnchr(const char *s, int c, unsigned int len)
{
    for (; *s != (char)c; ++s, --len) {
        if ((*s == '\0') || (!len)) {
            return NULL;
        }
    }
    return (char *)s;
}

const char *
strnodetype(LLLYS_NODE type)
{
    switch (type) {
    case LLLYS_UNKNOWN:
        return NULL;
    case LLLYS_AUGMENT:
        return "augment";
    case LLLYS_CONTAINER:
        return "container";
    case LLLYS_CHOICE:
        return "choice";
    case LLLYS_LEAF:
        return "leaf";
    case LLLYS_LEAFLIST:
        return "leaf-list";
    case LLLYS_LIST:
        return "list";
    case LLLYS_ANYXML:
        return "anyxml";
    case LLLYS_GROUPING:
        return "grouping";
    case LLLYS_CASE:
        return "case";
    case LLLYS_INPUT:
        return "input";
    case LLLYS_OUTPUT:
        return "output";
    case LLLYS_NOTIF:
        return "notification";
    case LLLYS_RPC:
        return "rpc";
    case LLLYS_USES:
        return "uses";
    case LLLYS_ACTION:
        return "action";
    case LLLYS_ANYDATA:
        return "anydata";
    case LLLYS_EXT:
        return "extension instance";
    }

    return NULL;
}

const char *
transform_module_name2import_prefix(const struct lllys_module *module, const char *module_name)
{
    uint16_t i;

    if (!module_name) {
        return NULL;
    }

    if (!strcmp(lllys_main_module(module)->name, module_name)) {
        /* the same for module and submodule */
        return module->prefix;
    }

    for (i = 0; i < module->imp_size; ++i) {
        if (!strcmp(module->imp[i].module->name, module_name)) {
            return module->imp[i].prefix;
        }
    }

    return NULL;
}

static int
_transform_json2xml_subexp(const struct lllys_module *module, const char *expr, char **out, size_t *out_used, size_t *out_size, int schema, int inst_id, const char ***prefixes,
                    const char ***namespaces, uint32_t *ns_count)
{
    const char *cur_expr, *end, *prefix, *literal;
    char *name;
    size_t name_len;
    const struct lllys_module *mod = NULL, *prev_mod = NULL;
    uint32_t i, j;
    struct lllyxp_expr *exp;
    struct llly_ctx *ctx = module->ctx;
    enum int_log_opts prev_ilo;

    assert(module && expr && ((!prefixes && !namespaces && !ns_count) || (prefixes && namespaces && ns_count)));

    exp = lllyxp_parse_expr(ctx, expr);
    LLLY_CHECK_RETURN(!exp, 1);

    for (i = 0; i < exp->used; ++i) {
        cur_expr = &exp->expr[exp->expr_pos[i]];

        /* copy WS */
        if (i && ((end = exp->expr + exp->expr_pos[i - 1] + exp->tok_len[i - 1]) != cur_expr)) {
            strncpy(&(*out)[*out_used], end, cur_expr - end);
            (*out_used) += cur_expr - end;
        }

        if ((exp->tokens[i] == LLLYXP_TOKEN_NAMETEST) && ((end = strnchr(cur_expr, ':', exp->tok_len[i])) || inst_id)) {
            /* get the module */
            if (!schema) {
                if (end) {
                    name_len = end - cur_expr;
                    name = strndup(cur_expr, name_len);
                    mod = llly_ctx_get_module(module->ctx, name, NULL, 0);
                    if (module->ctx->data_clb) {
                        if (!mod) {
                            mod = module->ctx->data_clb(module->ctx, name, NULL, 0, module->ctx->data_clb_data);
                        } else if (!mod->implemented) {
                            mod = module->ctx->data_clb(module->ctx, name, mod->ns, LLLY_MODCLB_NOT_IMPLEMENTED, module->ctx->data_clb_data);
                        }
                    }
                    free(name);
                    if (!mod) {
                        LOGVAL(ctx, LLLYE_INMOD_LEN, LLLY_VLOG_NONE, NULL, name_len, cur_expr);
                        goto error;
                    }
                    prev_mod = mod;
                } else {
                    mod = prev_mod;
                    if (!mod) {
                        LOGINT(ctx);
                        goto error;
                    }
                    name_len = 0;
                    end = cur_expr;
                }
                prefix = mod->prefix;
            } else {
                if (end) {
                    name_len = end - cur_expr;
                } else {
                    name_len = strlen(cur_expr);
                    end = cur_expr;
                }
                name = strndup(cur_expr, name_len);
                prefix = transform_module_name2import_prefix(module, name);
                free(name);
                if (!prefix) {
                    LOGVAL(ctx, LLLYE_INMOD_LEN, LLLY_VLOG_NONE, NULL, name_len, cur_expr);
                    goto error;
                }
            }

            /* remember the namespace definition (only if it's new) */
            if (!schema && ns_count) {
                for (j = 0; j < *ns_count; ++j) {
                    if (llly_strequal((*namespaces)[j], mod->ns, 1)) {
                        break;
                    }
                }
                if (j == *ns_count) {
                    ++(*ns_count);
                    *prefixes = llly_realloc(*prefixes, *ns_count * sizeof **prefixes);
                    LLLY_CHECK_ERR_GOTO(!(*prefixes), LOGMEM(ctx), error);
                    *namespaces = llly_realloc(*namespaces, *ns_count * sizeof **namespaces);
                    LLLY_CHECK_ERR_GOTO(!(*namespaces), LOGMEM(ctx), error);
                    (*prefixes)[*ns_count - 1] = mod->prefix;
                    (*namespaces)[*ns_count - 1] = mod->ns;
                }
            }

            /* adjust out size (it can even decrease in some strange cases) */
            *out_size += strlen(prefix) + 1 - name_len;
            *out = llly_realloc(*out, *out_size);
            LLLY_CHECK_ERR_GOTO(!(*out), LOGMEM(ctx), error);

            /* copy the model name */
            strcpy(&(*out)[*out_used], prefix);
            *out_used += strlen(prefix);

            if (!name_len) {
                /* we are adding the prefix, so also ':' */
                (*out)[*out_used] = ':';
                ++(*out_used);
            }

            /* copy the rest */
            strncpy(&(*out)[*out_used], end, exp->tok_len[i] - name_len);
            *out_used += exp->tok_len[i] - name_len;
        } else if ((exp->tokens[i] == LLLYXP_TOKEN_LITERAL) && (end = strnchr(cur_expr, ':', exp->tok_len[i]))) {
            /* copy begin quote */
            (*out)[*out_used] = cur_expr[0];
            ++(*out_used);

            /* skip quotes */
            literal = lllydict_insert(module->ctx, cur_expr + 1, exp->tok_len[i] - 2);

            /* parse literals as subexpressions if possible, otherwise treat as a literal */
            llly_ilo_change(NULL, ILO_IGNORE, &prev_ilo, NULL);
            if (_transform_json2xml_subexp(module, literal, out, out_used, out_size, schema, inst_id, prefixes, namespaces, ns_count)) {
                strncpy(&(*out)[*out_used], literal, exp->tok_len[i] - 2);
                *out_used += exp->tok_len[i] - 2;
            }
            llly_ilo_restore(NULL, prev_ilo, NULL, 0);

            lllydict_remove(module->ctx, literal);

            /* copy end quote */
            (*out)[*out_used] = cur_expr[exp->tok_len[i] - 1];
            ++(*out_used);
        } else {
            strncpy(&(*out)[*out_used], &exp->expr[exp->expr_pos[i]], exp->tok_len[i]);
            *out_used += exp->tok_len[i];
        }
    }

    lllyxp_expr_free(exp);
    return 0;

error:
    if (!schema && ns_count) {
        free(*prefixes);
        free(*namespaces);
    }
    lllyxp_expr_free(exp);
    return 1;
}

static const char *
_transform_json2xml(const struct lllys_module *module, const char *expr, int schema, int inst_id, const char ***prefixes,
                    const char ***namespaces, uint32_t *ns_count)
{
    char *out;
    size_t out_size, out_used;
    int ret;

    assert(module && expr && ((!prefixes && !namespaces && !ns_count) || (prefixes && namespaces && ns_count)));

    if (ns_count) {
        *ns_count = 0;
        *prefixes = NULL;
        *namespaces = NULL;
    }

    if (!expr[0]) {
        /* empty value */
        return lllydict_insert(module->ctx, expr, 0);
    }

    out_size = strlen(expr) + 1;
    out = malloc(out_size);
    LLLY_CHECK_ERR_RETURN(!out, LOGMEM(module->ctx), NULL);
    out_used = 0;

    ret = _transform_json2xml_subexp(module, expr, &out, &out_used, &out_size, schema, inst_id, prefixes, namespaces, ns_count);
    if (!ret) {
        out[out_used] = '\0';
        return lllydict_insert_zc(module->ctx, out);
    }

    free(out);
    return NULL;
}

const char *
transform_json2xml(const struct lllys_module *module, const char *expr, int inst_id, const char ***prefixes,
                   const char ***namespaces, uint32_t *ns_count)
{
    return _transform_json2xml(module, expr, 0, inst_id, prefixes, namespaces, ns_count);
}

const char *
transform_json2schema(const struct lllys_module *module, const char *expr)
{
    return _transform_json2xml(module, expr, 1, 0, NULL, NULL, NULL);
}

static int
transform_xml2json_subexp(struct llly_ctx *ctx, const char *expr, char **out, size_t *out_used, size_t *out_size,
                          struct lllyxml_elem *xml, int inst_id, int use_ctx_data_clb)
{
    const char *end, *cur_expr, *literal;
    char *prefix;
    uint16_t i;
    enum int_log_opts prev_ilo;
    size_t pref_len;
    const struct lllys_module *mod, *prev_mod = NULL;
    const struct lllyxml_ns *ns;
    struct lllyxp_expr *exp;

    exp = lllyxp_parse_expr(ctx, expr);
    if (!exp) {
        return 1;
    }

    for (i = 0; i < exp->used; ++i) {
        cur_expr = &exp->expr[exp->expr_pos[i]];

        /* copy WS */
        if (i && ((end = exp->expr + exp->expr_pos[i - 1] + exp->tok_len[i - 1]) != cur_expr)) {
            strncpy(&(*out)[*out_used], end, cur_expr - end);
            (*out_used) += cur_expr - end;
        }

        if ((exp->tokens[i] == LLLYXP_TOKEN_NAMETEST) && (end = strnchr(cur_expr, ':', exp->tok_len[i]))) {
            /* get the module */
            pref_len = end - cur_expr;
            prefix = strndup(cur_expr, pref_len);
            if (!prefix) {
                LOGMEM(ctx);
                goto error;
            }
            ns = lllyxml_get_ns(xml, prefix);
            free(prefix);
            if (!ns) {
                LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_XML, xml, "namespace prefix");
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "XML namespace with prefix \"%.*s\" not defined.", pref_len, cur_expr);
                goto error;
            }
            mod = llly_ctx_get_module_by_ns(ctx, ns->value, NULL, 0);
            if (use_ctx_data_clb && ctx->data_clb) {
                if (!mod) {
                    mod = ctx->data_clb(ctx, NULL, ns->value, 0, ctx->data_clb_data);
                } else if (!mod->implemented) {
                    mod = ctx->data_clb(ctx, mod->name, mod->ns, LLLY_MODCLB_NOT_IMPLEMENTED, ctx->data_clb_data);
                }
            }
            if (!mod) {
                LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_XML, xml, "module namespace");
                LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "Module with the namespace \"%s\" could not be found.", ns->value);
                goto error;
            }

            if (!inst_id || (mod != prev_mod)) {
                /* adjust out size (it can even decrease in some strange cases) */
                *out_size += strlen(mod->name) - pref_len;
                *out = llly_realloc(*out, *out_size);
                if (!(*out)) {
                    LOGMEM(ctx);
                    goto error;
                }

                /* copy the model name */
                strcpy(&(*out)[*out_used], mod->name);
                *out_used += strlen(mod->name);
            } else {
                /* skip ':' */
                ++end;
                ++pref_len;
            }

            /* remember previous model name */
            prev_mod = mod;

            /* copy the rest */
            strncpy(&(*out)[*out_used], end, exp->tok_len[i] - pref_len);
            *out_used += exp->tok_len[i] - pref_len;
        } else if ((exp->tokens[i] == LLLYXP_TOKEN_NAMETEST) && inst_id) {
            LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_XML, xml, "namespace prefix");
            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_PREV, NULL, "Node name is missing module prefix.");
            goto error;
        } else if ((exp->tokens[i] == LLLYXP_TOKEN_LITERAL) && (end = strnchr(cur_expr, ':', exp->tok_len[i]))) {
            /* copy begin quote */
            (*out)[*out_used] = cur_expr[0];
            ++(*out_used);

            /* skip quotes */
            literal = lllydict_insert(ctx, cur_expr + 1, exp->tok_len[i] - 2);

            /* parse literals as subexpressions if possible, otherwise treat as a literal, do not log */
            prev_ilo = log_opt;
            log_opt = ILO_IGNORE;
            if (transform_xml2json_subexp(ctx, literal, out, out_used, out_size, xml, inst_id, use_ctx_data_clb)) {
                strncpy(&(*out)[*out_used], literal, exp->tok_len[i] - 2);
                *out_used += exp->tok_len[i] - 2;
            }
            log_opt = prev_ilo;

            lllydict_remove(ctx, literal);

            /* copy end quote */
            (*out)[*out_used] = cur_expr[exp->tok_len[i] - 1];
            ++(*out_used);
        } else {
            strncpy(&(*out)[*out_used], &exp->expr[exp->expr_pos[i]], exp->tok_len[i]);
            *out_used += exp->tok_len[i];
        }
    }

    lllyxp_expr_free(exp);
    return 0;

error:
    lllyxp_expr_free(exp);
    return 1;
}

const char *
transform_xml2json(struct llly_ctx *ctx, const char *expr, struct lllyxml_elem *xml, int inst_id, int use_ctx_data_clb)
{
    char *out;
    size_t out_size, out_used;
    int ret;

    out_size = strlen(expr) + 1;
    out = malloc(out_size);
    if (!out) {
        LOGMEM(ctx);
        return NULL;
    }
    out_used = 0;

    ret = transform_xml2json_subexp(ctx, expr, &out, &out_used, &out_size, xml, inst_id, use_ctx_data_clb);
    if (!ret) {
        out[out_used] = '\0';
        return lllydict_insert_zc(ctx, out);
    }

    free(out);
    return NULL;
}

API char *
llly_path_xml2json(struct llly_ctx *ctx, const char *xml_path, struct lllyxml_elem *xml)
{
    FUN_IN;

    const char *json_path;
    char *ret = NULL;

    if (!ctx || !xml_path || !xml) {
        LOGARG;
        return NULL;
    }

    json_path = transform_xml2json(ctx, xml_path, xml, 0, 1);
    if (json_path) {
        ret = strdup(json_path);
        lllydict_remove(ctx, json_path);
    }

    return ret;
}

const char *
transform_schema2json(const struct lllys_module *module, const char *expr)
{
    const char *end, *cur_expr, *ptr;
    char *out;
    uint16_t i;
    size_t out_size, out_used, pref_len;
    const struct lllys_module *mod;
    struct llly_ctx *ctx = module->ctx;
    struct lllyxp_expr *exp = NULL;

    out_size = strlen(expr) + 1;
    out = malloc(out_size);
    LLLY_CHECK_ERR_RETURN(!out, LOGMEM(ctx), NULL);
    out_used = 0;

    exp = lllyxp_parse_expr(ctx, expr);
    LLLY_CHECK_ERR_GOTO(!exp, , error);

    for (i = 0; i < exp->used; ++i) {
        cur_expr = &exp->expr[exp->expr_pos[i]];

        /* copy WS */
        if (i && ((end = exp->expr + exp->expr_pos[i - 1] + exp->tok_len[i - 1]) != cur_expr)) {
            strncpy(&out[out_used], end, cur_expr - end);
            out_used += cur_expr - end;
        }

        if ((exp->tokens[i] == LLLYXP_TOKEN_NAMETEST) && (end = strnchr(cur_expr, ':', exp->tok_len[i]))) {
            /* get the module */
            pref_len = end - cur_expr;
            mod = lllyp_get_module(module, cur_expr, pref_len, NULL, 0, 0);
            if (!mod) {
                LOGVAL(ctx, LLLYE_INMOD_LEN, LLLY_VLOG_NONE, NULL, pref_len, cur_expr);
                goto error;
            }

            /* adjust out size (it can even decrease in some strange cases) */
            out_size += strlen(mod->name) - pref_len;
            out = llly_realloc(out, out_size);
            LLLY_CHECK_ERR_GOTO(!out, LOGMEM(ctx), error);

            /* copy the model name */
            strcpy(&out[out_used], mod->name);
            out_used += strlen(mod->name);

            /* copy the rest */
            strncpy(&out[out_used], end, exp->tok_len[i] - pref_len);
            out_used += exp->tok_len[i] - pref_len;
        } else if ((exp->tokens[i] == LLLYXP_TOKEN_LITERAL) && (end = strnchr(cur_expr, ':', exp->tok_len[i]))) {
            ptr = end;
            while (isalnum(ptr[-1]) || (ptr[-1] == '_') || (ptr[-1] == '-') || (ptr[-1] == '.')) {
                --ptr;
            }

            /* get the module */
            pref_len = end - ptr;
            mod = lllyp_get_module(module, ptr, pref_len, NULL, 0, 0);
            if (mod) {
                /* adjust out size (it can even decrease in some strange cases) */
                out_size += strlen(mod->name) - pref_len;
                out = llly_realloc(out, out_size);
                LLLY_CHECK_ERR_GOTO(!out, LOGMEM(ctx), error);

                /* copy any beginning */
                strncpy(&out[out_used], cur_expr, ptr - cur_expr);
                out_used += ptr - cur_expr;

                /* copy the model name */
                strcpy(&out[out_used], mod->name);
                out_used += strlen(mod->name);

                /* copy the rest */
                strncpy(&out[out_used], end, (exp->tok_len[i] - pref_len) - (ptr - cur_expr));
                out_used += (exp->tok_len[i] - pref_len) - (ptr - cur_expr);
            } else {
                strncpy(&out[out_used], &exp->expr[exp->expr_pos[i]], exp->tok_len[i]);
                out_used += exp->tok_len[i];
            }
        } else {
            strncpy(&out[out_used], &exp->expr[exp->expr_pos[i]], exp->tok_len[i]);
            out_used += exp->tok_len[i];
        }
    }
    out[out_used] = '\0';

    lllyxp_expr_free(exp);
    return lllydict_insert_zc(module->ctx, out);

error:
    free(out);
    lllyxp_expr_free(exp);
    return NULL;
}

const char *
transform_iffeat_schema2json(const struct lllys_module *module, const char *expr)
{
    const char *in, *id;
    char *out, *col;
    size_t out_size, out_used, id_len, rc;
    const struct lllys_module *mod;
    struct llly_ctx *ctx = module->ctx;

    in = expr;
    out_size = strlen(in) + 1;
    out = malloc(out_size);
    LLLY_CHECK_ERR_RETURN(!out, LOGMEM(ctx), NULL);
    out_used = 0;

    while (1) {
        col = strchr(in, ':');
        /* we're finished, copy the remaining part */
        if (!col) {
            strcpy(&out[out_used], in);
            out_used += strlen(in) + 1;
            assert(out_size == out_used);
            return lllydict_insert_zc(ctx, out);
        }
        id = strpbrk_backwards(col - 1, " \f\n\r\t\v(", (col - in) - 1);
        if ((id[0] == ' ') || (id[0] == '\f') || (id[0] == '\n') || (id[0] == '\r') ||
            (id[0] == '\t') || (id[0] == '\v') || (id[0] == '(')) {
            ++id;
        }
        id_len = col - id;
        rc = parse_identifier(id);
        if (rc < id_len) {
            LOGVAL(ctx, LLLYE_INCHAR, LLLY_VLOG_NONE, NULL, id[rc], &id[rc]);
            free(out);
            return NULL;
        }

        /* get the module */
        mod = lllyp_get_module(module, id, id_len, NULL, 0, 0);
        if (!mod) {
            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_NONE, NULL, "Module prefix \"%.*s\" is unknown.", id_len, id);
            free(out);
            return NULL;
        }

        /* adjust out size (it can even decrease in some strange cases) */
        out_size += strlen(mod->name) - id_len;
        out = llly_realloc(out, out_size);
        LLLY_CHECK_ERR_RETURN(!out, LOGMEM(ctx), NULL);

        /* copy the data before prefix */
        strncpy(&out[out_used], in, id - in);
        out_used += id - in;

        /* copy the model name */
        strcpy(&out[out_used], mod->name);
        out_used += strlen(mod->name);

        /* copy ':' */
        out[out_used] = ':';
        ++out_used;

        /* finally adjust in pointer for next round */
        in = col + 1;
    }

    /* unreachable */
    LOGINT(ctx);
    return NULL;
}

static int
transform_json2xpath_subexpr(const struct lllys_module *cur_module, const struct lllys_module *prev_mod, struct lllyxp_expr *exp,
                             uint32_t *i, enum lllyxp_token end_token, char **out, size_t *out_used, size_t *out_size)
{
    const char *cur_expr, *end, *ptr;
    size_t name_len;
    char *name;
    const struct lllys_module *mod;
    struct llly_ctx *ctx = cur_module->ctx;

    while (*i < exp->used) {
        if (exp->tokens[*i] == end_token) {
            return 0;
        }

        cur_expr = &exp->expr[exp->expr_pos[*i]];

        /* copy WS */
        if (*i && ((end = exp->expr + exp->expr_pos[*i - 1] + exp->tok_len[*i - 1]) != cur_expr)) {
            strncpy(*out + *out_used, end, cur_expr - end);
            *out_used += cur_expr - end;
        }

        if (exp->tokens[*i] == LLLYXP_TOKEN_BRACK1) {
            /* copy "[" */
            strncpy(*out + *out_used, &exp->expr[exp->expr_pos[*i]], exp->tok_len[*i]);
            *out_used += exp->tok_len[*i];
            ++(*i);

            /* call recursively because we need to remember current prev_mod for after the predicate */
            if (transform_json2xpath_subexpr(cur_module, prev_mod, exp, i, LLLYXP_TOKEN_BRACK2, out, out_used, out_size)) {
                return -1;
            }

            if (*i >= exp->used) {
                LOGVAL(ctx, LLLYE_XPATH_EOF, LLLY_VLOG_NONE, NULL);
                return -1;
            }

            /* copy "]" */
            strncpy(*out + *out_used, &exp->expr[exp->expr_pos[*i]], exp->tok_len[*i]);
            *out_used += exp->tok_len[*i];
        } else if (exp->tokens[*i] == LLLYXP_TOKEN_NAMETEST) {
            if ((end = strnchr(cur_expr, ':', exp->tok_len[*i]))) {
                /* there is a prefix, get the module */
                name_len = end - cur_expr;
                name = strndup(cur_expr, name_len);
                prev_mod = llly_ctx_get_module(ctx, name, NULL, 1);
                free(name);
                if (!prev_mod) {
                    LOGVAL(ctx, LLLYE_INMOD_LEN, LLLY_VLOG_NONE, NULL, name_len ? name_len : exp->tok_len[*i], cur_expr);
                    return -1;
                }
                /* skip ":" */
                ++end;
                ++name_len;
            } else {
                end = cur_expr;
                name_len = 0;
            }

            /* do we print the module name? (always for "*" if there was any, it's an exception) */
            if (((prev_mod != cur_module) && (end[0] != '*')) || (name_len && (end[0] == '*'))) {
                /* adjust out size (it can even decrease in some strange cases) */
                *out_size += (strlen(prev_mod->name) - name_len) + 1;
                *out = llly_realloc(*out, *out_size);
                LLLY_CHECK_ERR_RETURN(!*out, LOGMEM(ctx), -1);

                /* copy the model name */
                strcpy(*out + *out_used, prev_mod->name);
                *out_used += strlen(prev_mod->name);

                /* print ":" */
                (*out)[*out_used] = ':';
                ++(*out_used);
            }

            /* copy the rest */
            strncpy(*out + *out_used, end, exp->tok_len[*i] - name_len);
            *out_used += exp->tok_len[*i] - name_len;
        } else if ((exp->tokens[*i] == LLLYXP_TOKEN_LITERAL) && (end = strnchr(cur_expr, ':', exp->tok_len[*i]))) {
            ptr = end;
            while (isalnum(ptr[-1]) || (ptr[-1] == '_') || (ptr[-1] == '-') || (ptr[-1] == '.')) {
                --ptr;
            }

            /* get the module, but it may actually not be a module name */
            name_len = end - ptr;
            name = strndup(ptr, name_len);
            mod = llly_ctx_get_module(ctx, name, NULL, 1);
            free(name);

            if (mod && (mod != cur_module)) {
                /* adjust out size (it can even decrease in some strange cases) */
                *out_size += strlen(mod->name) - name_len;
                *out = llly_realloc(*out, *out_size);
                LLLY_CHECK_ERR_RETURN(!*out, LOGMEM(ctx), -1);

                /* copy any beginning */
                strncpy(*out + *out_used, cur_expr, ptr - cur_expr);
                *out_used += ptr - cur_expr;

                /* copy the model name */
                strcpy(*out + *out_used, mod->name);
                *out_used += strlen(mod->name);

                /* copy the rest */
                strncpy(*out + *out_used, end, (exp->tok_len[*i] - name_len) - (ptr - cur_expr));
                *out_used += (exp->tok_len[*i] - name_len) - (ptr - cur_expr);
            } else {
                strncpy(*out + *out_used, &exp->expr[exp->expr_pos[*i]], exp->tok_len[*i]);
                *out_used += exp->tok_len[*i];
            }
        } else {
            strncpy(*out + *out_used, &exp->expr[exp->expr_pos[*i]], exp->tok_len[*i]);
            *out_used += exp->tok_len[*i];
        }

        ++(*i);
    }

    return 0;
}

char *
transform_json2xpath(const struct lllys_module *cur_module, const char *expr)
{
    char *out;
    size_t out_size, out_used;
    uint32_t i;
    struct lllyxp_expr *exp;

    assert(cur_module && expr);

    out_size = strlen(expr) + 1;
    out = malloc(out_size);
    LLLY_CHECK_ERR_RETURN(!out, LOGMEM(cur_module->ctx), NULL);
    out_used = 0;

    exp = lllyxp_parse_expr(cur_module->ctx, expr);
    LLLY_CHECK_ERR_RETURN(!exp, free(out), NULL);

    i = 0;
    if (transform_json2xpath_subexpr(cur_module, cur_module, exp, &i, LLLYXP_TOKEN_NONE, &out, &out_used, &out_size)) {
        goto error;
    }
    out[out_used] = '\0';

    lllyxp_expr_free(exp);
    return out;

error:
    free(out);
    lllyxp_expr_free(exp);
    return NULL;
}

static int
llly_path_data2schema_copy_token(const struct llly_ctx *ctx, struct lllyxp_expr *exp, uint16_t cur_exp, char **out, uint16_t *out_used)
{
    uint16_t len;

    for (len = exp->tok_len[cur_exp]; isspace(exp->expr[exp->expr_pos[cur_exp] + len]); ++len);
    *out = llly_realloc(*out, *out_used + len);
    LLLY_CHECK_ERR_RETURN(!(*out), LOGMEM(ctx), -1);
    sprintf(*out + *out_used - 1, "%.*s", len, exp->expr + exp->expr_pos[cur_exp]);
    *out_used += len;

    return 0;
}

static int
llly_path_data2schema_subexp(const struct llly_ctx *ctx, const struct lllys_node *orig_parent, const struct lllys_module *cur_mod,
                           struct lllyxp_expr *exp, uint16_t *cur_exp, char **out, uint16_t *out_used)
{
    uint16_t j, k, len, slash;
    char *str = NULL, *col;
    const struct lllys_node *node, *node2, *parent;
    enum lllyxp_token end_token = 0;
    int first, path_lost;

    switch (exp->tokens[*cur_exp]) {
    case LLLYXP_TOKEN_BRACK1:
        end_token = LLLYXP_TOKEN_BRACK2;

        if (llly_path_data2schema_copy_token(ctx, exp, *cur_exp, out, out_used)) {
            goto error;
        }
        ++(*cur_exp);
        first = 0;
        break;
    case LLLYXP_TOKEN_PAR1:
        end_token = LLLYXP_TOKEN_PAR2;

        if (llly_path_data2schema_copy_token(ctx, exp, *cur_exp, out, out_used)) {
            goto error;
        }
        ++(*cur_exp);
        first = 0;
        break;
    case LLLYXP_TOKEN_OPERATOR_PATH:
        first = (orig_parent) ? 0 : 1;
        break;
    default:
        first = 1;
        break;
    }

    path_lost = 0;
    parent = orig_parent;
    while (*cur_exp < exp->used) {
        switch (exp->tokens[*cur_exp]) {
        case LLLYXP_TOKEN_DOT:
        case LLLYXP_TOKEN_DDOT:
        case LLLYXP_TOKEN_NAMETEST:
            if (path_lost) {
                /* we do not know anything anymore, just copy it */
                if (llly_path_data2schema_copy_token(ctx, exp, *cur_exp, out, out_used)) {
                    goto error;
                }
                break;
            }

            str = strndup(exp->expr + exp->expr_pos[*cur_exp], exp->tok_len[*cur_exp]);
            LLLY_CHECK_ERR_GOTO(!str, LOGMEM(ctx), error);

            col = strchr(str, ':');
            if (col) {
                *col = '\0';
                ++col;
            }

            /* first node */
            if (first) {
                if (!col) {
                    LOGVAL(ctx, LLLYE_PATH_MISSMOD, LLLY_VLOG_NONE, NULL);
                    goto error;
                }

                cur_mod = llly_ctx_get_module(ctx, str, NULL, 0);
                if (!cur_mod) {
                    LOGVAL(ctx, LLLYE_PATH_INMOD, LLLY_VLOG_STR, str);
                    goto error;
                }

                first = 0;
            }

            if (((col ? col[0] : str[0]) == '.') || ((col ? col[0] : str[0]) == '*')) {
                free(str);
                str = NULL;

                if (end_token) {
                    LOGERR(ctx, LLLY_EINVAL, "Invalid path used (%s in a subexpression).", str);
                    goto error;
                }

                /* we can no longer evaluate the path, so just copy the rest */
                path_lost = 1;
                if (llly_path_data2schema_copy_token(ctx, exp, *cur_exp, out, out_used)) {
                    goto error;
                }
                break;
            }

            /* create schema path for this data node */
            node = NULL;
            while ((node = lllys_getnext(node, parent, cur_mod, LLLYS_GETNEXT_NOSTATECHECK))) {
                if (strcmp(node->name, col ? col : str)) {
                    continue;
                }

                if (col && strcmp(lllys_node_module(node)->name, str)) {
                    continue;
                }
                if (!col && (lllys_node_module(node) != lllys_node_module(parent))) {
                    continue;
                }

                /* determine how deep the node actually is, we must generate the path from the highest parent */
                j = 0;
                node2 = node;
                while (node2 != parent) {
                    node2 = lllys_parent(node2);
                    if (!node2 || (node2->nodetype != LLLYS_USES)) {
                        ++j;
                    }
                }

                /* first node, do not print '/' */
                slash = 0;
                while (j) {
                    k = j - 1;
                    node2 = node;
                    while (k) {
                        node2 = lllys_parent(node2);
                        assert(node2);
                        if (node2->nodetype != LLLYS_USES) {
                            --k;
                        }
                    }

                    if ((lllys_node_module(node2) != cur_mod) || !parent) {
                        /* module name and node name */
                        len = slash + strlen(lllys_node_module(node2)->name) + 1 + strlen(node2->name);
                        *out = llly_realloc(*out, *out_used + len);
                        LLLY_CHECK_ERR_GOTO(!(*out), LOGMEM(ctx), error);
                        sprintf(*out + *out_used - 1, "%s%s:%s", slash ? "/" : "", lllys_node_module(node2)->name, node2->name);
                        *out_used += len;
                    } else {
                        /* only node name */
                        len = slash + strlen(node2->name);
                        *out = llly_realloc(*out, *out_used + len);
                        LLLY_CHECK_ERR_GOTO(!(*out), LOGMEM(ctx), error);
                        sprintf(*out + *out_used - 1, "%s%s", slash ? "/" : "", node2->name);
                        *out_used += len;
                    }

                    slash = 1;
                    --j;
                }

                break;
            }
            if (!node) {
                LOGVAL(ctx, LLLYE_PATH_INNODE, LLLY_VLOG_STR, col ? col : str);
                goto error;
            }

            /* copy any whitespaces */
            for (len = 0; isspace(exp->expr[exp->expr_pos[*cur_exp] + exp->tok_len[*cur_exp] + len]); ++len);
            if (len) {
                *out = llly_realloc(*out, *out_used + len);
                LLLY_CHECK_ERR_GOTO(!(*out), LOGMEM(ctx), error);
                sprintf(*out + *out_used - 1, "%*s", len, " ");
                *out_used += len;
            }

            /* next iteration */
            free(str);
            str = NULL;
            parent = node;
            break;
        case LLLYXP_TOKEN_COMMA:
        case LLLYXP_TOKEN_OPERATOR_LOG:
        case LLLYXP_TOKEN_OPERATOR_COMP:
        case LLLYXP_TOKEN_OPERATOR_MATH:
        case LLLYXP_TOKEN_OPERATOR_UNI:
            /* reset the processing */
            first = 1;
            path_lost = 0;
            parent = orig_parent;

            /* fallthrough */
        case LLLYXP_TOKEN_OPERATOR_PATH:
            if ((exp->tokens[*cur_exp] == LLLYXP_TOKEN_OPERATOR_PATH) && (exp->tok_len[*cur_exp] == 2)) {
                /* we can no longer evaluate the path further */
                path_lost = 1;
            }
            /* fallthrough */
        case LLLYXP_TOKEN_NODETYPE:
        case LLLYXP_TOKEN_FUNCNAME:
        case LLLYXP_TOKEN_LITERAL:
        case LLLYXP_TOKEN_NUMBER:
            /* just copy it */
            if (llly_path_data2schema_copy_token(ctx, exp, *cur_exp, out, out_used)) {
                goto error;
            }
            break;
        case LLLYXP_TOKEN_BRACK1:
        case LLLYXP_TOKEN_PAR1:
            if (llly_path_data2schema_subexp(ctx, parent, cur_mod, exp, cur_exp, out, out_used)) {
                goto error;
            }
            break;
        default:
            if (end_token && (exp->tokens[*cur_exp] == end_token)) {
                /* we are done (with this subexpression) */
                if (llly_path_data2schema_copy_token(ctx, exp, *cur_exp, out, out_used)) {
                    goto error;
                }

                return 0;
            }
            LOGERR(ctx, LLLY_EINVAL, "Invalid token used (%.*s).", exp->tok_len[*cur_exp], exp->expr + exp->expr_pos[*cur_exp]);
            goto error;
        }

        ++(*cur_exp);
    }

    if (end_token) {
        LOGVAL(ctx, LLLYE_XPATH_EOF, LLLY_VLOG_NONE, NULL);
        return -1;
    }

    return 0;

error:
    free(str);
    return -1;
}

API char *
llly_path_data2schema(struct llly_ctx *ctx, const char *data_path)
{
    FUN_IN;

    struct lllyxp_expr *exp;
    uint16_t out_used, cur_exp = 0;
    char *out;
    int r, mod_name_len, nam_len, is_relative = -1;
    const char *mod_name, *name;
    const struct lllys_module *mod = NULL;
    const struct lllys_node *parent = NULL;
    char *str;

    if (!ctx || !data_path) {
        LOGARG;
        return NULL;
    }

    if ((r = parse_schema_nodeid(data_path, &mod_name, &mod_name_len, &name, &nam_len, &is_relative, NULL, NULL, 1)) < 1) {
        LOGVAL(ctx, LLLYE_PATH_INCHAR, LLLY_VLOG_NONE, NULL, data_path[-r], &data_path[-r]);
        return NULL;
    }

    if (name[0] == '#') {
        if (is_relative) {
            LOGVAL(ctx, LLLYE_PATH_INCHAR, LLLY_VLOG_NONE, NULL, '#', name);
            return NULL;
        }

        ++name;
        --nam_len;

        if (!mod_name) {
            str = strndup(data_path, (name + nam_len) - data_path);
            LOGVAL(ctx, LLLYE_PATH_MISSMOD, LLLY_VLOG_STR, str);
            free(str);
            return NULL;
        }

        str = strndup(mod_name, mod_name_len);
        if (!str) {
            LOGMEM(ctx);
            return NULL;
        }
        mod = llly_ctx_get_module(ctx, str, NULL, 1);
        free(str);
        if (!mod) {
            str = strndup(data_path, (mod_name + mod_name_len) - data_path);
            LOGVAL(ctx, LLLYE_PATH_INMOD, LLLY_VLOG_STR, str);
            free(str);
            return NULL;
        }

        parent = lllyp_get_yang_data_template(mod, name, nam_len);
        if (!parent) {
            str = strndup(data_path, (name + nam_len) - data_path);
            LOGVAL(ctx, LLLYE_PATH_INNODE, LLLY_VLOG_STR, str);
            free(str);
            return NULL;
        }

        out_used = (name + nam_len) - data_path + 1;
        out = malloc(out_used);
        LLLY_CHECK_ERR_RETURN(!out, LOGMEM(ctx), NULL);
        memcpy(out, data_path, out_used -1);
        data_path += r;
    } else {
        out_used = 1;
        out = malloc(1);
        LLLY_CHECK_ERR_RETURN(!out, LOGMEM(ctx), NULL);
    }

    exp = lllyxp_parse_expr(ctx, data_path);
    if (!exp) {
        free(out);
        return NULL;
    }

    if (parent) {
        if (llly_path_data2schema_subexp(ctx, parent, mod, exp, &cur_exp, &out, &out_used)) {
            free(out);
            out = NULL;
        }
    } else {
        if (llly_path_data2schema_subexp(ctx, NULL, NULL, exp, &cur_exp, &out, &out_used)) {
            free(out);
            out = NULL;
        }
    }

    lllyxp_expr_free(exp);
    return out;
}

int
llly_new_node_validity(const struct lllys_node *schema)
{
    int validity;

    validity = LLLYD_VAL_OK;

    if (schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST)) {
        if (((struct lllys_node_leaf *)schema)->type.base == LLLY_TYPE_LEAFREF) {
            /* leafref target validation */
            validity |= LLLYD_VAL_LEAFREF;
        }
    }
    if (schema->nodetype & (LLLYS_LEAFLIST | LLLYS_LIST)) {
        /* duplicit instance check */
        validity |= LLLYD_VAL_DUP;
    }
    if ((schema->nodetype == LLLYS_LIST) && ((struct lllys_node_list *)schema)->unique_size) {
        /* unique check */
        validity |= LLLYD_VAL_UNIQUE;
    }
    if (schema->nodetype & (LLLYS_LEAF | LLLYS_LEAFLIST | LLLYS_LIST | LLLYS_CONTAINER | LLLYS_NOTIF | LLLYS_RPC | LLLYS_ACTION | LLLYS_ANYDATA)) {
        /* mandatory children check */
        validity |= LLLYD_VAL_MAND;
    }

    return validity;
}

void *
llly_realloc(void *ptr, size_t size)
{
    void *new_mem;

    new_mem = realloc(ptr, size);
    if (!new_mem) {
        free(ptr);
    }

    return new_mem;
}

int
llly_strequal_(const char *s1, const char *s2)
{
    if (s1 == s2) {
        return 1;
    } else if (!s1 || !s2) {
        return 0;
    } else {
        for ( ; *s1 == *s2; s1++, s2++) {
            if (*s1 == '\0') {
                return 1;
            }
        }
        return 0;
    }
}

int64_t
dec_pow(uint8_t exp)
{
    int64_t ret = 1;
    uint8_t i;

    for (i = 0; i < exp; ++i) {
        ret *= 10;
    }

    return ret;
}

int
dec64cmp(int64_t num1, uint8_t dig1, int64_t num2, uint8_t dig2)
{
    if (dig1 < dig2) {
        num2 /= dec_pow(dig2 - dig1);
    } else if (dig1 > dig2) {
        num1 /= dec_pow(dig1 - dig2);
    }

    if (num1 == num2) {
        return 0;
    }
    return (num1 > num2 ? 1 : -1);
}

LLLYB_HASH
lllyb_hash(struct lllys_node *sibling, uint8_t collision_id)
{
    struct lllys_module *mod;
    int ext_len;
    uint32_t full_hash;
    LLLYB_HASH hash;

#ifdef LLLY_ENABLED_CACHE
    if ((collision_id < LLLYS_NODE_HASH_COUNT) && sibling->hash[collision_id]) {
        return sibling->hash[collision_id];
    }
#endif

    mod = lllys_node_module(sibling);

    full_hash = dict_hash_multi(0, mod->name, strlen(mod->name));
    full_hash = dict_hash_multi(full_hash, sibling->name, strlen(sibling->name));
    if (collision_id) {
        if (collision_id > strlen(mod->name)) {
            /* fine, we will not hash more bytes, just use more bits from the hash than previously */
            ext_len = strlen(mod->name);
        } else {
            /* use one more byte from the module name than before */
            ext_len = collision_id;
        }
        full_hash = dict_hash_multi(full_hash, mod->name, ext_len);
    }
    full_hash = dict_hash_multi(full_hash, NULL, 0);

    /* use the shortened hash */
    hash = full_hash & (LLLYB_HASH_MASK >> collision_id);
    /* add colision identificator */
    hash |= LLLYB_HASH_COLLISION_ID >> collision_id;

    /* save this hash */
#ifdef LLLY_ENABLED_CACHE
    if (collision_id < LLLYS_NODE_HASH_COUNT) {
        sibling->hash[collision_id] = hash;
    }
#endif

    return hash;
}

int
lllyb_has_schema_model(struct lllys_node *sibling, const struct lllys_module **models, int mod_count)
{
    int i;
    const struct lllys_module *mod = lllys_node_module(sibling);

    for (i = 0; i < mod_count; ++i) {
        if (mod == models[i]) {
            return 1;
        }
    }

    return 0;
}

/**
 * @brief Static table of the UTF8 characters lengths according to their first byte.
 */
static const unsigned char
utf8_char_length_table[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 1, 1
};

/**
 * @brief Use of utf8_char_length_table.
 */
#define UTF8LEN(x) utf8_char_length_table[((unsigned char)(x))]

size_t
llly_strlen_utf8(const char *str)
{
    size_t clen, len;
    const char *ptr;

    for (len = 0, clen = strlen(str), ptr = str; *ptr && len < clen; ++len, ptr += UTF8LEN(*ptr));
    return len;
}
