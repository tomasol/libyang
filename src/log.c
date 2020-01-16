/**
 * @file log.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief libyang logger implementation
 *
 * Copyright (c) 2015 - 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE
#define _BSD_SOURCE
#define _DEFAULT_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <assert.h>

#include "common.h"
#include "parser.h"
#include "context.h"
#include "tree_internal.h"

volatile uint8_t llly_log_level = LLLY_LLWRN;
volatile uint8_t llly_log_opts = LLLY_LOLOG | LLLY_LOSTORE_LAST;
static void (*llly_log_clb)(LLLY_LOG_LEVEL level, const char *msg, const char *path);
static volatile int path_flag = 1;
#ifndef NDEBUG
volatile int llly_log_dbg_groups = 0;
#endif

API LLLY_LOG_LEVEL
llly_verb(LLLY_LOG_LEVEL level)
{
    LLLY_LOG_LEVEL prev = llly_log_level;

    llly_log_level = level;
    return prev;
}

API int
llly_log_options(int opts)
{
    uint8_t prev = llly_log_opts;

    llly_log_opts = opts;
    return prev;
}

API void
llly_verb_dbg(int dbg_groups)
{
#ifndef NDEBUG
    llly_log_dbg_groups = dbg_groups;
#else
    (void)dbg_groups;
#endif
}

API void
llly_set_log_clb(void (*clb)(LLLY_LOG_LEVEL level, const char *msg, const char *path), int path)
{
    llly_log_clb = clb;
    path_flag = path;
}

API void
(*llly_get_log_clb(void))(LLLY_LOG_LEVEL, const char *, const char *)
{
    return llly_log_clb;
}

/* !! spends all string parameters !! */
static int
log_store(const struct llly_ctx *ctx, LLLY_LOG_LEVEL level, LLLY_ERR no, LLLY_VECODE vecode, char *msg, char *path, char *apptag)
{
    struct llly_err_item *eitem, *last;

    assert(ctx && (level < LLLY_LLVRB));

    eitem = pthread_getspecific(ctx->errlist_key);
    if (!eitem) {
        /* if we are only to fill in path, there must have been an error stored */
        assert(msg);
        eitem = malloc(sizeof *eitem);
        if (!eitem) {
            goto mem_fail;
        }
        eitem->prev = eitem;
        eitem->next = NULL;

        pthread_setspecific(ctx->errlist_key, eitem);
    } else if (!msg) {
        /* only filling the path */
        assert(path);

        /* find last error */
        eitem = eitem->prev;
        do {
            if (eitem->level == LLLY_LLERR) {
                /* fill the path */
                free(eitem->path);
                eitem->path = path;
                return 0;
            }
            eitem = eitem->prev;
        } while (eitem->prev->next);
        /* last error was not found */
        assert(0);
    } else if ((log_opt != ILO_STORE) && ((llly_log_opts & LLLY_LOSTORE_LAST) == LLLY_LOSTORE_LAST)) {
        /* overwrite last message */
        free(eitem->msg);
        free(eitem->path);
        free(eitem->apptag);
    } else {
        /* store new message */
        last = eitem->prev;
        eitem->prev = malloc(sizeof *eitem);
        if (!eitem->prev) {
            goto mem_fail;
        }
        eitem = eitem->prev;
        eitem->prev = last;
        eitem->next = NULL;
        last->next = eitem;
    }

    /* fill in the information */
    eitem->level = level;
    eitem->no = no;
    eitem->vecode = vecode;
    eitem->msg = msg;
    eitem->path = path;
    eitem->apptag = apptag;
    return 0;

mem_fail:
    LOGMEM(NULL);
    free(msg);
    free(path);
    free(apptag);
    return -1;
}

/* !! spends path !! */
static void
log_vprintf(const struct llly_ctx *ctx, LLLY_LOG_LEVEL level, LLLY_ERR no, LLLY_VECODE vecode, char *path,
            const char *format, va_list args)
{
    char *msg = NULL;
    int free_strs;

    if ((log_opt == ILO_ERR2WRN) && (level == LLLY_LLERR)) {
        /* change error to warning */
        level = LLLY_LLWRN;
    }

    if ((log_opt == ILO_IGNORE) || (level > llly_log_level)) {
        /* do not print or store the message */
        free(path);
        return;
    }

    /* set global errno on normal logging, but do not erase */
    if ((log_opt != ILO_STORE) && no) {
        llly_errno = no;
    }

    if ((no == LLLY_EVALID) && (vecode == LLLYVE_SUCCESS)) {
        /* assume we are inheriting the error, so inherit vecode as well */
        vecode = llly_vecode(ctx);
    }

    /* store the error/warning (if we need to store errors internally, it does not matter what are the user log options) */
    if ((level < LLLY_LLVRB) && ctx && ((llly_log_opts & LLLY_LOSTORE) || (log_opt == ILO_STORE))) {
        if (!format) {
            assert(path);
            /* postponed print of path related to the previous error, do not rewrite stored original message */
            if (log_store(ctx, level, no, vecode, NULL, path, NULL)) {
                return;
            }
            msg = "Path is related to the previous error message.";
        } else {
            if (vasprintf(&msg, format, args) == -1) {
                LOGMEM(ctx);
                free(path);
                return;
            }
            if (log_store(ctx, level, no, vecode, msg, path, NULL)) {
                return;
            }
        }
        free_strs = 0;
    } else {
        if (vasprintf(&msg, format, args) == -1) {
            LOGMEM(ctx);
            free(path);
            return;
        }
        free_strs = 1;
    }

    /* if we are only storing errors internally, never print the message (yet) */
    if ((llly_log_opts & LLLY_LOLOG) && (log_opt != ILO_STORE)) {
        if (llly_log_clb) {
            llly_log_clb(level, msg, path);
        } else {
            fprintf(stderr, "libyang[%d]: %s%s", level, msg, path ? " " : "\n");
            if (path) {
                fprintf(stderr, "(path: %s)\n", path);
            }
        }
    }

    if (free_strs) {
        free(path);
        free(msg);
    }
}

void
llly_log(const struct llly_ctx *ctx, LLLY_LOG_LEVEL level, LLLY_ERR no, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    log_vprintf(ctx, level, no, 0, NULL, format, ap);
    va_end(ap);
}

#ifndef NDEBUG

void
llly_log_dbg(int group, const char *format, ...)
{
    char *dbg_format;
    const char *str_group;
    va_list ap;

    if (!(llly_log_dbg_groups & group)) {
        return;
    }

    switch (group) {
    case LLLY_LDGDICT:
        str_group = "DICT";
        break;
    case LLLY_LDGYANG:
        str_group = "YANG";
        break;
    case LLLY_LDGYIN:
        str_group = "YIN";
        break;
    case LLLY_LDGXPATH:
        str_group = "XPATH";
        break;
    case LLLY_LDGDIFF:
        str_group = "DIFF";
        break;
    case LLLY_LDGAPI:
        str_group = "API";
        break;
    case LLLY_LDGHASH:
        str_group = "HASH";
        break;
    default:
        LOGINT(NULL);
        return;
    }

    if (asprintf(&dbg_format, "%s: %s", str_group, format) == -1) {
        LOGMEM(NULL);
        return;
    }

    va_start(ap, format);
    log_vprintf(NULL, LLLY_LLDBG, 0, 0, NULL, dbg_format, ap);
    va_end(ap);
    free(dbg_format);
}

#endif

API void
lllyext_log(const struct llly_ctx *ctx, LLLY_LOG_LEVEL level, const char *plugin, const char *function, const char *format, ...)
{
    va_list ap;
    char *plugin_msg;
    int ret;

    if (llly_log_level < level) {
        return;
    }

    if (plugin)
        ret = asprintf(&plugin_msg, "%s (reported by plugin %s, %s())", format, plugin, function);
    else
        ret = asprintf(&plugin_msg, "%s", format);

    if (ret == -1) {
        LOGMEM(ctx);
        return;
    }

    va_start(ap, format);
    log_vprintf(ctx, level, (level == LLLY_LLERR ? LLLY_EPLUGIN : 0), 0, NULL, plugin_msg, ap);
    va_end(ap);

    free(plugin_msg);
}

static enum LLLY_VLOG_ELEM extvelog2velog[] = {
    LLLY_VLOG_NONE, /* LLLYEXT_VLOG_NONE */
    LLLY_VLOG_XML, /* LLLYEXT_VLOG_XML */
    LLLY_VLOG_LYS, /* LLLYEXT_VLOG_LYS */
    LLLY_VLOG_LYD, /* LLLYEXT_VLOG_LYD */
    LLLY_VLOG_STR, /* LLLYEXT_VLOG_STR */
    LLLY_VLOG_PREV, /* LLLYEXT_VLOG_PREV */
};

API void
lllyext_vlog(const struct llly_ctx *ctx, LLLY_VECODE vecode, const char *plugin, const char *function,
           LLLYEXT_VLOG_ELEM elem_type, const void *elem, const char *format, ...)
{
    enum LLLY_VLOG_ELEM etype = extvelog2velog[elem_type];
    char *plugin_msg, *path = NULL;
    va_list ap;
    int ret;

    if (path_flag && (etype != LLLY_VLOG_NONE)) {
        if (etype == LLLY_VLOG_PREV) {
            /* use previous path */
            const struct llly_err_item *first = llly_err_first(ctx);
            if (first && first->prev->path) {
                path = strdup(first->prev->path);
            }
        } else {
            /* print path */
            if (!elem) {
                /* top-level */
                path = strdup("/");
            } else {
                llly_vlog_build_path(etype, elem, &path, 0, 0);
            }
        }
    }

    if (plugin)
        ret = asprintf(&plugin_msg, "%s (reported by plugin %s, %s())", format, plugin, function);
    else
        ret = asprintf(&plugin_msg, "%s", format);

    if (ret == -1) {
        LOGMEM(ctx);
        free(path);
        return;
    }

    va_start(ap, format);
    /* path is spent and should not be freed! */
    log_vprintf(ctx, LLLY_LLERR, LLLY_EVALID, vecode, path, plugin_msg, ap);
    va_end(ap);

    free(plugin_msg);
}

const char *llly_errs[] = {
/* LLLYE_SUCCESS */      "",
/* LLLYE_XML_MISS */     "Missing %s \"%s\".",
/* LLLYE_XML_INVAL */    "Invalid %s.",
/* LLLYE_XML_INCHAR */   "Encountered invalid character sequence \"%.10s\".",

/* LLLYE_EOF */          "Unexpected end of input data.",
/* LLLYE_INSTMT */       "Invalid keyword \"%s\".",
/* LLLYE_INCHILDSTMT */  "Invalid keyword \"%s\" as a child to \"%s\".",
/* LLLYE_INPAR */        "Invalid ancestor \"%s\" of \"%s\".",
/* LLLYE_INID */         "Invalid identifier \"%s\" (%s).",
/* LLLYE_INDATE */       "Invalid date \"%s\", valid date in format \"YYYY-MM-DD\" expected.",
/* LLLYE_INARG */        "Invalid value \"%s\" of \"%s\".",
/* LLLYE_MISSSTMT */     "Missing keyword \"%s\".",
/* LLLYE_MISSCHILDSTMT */ "Missing keyword \"%s\" as a child to \"%s\".",
/* LLLYE_MISSARG */      "Missing argument \"%s\" to keyword \"%s\".",
/* LLLYE_TOOMANY */      "Too many instances of \"%s\" in \"%s\".",
/* LLLYE_DUPID */        "Duplicated %s identifier \"%s\".",
/* LLLYE_DUPLEAFLIST */  "Duplicated instance of \"%s\" leaf-list (\"%s\").",
/* LLLYE_DUPLIST */      "Duplicated instance of \"%s\" list.",
/* LLLYE_NOUNIQ */       "Unique data leaf(s) \"%s\" not satisfied in \"%s\" and \"%s\".",
/* LLLYE_ENUM_INVAL */   "Invalid value \"%d\" of \"%s\" enum, restricted enum value does not match the base type value \"%d\".",
/* LLLYE_ENUM_INNAME */  "Adding new enum name \"%s\" in restricted enumeration type is not allowed.",
/* LLLYE_ENUM_DUPVAL */  "The value \"%d\" of \"%s\" enum has already been assigned to \"%s\" enum.",
/* LLLYE_ENUM_DUPNAME */ "The enum name \"%s\" has already been assigned to another enum.",
/* LLLYE_ENUM_WS */      "The enum name \"%s\" includes invalid leading or trailing whitespaces.",
/* LLLYE_BITS_INVAL */   "Invalid position \"%d\" of \"%s\" bit, restricted bits position does not match the base type position \"%d\".",
/* LLLYE_BITS_INNAME */  "Adding new bit name \"%s\" in restricted bits type is not allowed.",
/* LLLYE_BITS_DUPVAL */  "The position \"%d\" of \"%s\" bit has already been assigned to \"%s\" bit.",
/* LLLYE_BITS_DUPNAME */ "The bit name \"%s\" has already been assigned to another bit.",
/* LLLYE_INMOD */        "Module name \"%s\" refers to an unknown module.",
/* LLLYE_INMOD_LEN */    "Module name \"%.*s\" refers to an unknown module.",
/* LLLYE_KEY_NLEAF */    "Key \"%s\" is not a leaf.",
/* LLLYE_KEY_TYPE */     "Key \"%s\" must not be the built-in type \"empty\".",
/* LLLYE_KEY_CONFIG */   "The \"config\" value of the \"%s\" key differs from its list config value.",
/* LLLYE_KEY_MISS */     "Leaf \"%s\" defined as key in a list not found.",
/* LLLYE_KEY_DUP */      "Key identifier \"%s\" is not unique.",
/* LLLYE_INREGEX */      "Regular expression \"%s\" is not valid (\"%s\": %s).",
/* LLLYE_INRESOLV */     "Failed to resolve %s \"%s\".",
/* LLLYE_INSTATUS */     "A %s definition \"%s\" %s %s definition \"%s\".",
/* LLLYE_CIRC_LEAFREFS */"A circular chain of leafrefs detected.",
/* LLLYE_CIRC_FEATURES */"A circular chain features detected in \"%s\" feature.",
/* LLLYE_CIRC_IMPORTS */ "A circular dependency (import) for module \"%s\".",
/* LLLYE_CIRC_INCLUDES */"A circular dependency (include) for submodule \"%s\".",
/* LLLYE_INVER */        "Different YANG versions of a submodule and its main module.",
/* LLLYE_SUBMODULE */    "Unable to parse submodule, parse the main module instead.",

/* LLLYE_OBSDATA */      "Obsolete data \"%s\" instantiated.",
/* LLLYE_OBSTYPE */      "Data node \"%s\" with obsolete type \"%s\" instantiated.",
/* LLLYE_NORESOLV */     "No resolvents found for %s \"%s\".",
/* LLLYE_INELEM */       "Unknown element \"%s\".",
/* LLLYE_INELEM_LEN */   "Unknown element \"%.*s\".",
/* LLLYE_MISSELEM */     "Missing required element \"%s\" in \"%s\".",
/* LLLYE_INVAL */        "Invalid value \"%s\" in \"%s\" element.",
/* LLLYE_INMETA */       "Invalid \"%s:%s\" metadata with value \"%s\".",
/* LLLYE_INATTR */       "Invalid attribute \"%s\".",
/* LLLYE_MISSATTR */     "Missing attribute \"%s\" in \"%s\" element.",
/* LLLYE_NOCONSTR */     "Value \"%s\" does not satisfy the constraint \"%s\" (range, length, or pattern).",
/* LLLYE_INCHAR */       "Unexpected character(s) '%c' (%.15s).",
/* LLLYE_INPRED */       "Predicate resolution failed on \"%s\".",
/* LLLYE_MCASEDATA */    "Data for more than one case branch of \"%s\" choice present.",
/* LLLYE_NOMUST */       "Must condition \"%s\" not satisfied.",
/* LLLYE_NOWHEN */       "When condition \"%s\" not satisfied.",
/* LLLYE_INORDER */      "Invalid order of elements \"%s\" and \"%s\".",
/* LLLYE_INWHEN */       "Irresolvable when condition \"%s\".",
/* LLLYE_NOMIN */        "Too few \"%s\" elements.",
/* LLLYE_NOMAX */        "Too many \"%s\" elements.",
/* LLLYE_NOREQINS */     "Required instance of \"%s\" does not exist.",
/* LLLYE_NOLEAFREF */    "Leafref \"%s\" of value \"%s\" points to a non-existing leaf.",
/* LLLYE_NOMANDCHOICE */ "Mandatory choice \"%s\" missing a case branch.",

/* LLLYE_XPATH_INTOK */  "Unexpected XPath token %s (%.15s).",
/* LLLYE_XPATH_EOF */    "Unexpected XPath expression end.",
/* LLLYE_XPATH_INOP_1 */ "Cannot apply XPath operation %s on %s.",
/* LLLYE_XPATH_INOP_2 */ "Cannot apply XPath operation %s on %s and %s.",
/* LLLYE_XPATH_INCTX */  "Invalid context type %s in %s.",
/* LLLYE_XPATH_INMOD */  "Unknown module \"%.*s\".",
/* LLLYE_XPATH_INFUNC */ "Unknown XPath function \"%.*s\".",
/* LLLYE_XPATH_INARGCOUNT */ "Invalid number of arguments (%d) for the XPath function %.*s.",
/* LLLYE_XPATH_INARGTYPE */ "Wrong type of argument #%d (%s) for the XPath function %s.",
/* LLLYE_XPATH_DUMMY */   "Accessing the value of the dummy node \"%s\".",
/* LLLYE_XPATH_NOEND */   "Unterminated string delimited with %c (%.15s).",

/* LLLYE_PATH_INCHAR */  "Unexpected character(s) '%c' (\"%s\").",
/* LLLYE_PATH_INMOD */   "Module not found or not implemented.",
/* LLLYE_PATH_MISSMOD */ "Missing module name.",
/* LLLYE_PATH_INNODE */  "Schema node not found.",
/* LLLYE_PATH_INKEY */   "List key not found or on incorrect position (\"%s\").",
/* LLLYE_PATH_MISSKEY */ "List keys or position missing (\"%s\").",
/* LLLYE_PATH_INIDENTREF */ "Identityref predicate value \"%.*s\" missing module name.",
/* LLLYE_PATH_EXISTS */  "Node already exists.",
/* LLLYE_PATH_MISSPAR */ "Parent does not exist.",
/* LLLYE_PATH_PREDTOOMANY */ "Too many predicates.",
};

static const LLLY_VECODE ecode2vecode[] = {
    LLLYVE_SUCCESS,      /* LLLYE_SUCCESS */

    LLLYVE_XML_MISS,     /* LLLYE_XML_MISS */
    LLLYVE_XML_INVAL,    /* LLLYE_XML_INVAL */
    LLLYVE_XML_INCHAR,   /* LLLYE_XML_INCHAR */

    LLLYVE_EOF,          /* LLLYE_EOF */
    LLLYVE_INSTMT,       /* LLLYE_INSTMT */
    LLLYVE_INSTMT,       /* LLLYE_INCHILDSTMT */
    LLLYVE_INPAR,        /* LLLYE_INPAR */
    LLLYVE_INID,         /* LLLYE_INID */
    LLLYVE_INDATE,       /* LLLYE_INDATE */
    LLLYVE_INARG,        /* LLLYE_INARG */
    LLLYVE_MISSSTMT,     /* LLLYE_MISSCHILDSTMT */
    LLLYVE_MISSSTMT,     /* LLLYE_MISSSTMT */
    LLLYVE_MISSARG,      /* LLLYE_MISSARG */
    LLLYVE_TOOMANY,      /* LLLYE_TOOMANY */
    LLLYVE_DUPID,        /* LLLYE_DUPID */
    LLLYVE_DUPLEAFLIST,  /* LLLYE_DUPLEAFLIST */
    LLLYVE_DUPLIST,      /* LLLYE_DUPLIST */
    LLLYVE_NOUNIQ,       /* LLLYE_NOUNIQ */
    LLLYVE_ENUM_INVAL,   /* LLLYE_ENUM_INVAL */
    LLLYVE_ENUM_INNAME,  /* LLLYE_ENUM_INNAME */
    LLLYVE_ENUM_INVAL,   /* LLLYE_ENUM_DUPVAL */
    LLLYVE_ENUM_INNAME,  /* LLLYE_ENUM_DUPNAME */
    LLLYVE_ENUM_WS,      /* LLLYE_ENUM_WS */
    LLLYVE_BITS_INVAL,   /* LLLYE_BITS_INVAL */
    LLLYVE_BITS_INNAME,  /* LLLYE_BITS_INNAME */
    LLLYVE_BITS_INVAL,   /* LLLYE_BITS_DUPVAL */
    LLLYVE_BITS_INNAME,  /* LLLYE_BITS_DUPNAME */
    LLLYVE_INMOD,        /* LLLYE_INMOD */
    LLLYVE_INMOD,        /* LLLYE_INMOD_LEN */
    LLLYVE_KEY_NLEAF,    /* LLLYE_KEY_NLEAF */
    LLLYVE_KEY_TYPE,     /* LLLYE_KEY_TYPE */
    LLLYVE_KEY_CONFIG,   /* LLLYE_KEY_CONFIG */
    LLLYVE_KEY_MISS,     /* LLLYE_KEY_MISS */
    LLLYVE_KEY_DUP,      /* LLLYE_KEY_DUP */
    LLLYVE_INREGEX,      /* LLLYE_INREGEX */
    LLLYVE_INRESOLV,     /* LLLYE_INRESOLV */
    LLLYVE_INSTATUS,     /* LLLYE_INSTATUS */
    LLLYVE_CIRC_LEAFREFS,/* LLLYE_CIRC_LEAFREFS */
    LLLYVE_CIRC_FEATURES,/* LLLYE_CIRC_FEATURES */
    LLLYVE_CIRC_IMPORTS, /* LLLYE_CIRC_IMPORTS */
    LLLYVE_CIRC_INCLUDES,/* LLLYE_CIRC_INCLUDES */
    LLLYVE_INVER,        /* LLLYE_INVER */
    LLLYVE_SUBMODULE,    /* LLLYE_SUBMODULE */

    LLLYVE_OBSDATA,      /* LLLYE_OBSDATA */
    LLLYVE_OBSDATA,      /* LLLYE_OBSTYPE */
    LLLYVE_NORESOLV,     /* LLLYE_NORESOLV */
    LLLYVE_INELEM,       /* LLLYE_INELEM */
    LLLYVE_INELEM,       /* LLLYE_INELEM_LEN */
    LLLYVE_MISSELEM,     /* LLLYE_MISSELEM */
    LLLYVE_INVAL,        /* LLLYE_INVAL */
    LLLYVE_INMETA,       /* LLLYE_INMETA */
    LLLYVE_INATTR,       /* LLLYE_INATTR */
    LLLYVE_MISSATTR,     /* LLLYE_MISSATTR */
    LLLYVE_NOCONSTR,     /* LLLYE_NOCONSTR */
    LLLYVE_INCHAR,       /* LLLYE_INCHAR */
    LLLYVE_INPRED,       /* LLLYE_INPRED */
    LLLYVE_MCASEDATA,    /* LLLYE_MCASEDATA */
    LLLYVE_NOMUST,       /* LLLYE_NOMUST */
    LLLYVE_NOWHEN,       /* LLLYE_NOWHEN */
    LLLYVE_INORDER,      /* LLLYE_INORDER */
    LLLYVE_INWHEN,       /* LLLYE_INWHEN */
    LLLYVE_NOMIN,        /* LLLYE_NOMIN */
    LLLYVE_NOMAX,        /* LLLYE_NOMAX */
    LLLYVE_NOREQINS,     /* LLLYE_NOREQINS */
    LLLYVE_NOLEAFREF,    /* LLLYE_NOLEAFREF */
    LLLYVE_NOMANDCHOICE, /* LLLYE_NOMANDCHOICE */

    LLLYVE_XPATH_INTOK,  /* LLLYE_XPATH_INTOK */
    LLLYVE_XPATH_EOF,    /* LLLYE_XPATH_EOF */
    LLLYVE_XPATH_INOP,   /* LLLYE_XPATH_INOP_1 */
    LLLYVE_XPATH_INOP,   /* LLLYE_XPATH_INOP_2 */
    LLLYVE_XPATH_INCTX,  /* LLLYE_XPATH_INCTX */
    LLLYVE_XPATH_INMOD,  /* LLLYE_XPATH_INMOD */
    LLLYVE_XPATH_INFUNC, /* LLLYE_XPATH_INFUNC */
    LLLYVE_XPATH_INARGCOUNT, /* LLLYE_XPATH_INARGCOUNT */
    LLLYVE_XPATH_INARGTYPE, /* LLLYE_XPATH_INARGTYPE */
    LLLYVE_XPATH_DUMMY,  /* LLLYE_XPATH_DUMMY */
    LLLYVE_XPATH_NOEND,  /* LLLYE_XPATH_NOEND */

    LLLYVE_PATH_INCHAR,  /* LLLYE_PATH_INCHAR */
    LLLYVE_PATH_INMOD,   /* LLLYE_PATH_INMOD */
    LLLYVE_PATH_MISSMOD, /* LLLYE_PATH_MISSMOD */
    LLLYVE_PATH_INNODE,  /* LLLYE_PATH_INNODE */
    LLLYVE_PATH_INKEY,   /* LLLYE_PATH_INKEY */
    LLLYVE_PATH_MISSKEY, /* LLLYE_PATH_MISSKEY */
    LLLYVE_PATH_INIDENTREF, /* LLLYE_PATH_INIDENTREF */
    LLLYVE_PATH_EXISTS,  /* LLLYE_PATH_EXISTS */
    LLLYVE_PATH_MISSPAR, /* LLLYE_PATH_MISSPAR */
    LLLYVE_PATH_PREDTOOMANY, /* LLLYE_PATH_PREDTOOMANY */
};

static int
llly_vlog_build_path_print(char **path, uint16_t *index, const char *str, uint16_t str_len, uint16_t *length)
{
    void *mem;
    uint16_t step;

    if ((*index) < str_len) {
        /* enlarge buffer */
        step = (str_len < LLLY_BUF_STEP) ? LLLY_BUF_STEP : str_len;
        mem = realloc(*path, *length + *index + step + 1);
        LLLY_CHECK_ERR_RETURN(!mem, LOGMEM(NULL), -1);
        *path = mem;

        /* move data, lengths */
        memmove(&(*path)[*index + step], &(*path)[*index], *length);
        (*index) += step;
    }

    (*index) -= str_len;
    memcpy(&(*path)[*index], str, str_len);
    *length += str_len;

    return 0;
}

int
llly_vlog_build_path(enum LLLY_VLOG_ELEM elem_type, const void *elem, char **path, int schema_all_prefixes, int data_no_last_predicate)
{
    int i, j, yang_data_extension = 0;
    struct lllys_node_list *slist;
    struct lllys_node *sparent = NULL;
    struct lllyd_node *dlist, *diter;
    const struct lllys_module *top_smodule = NULL;
    const char *name, *prefix = NULL, *val_end, *val_start, *ext_name;
    char *str;
    uint16_t length, index;
    size_t len;

    length = 0;
    *path = malloc(1);
    LLLY_CHECK_ERR_RETURN(!(*path), LOGMEM(NULL), -1);
    index = 0;

    while (elem) {
        switch (elem_type) {
        case LLLY_VLOG_XML:
            name = ((struct lllyxml_elem *)elem)->name;
            prefix = ((struct lllyxml_elem *)elem)->ns ? ((struct lllyxml_elem *)elem)->ns->prefix : NULL;
            elem = ((struct lllyxml_elem *)elem)->parent;
            break;
        case LLLY_VLOG_LYS:
            if (!top_smodule) {
                /* remember the top module, it will act as the current module */
                for (sparent = (struct lllys_node *)elem; lllys_parent(sparent); sparent = lllys_parent(sparent));
                top_smodule = lllys_node_module(sparent);
            }

            /* skip uses */
            sparent = lllys_parent((struct lllys_node *)elem);
            while (sparent && (sparent->nodetype == LLLYS_USES)) {
                sparent = lllys_parent(sparent);
            }
            if (!sparent || (lllys_node_module((struct lllys_node *)elem) != top_smodule) || schema_all_prefixes) {
                prefix = lllys_node_module((struct lllys_node *)elem)->name;
            } else {
                prefix = NULL;
            }

            if (((struct lllys_node *)elem)->nodetype & (LLLYS_AUGMENT | LLLYS_GROUPING)) {
                if (llly_vlog_build_path_print(path, &index, "]", 1, &length)) {
                    return -1;
                }

                name = ((struct lllys_node *)elem)->name;
                if (llly_vlog_build_path_print(path, &index, name, strlen(name), &length)) {
                    return -1;
                }

                if (((struct lllys_node *)elem)->nodetype == LLLYS_GROUPING) {
                    name = "{grouping}[";
                } else { /* augment */
                    name = "{augment}[";
                }
            } else if (((struct lllys_node *)elem)->nodetype == LLLYS_EXT) {
                name = ((struct lllys_ext_instance *)elem)->def->name;
                if (!strcmp(name, "yang-data")) {
                    yang_data_extension = 1;
                    name = ((struct lllys_ext_instance *)elem)->arg_value;
                    prefix = lllys_node_module((struct lllys_node *)elem)->name;
                }
            } else {
                name = ((struct lllys_node *)elem)->name;
            }

            if (((struct lllys_node *)elem)->nodetype == LLLYS_EXT) {
                if (((struct lllys_ext_instance*)elem)->parent_type == LLLYEXT_PAR_NODE) {
                    elem = (struct lllys_node*)((struct lllys_ext_instance*)elem)->parent;
                } else {
                    sparent = NULL;
                    elem = NULL;
                }
                break;
            }

            /* need to find the parent again because we don't want to skip augments */
            do {
                sparent = ((struct lllys_node *)elem)->parent;
                elem = lllys_parent((struct lllys_node *)elem);
            } while (elem && (((struct lllys_node *)elem)->nodetype == LLLYS_USES));
            break;
        case LLLY_VLOG_LYD:
            name = ((struct lllyd_node *)elem)->schema->name;
            if (!((struct lllyd_node *)elem)->parent ||
                    lllyd_node_module((struct lllyd_node *)elem) != lllyd_node_module(((struct lllyd_node *)elem)->parent)) {
                prefix = lllyd_node_module((struct lllyd_node *)elem)->name;
            } else {
                prefix = NULL;
            }

            /* handle predicates (keys) in case of lists */
            if (!data_no_last_predicate || index) {
                if (((struct lllyd_node *)elem)->schema->nodetype == LLLYS_LIST) {
                    dlist = (struct lllyd_node *)elem;
                    slist = (struct lllys_node_list *)((struct lllyd_node *)elem)->schema;
                    if (slist->keys_size) {
                        /* schema list with keys - use key values in predicates */
                        for (i = slist->keys_size - 1; i > -1; i--) {
                            LLLY_TREE_FOR(dlist->child, diter) {
                                if (diter->schema == (struct lllys_node *)slist->keys[i]) {
                                    break;
                                }
                            }
                            if (diter && ((struct lllyd_node_leaf_list *)diter)->value_str) {
                                if (strchr(((struct lllyd_node_leaf_list *)diter)->value_str, '\'')) {
                                    val_start = "=\"";
                                    val_end = "\"]";
                                } else {
                                    val_start = "='";
                                    val_end = "']";
                                }

                                /* print value */
                                if (llly_vlog_build_path_print(path, &index, val_end, 2, &length)) {
                                    return -1;
                                }
                                len = strlen(((struct lllyd_node_leaf_list *)diter)->value_str);
                                if (llly_vlog_build_path_print(path, &index,
                                        ((struct lllyd_node_leaf_list *)diter)->value_str, len, &length)) {
                                    return -1;
                                }

                                /* print schema name */
                                if (llly_vlog_build_path_print(path, &index, val_start, 2, &length)) {
                                    return -1;
                                }
                                len = strlen(diter->schema->name);
                                if (llly_vlog_build_path_print(path, &index, diter->schema->name, len, &length)) {
                                    return -1;
                                }

                                if (lllyd_node_module(dlist) != lllyd_node_module(diter)) {
                                    if (llly_vlog_build_path_print(path, &index, ":", 1, &length)) {
                                        return -1;
                                    }
                                    len = strlen(lllyd_node_module(diter)->name);
                                    if (llly_vlog_build_path_print(path, &index, lllyd_node_module(diter)->name, len, &length)) {
                                        return -1;
                                    }
                                }

                                if (llly_vlog_build_path_print(path, &index, "[", 1, &length)) {
                                    return -1;
                                }
                            }
                        }
                    } else {
                        /* schema list without keys - use instance position */
                        i = j = lllyd_list_pos(dlist);
                        len = 1;
                        while (j > 9) {
                            ++len;
                            j /= 10;
                        }

                        if (llly_vlog_build_path_print(path, &index, "]", 1, &length)) {
                            return -1;
                        }

                        str = malloc(len + 1);
                        LLLY_CHECK_ERR_RETURN(!str, LOGMEM(NULL), -1);
                        sprintf(str, "%d", i);

                        if (llly_vlog_build_path_print(path, &index, str, len, &length)) {
                            free(str);
                            return -1;
                        }
                        free(str);

                        if (llly_vlog_build_path_print(path, &index, "[", 1, &length)) {
                            return -1;
                        }
                    }
                } else if (((struct lllyd_node *)elem)->schema->nodetype == LLLYS_LEAFLIST &&
                        ((struct lllyd_node_leaf_list *)elem)->value_str) {
                    if (strchr(((struct lllyd_node_leaf_list *)elem)->value_str, '\'')) {
                        val_start = "[.=\"";
                        val_end = "\"]";
                    } else {
                        val_start = "[.='";
                        val_end = "']";
                    }

                    if (llly_vlog_build_path_print(path, &index, val_end, 2, &length)) {
                        return -1;
                    }
                    len = strlen(((struct lllyd_node_leaf_list *)elem)->value_str);
                    if (llly_vlog_build_path_print(path, &index, ((struct lllyd_node_leaf_list *)elem)->value_str, len, &length)) {
                        return -1;
                    }
                    if (llly_vlog_build_path_print(path, &index, val_start, 4, &length)) {
                        return -1;
                    }
                }
            }

            /* check if it is yang-data top element */
            if (!((struct lllyd_node *)elem)->parent) {
                ext_name = lllyp_get_yang_data_template_name(elem);
                if (ext_name) {
                    if (llly_vlog_build_path_print(path, &index, name, strlen(name), &length)) {
                        return -1;
                    }
                    if (llly_vlog_build_path_print(path, &index, "/", 1, &length)) {
                        return -1;
                    }
                    yang_data_extension = 1;
                    name = ext_name;
               }
            }

            elem = ((struct lllyd_node *)elem)->parent;
            break;
        case LLLY_VLOG_STR:
            len = strlen((const char *)elem);
            if (llly_vlog_build_path_print(path, &index, (const char *)elem, len, &length)) {
                return -1;
            }
            goto success;
        default:
            /* shouldn't be here */
            LOGINT(NULL);
            return -1;
        }
        if (name) {
            if (llly_vlog_build_path_print(path, &index, name, strlen(name), &length)) {
                return -1;
            }
            if (prefix) {
                if (yang_data_extension && llly_vlog_build_path_print(path, &index, "#", 1, &length)) {
                    return -1;
                }
                if (llly_vlog_build_path_print(path, &index, ":", 1, &length)) {
                    return -1;
                }
                if (llly_vlog_build_path_print(path, &index, prefix, strlen(prefix), &length)) {
                    return -1;
                }
            }
        }
        if (llly_vlog_build_path_print(path, &index, "/", 1, &length)) {
            return -1;
        }
        if ((elem_type == LLLY_VLOG_LYS) && !elem && sparent && (sparent->nodetype == LLLYS_AUGMENT)) {
            len = strlen(((struct lllys_node_augment *)sparent)->target_name);
            if (llly_vlog_build_path_print(path, &index, ((struct lllys_node_augment *)sparent)->target_name, len, &length)) {
                return -1;
            }
        }
    }

success:
    memmove(*path, (*path) + index, length);
    (*path)[length] = '\0';
    return 0;
}

void
llly_vlog(const struct llly_ctx *ctx, LLLY_ECODE ecode, enum LLLY_VLOG_ELEM elem_type, const void *elem, ...)
{
    va_list ap;
    const char *fmt;
    char* path = NULL;
    const struct llly_err_item *first;

    if ((ecode == LLLYE_PATH) && !path_flag) {
        return;
    }

    if (path_flag && (elem_type != LLLY_VLOG_NONE)) {
        if (elem_type == LLLY_VLOG_PREV) {
            /* use previous path */
            first = llly_err_first(ctx);
            if (first && first->prev->path) {
                path = strdup(first->prev->path);
            }
        } else {
            /* print path */
            if (!elem) {
                /* top-level */
                path = strdup("/");
            } else {
                llly_vlog_build_path(elem_type, elem, &path, 0, 0);
            }
        }
    }

    va_start(ap, elem);
    /* path is spent and should not be freed! */
    switch (ecode) {
    case LLLYE_SPEC:
        fmt = va_arg(ap, char *);
        log_vprintf(ctx, LLLY_LLERR, LLLY_EVALID, LLLYVE_SUCCESS, path, fmt, ap);
        break;
    case LLLYE_PATH:
        assert(path);
        log_vprintf(ctx, LLLY_LLERR, LLLY_EVALID, LLLYVE_SUCCESS, path, NULL, ap);
        break;
    default:
        log_vprintf(ctx, LLLY_LLERR, LLLY_EVALID, ecode2vecode[ecode], path, llly_errs[ecode], ap);
        break;
    }
    va_end(ap);
}

void
llly_vlog_str(const struct llly_ctx *ctx, enum LLLY_VLOG_ELEM elem_type, const char *str, ...)
{
    va_list ap;
    char *path = NULL, *fmt, *ptr;
    const struct llly_err_item *first;

    assert((elem_type == LLLY_VLOG_NONE) || (elem_type == LLLY_VLOG_PREV));

    if (elem_type == LLLY_VLOG_PREV) {
        /* use previous path */
        first = llly_err_first(ctx);
        if (first && first->prev->path) {
            path = strdup(first->prev->path);
        }
    }

    if (strchr(str, '%')) {
        /* must be enough */
        fmt = malloc(2 * strlen(str) + 1);
        strcpy(fmt, str);
        for (ptr = strchr(fmt, '%'); ptr; ptr = strchr(ptr + 2, '%')) {
            memmove(ptr + 1, ptr, strlen(ptr) + 1);
            ptr[0] = '%';
        }
    } else {
        fmt = strdup(str);
    }

    va_start(ap, str);
    /* path is spent and should not be freed! */
    log_vprintf(ctx, LLLY_LLERR, LLLY_EVALID, LLLYVE_SUCCESS, path, fmt, ap);
    va_end(ap);

    free(fmt);
}

API void
llly_err_print(struct llly_err_item *eitem)
{
    if (llly_log_opts & LLLY_LOLOG) {
        if (llly_log_clb) {
            llly_log_clb(eitem->level, eitem->msg, eitem->path);
        } else {
            fprintf(stderr, "libyang[%d]: %s%s", eitem->level, eitem->msg, eitem->path ? " " : "\n");
            if (eitem->path) {
                fprintf(stderr, "(path: %s)\n", eitem->path);
            }
        }
    }
}

static void
err_print(struct llly_ctx *ctx, struct llly_err_item *last_eitem)
{
    if (!last_eitem) {
        last_eitem = pthread_getspecific(ctx->errlist_key);
    } else {
        /* this last was already stored before, do not write it again */
        last_eitem = last_eitem->next;
    }

    if ((log_opt != ILO_STORE) && (log_opt != ILO_IGNORE)) {
        for (; last_eitem; last_eitem = last_eitem->next) {
            llly_err_print(last_eitem);

            /* also properly update llly_errno */
            if (last_eitem->level == LLLY_LLERR) {
                llly_errno = last_eitem->no;
            }
        }
    }
}

/**
 * @brief Make \p last_eitem the last error item ignoring any logging options.
 */
void
llly_err_free_next(struct llly_ctx *ctx, struct llly_err_item *last_eitem)
{
    if (!last_eitem) {
        llly_err_clean(ctx, NULL);
    } else if (last_eitem->next) {
        llly_err_clean(ctx, last_eitem->next);
    }
}

/**
 * @brief Properly clean errors from \p ctx based on the user and internal logging options
 * after resolving schema/data unres.
 *
 * @param[in] ctx Context used.
 * @param[in] prev_eitem Most recent error item before resolving data unres.
 * @param[in] keep Whether to keep the stored errors.
 */
static void
err_clean(struct llly_ctx *ctx, struct llly_err_item *prev_eitem, int keep)
{
    struct llly_err_item *first;

    /* internal options take precedence */
    if (log_opt == ILO_STORE) {
        /* keep all the new errors */
    } else if ((log_opt == ILO_IGNORE) || !keep || !(llly_log_opts & LLLY_LOSTORE)) {
        /* throw away all the new errors */
        llly_err_free_next(ctx, prev_eitem);
    } else if ((llly_log_opts & LLLY_LOSTORE_LAST) == LLLY_LOSTORE_LAST) {
        /* keep only the most recent error */
        first = pthread_getspecific(ctx->errlist_key);
        if (!first) {
            /* no errors whatsoever */
            return;
        }
        prev_eitem = first->prev;

        /* put the context errlist in order */
        pthread_setspecific(ctx->errlist_key, prev_eitem);
        assert(!prev_eitem->prev->next || (prev_eitem->prev->next == prev_eitem));
        prev_eitem->prev->next = NULL;
        prev_eitem->prev = prev_eitem;

        /* free all the errlist items except the last one, do not free any if there is only one */
        if (prev_eitem != first) {
            llly_err_free(first);
        }
    }
}

void
llly_ilo_change(struct llly_ctx *ctx, enum int_log_opts new_ilo, enum int_log_opts *prev_ilo, struct llly_err_item **prev_last_eitem)
{
    assert(prev_ilo);

    *prev_ilo = log_opt;
    if (new_ilo == ILO_STORE) {
        /* only in this case the errors are only temporarily stored */
        assert(ctx && prev_last_eitem);
        *prev_last_eitem = (struct llly_err_item *)llly_err_first(ctx);
        if (*prev_last_eitem) {
            *prev_last_eitem = (*prev_last_eitem)->prev;
        }
    }

    if (log_opt != ILO_IGNORE) {
        log_opt = new_ilo;
    } /* else we can just keep it, useless to change it */
}

void
llly_ilo_restore(struct llly_ctx *ctx, enum int_log_opts prev_ilo, struct llly_err_item *prev_last_eitem, int keep_and_print)
{
    assert(log_opt != ILO_LOG);
    if (log_opt != ILO_STORE) {
        /* nothing to print or free */
        assert(log_opt == prev_ilo || (!ctx && !prev_last_eitem && !keep_and_print));
        log_opt = prev_ilo;
        return;
    }

    assert(ctx);

    log_opt = prev_ilo;
    if (keep_and_print) {
        err_print(ctx, prev_last_eitem);
    }
    err_clean(ctx, prev_last_eitem, keep_and_print);
}

void
llly_err_last_set_apptag(const struct llly_ctx *ctx, const char *apptag)
{
    struct llly_err_item *i;

    if (log_opt != ILO_IGNORE) {
        i = llly_err_first(ctx);
        if (i) {
            i = i->prev;
            i->apptag = strdup(apptag);
        }
    }
}
