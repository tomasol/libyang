/**
 * @file xml.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief XML parser implementation for libyang
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
#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>

#include "common.h"
#include "hash_table.h"
#include "printer.h"
#include "parser.h"
#include "tree_schema.h"
#include "xml_internal.h"

#define ign_xmlws(p)                                                    \
    while (is_xmlws(*p)) {                                              \
        p++;                                                            \
    }

static struct lllyxml_attr *lllyxml_dup_attr(struct llly_ctx *ctx, struct lllyxml_elem *parent, struct lllyxml_attr *attr);

API const struct lllyxml_ns *
lllyxml_get_ns(const struct lllyxml_elem *elem, const char *prefix)
{
    FUN_IN;

    struct lllyxml_attr *attr;

    if (!elem) {
        return NULL;
    }

    for (attr = elem->attr; attr; attr = attr->next) {
        if (attr->type != LLLYXML_ATTR_NS) {
            continue;
        }
        if (!attr->name) {
            if (!prefix) {
                /* default namespace found */
                if (!attr->value) {
                    /* empty default namespace -> no default namespace */
                    return NULL;
                }
                return (struct lllyxml_ns *)attr;
            }
        } else if (prefix && !strcmp(attr->name, prefix)) {
            /* prefix found */
            return (struct lllyxml_ns *)attr;
        }
    }

    /* go recursively */
    return lllyxml_get_ns(elem->parent, prefix);
}

static void
lllyxml_correct_attr_ns(struct llly_ctx *ctx, struct lllyxml_attr *attr, struct lllyxml_elem *attr_parent, int copy_ns)
{
    const struct lllyxml_ns *tmp_ns;
    struct lllyxml_elem *ns_root, *attr_root;

    if ((attr->type != LLLYXML_ATTR_NS) && attr->ns) {
        /* find the root of attr */
        for (attr_root = attr_parent; attr_root->parent; attr_root = attr_root->parent);

        /* find the root of attr NS */
        for (ns_root = attr->ns->parent; ns_root->parent; ns_root = ns_root->parent);

        /* attr NS is defined outside attr parent subtree */
        if (ns_root != attr_root) {
            if (copy_ns) {
                tmp_ns = attr->ns;
                /* we may have already copied the NS over? */
                attr->ns = lllyxml_get_ns(attr_parent, tmp_ns->prefix);

                /* we haven't copied it over, copy it now */
                if (!attr->ns) {
                    attr->ns = (struct lllyxml_ns *)lllyxml_dup_attr(ctx, attr_parent, (struct lllyxml_attr *)tmp_ns);
                }
            } else {
                attr->ns = NULL;
            }
        }
    }
}

static struct lllyxml_attr *
lllyxml_dup_attr(struct llly_ctx *ctx, struct lllyxml_elem *parent, struct lllyxml_attr *attr)
{
    struct lllyxml_attr *result, *a;

    if (!attr || !parent) {
        return NULL;
    }

    if (attr->type == LLLYXML_ATTR_NS) {
        /* this is correct, despite that all attributes seems like a standard
         * attributes (struct lllyxml_attr), some of them can be namespace
         * definitions (and in that case they are struct lllyxml_ns).
         */
        result = (struct lllyxml_attr *)calloc(1, sizeof (struct lllyxml_ns));
    } else {
        result = calloc(1, sizeof (struct lllyxml_attr));
    }
    LLLY_CHECK_ERR_RETURN(!result, LOGMEM(ctx), NULL);

    result->value = lllydict_insert(ctx, attr->value, 0);
    result->name = lllydict_insert(ctx, attr->name, 0);
    result->type = attr->type;

    /* set namespace in case of standard attributes */
    if (result->type == LLLYXML_ATTR_STD && attr->ns) {
        result->ns = attr->ns;
        lllyxml_correct_attr_ns(ctx, result, parent, 1);
    }

    /* set parent pointer in case of namespace attribute */
    if (result->type == LLLYXML_ATTR_NS) {
        ((struct lllyxml_ns *)result)->parent = parent;
    }

    /* put attribute into the parent's attributes list */
    if (parent->attr) {
        /* go to the end of the list */
        for (a = parent->attr; a->next; a = a->next);
        /* and append new attribute */
        a->next = result;
    } else {
        /* add the first attribute in the list */
        parent->attr = result;
    }

    return result;
}

void
lllyxml_correct_elem_ns(struct llly_ctx *ctx, struct lllyxml_elem *elem, int copy_ns, int correct_attrs)
{
    const struct lllyxml_ns *tmp_ns;
    struct lllyxml_elem *elem_root, *ns_root, *tmp, *iter;
    struct lllyxml_attr *attr;

    /* find the root of elem */
    for (elem_root = elem; elem_root->parent; elem_root = elem_root->parent);

    LLLY_TREE_DFS_BEGIN(elem, tmp, iter) {
        if (iter->ns) {
            /* find the root of elem NS */
            for (ns_root = iter->ns->parent; ns_root; ns_root = ns_root->parent);

            /* elem NS is defined outside elem subtree */
            if (ns_root != elem_root) {
                if (copy_ns) {
                    tmp_ns = iter->ns;
                    /* we may have already copied the NS over? */
                    iter->ns = lllyxml_get_ns(iter, tmp_ns->prefix);

                    /* we haven't copied it over, copy it now */
                    if (!iter->ns) {
                        iter->ns = (struct lllyxml_ns *)lllyxml_dup_attr(ctx, iter, (struct lllyxml_attr *)tmp_ns);
                    }
                } else {
                    iter->ns = NULL;
                }
            }
        }
        if (correct_attrs) {
            LLLY_TREE_FOR(iter->attr, attr) {
                lllyxml_correct_attr_ns(ctx, attr, elem_root, copy_ns);
            }
        }
        LLLY_TREE_DFS_END(elem, tmp, iter);
    }
}

struct lllyxml_elem *
lllyxml_dup_elem(struct llly_ctx *ctx, struct lllyxml_elem *elem, struct lllyxml_elem *parent, int recursive, int with_siblings)
{
    struct lllyxml_elem *dup, *result = NULL;
    struct lllyxml_attr *attr;

    if (!elem) {
        return NULL;
    }

    LLLY_TREE_FOR(elem, elem) {
        dup = calloc(1, sizeof *dup);
        LLLY_CHECK_ERR_RETURN(!dup, LOGMEM(ctx), NULL);
        dup->content = lllydict_insert(ctx, elem->content, 0);
        dup->name = lllydict_insert(ctx, elem->name, 0);
        dup->flags = elem->flags;
        dup->prev = dup;

        if (parent) {
            lllyxml_add_child(ctx, parent, dup);
        } else if (result) {
            dup->prev = result->prev;
            dup->prev->next = dup;
            result->prev = dup;
        }

        /* keep old namespace for now */
        dup->ns = elem->ns;

        /* duplicate attributes */
        for (attr = elem->attr; attr; attr = attr->next) {
            lllyxml_dup_attr(ctx, dup, attr);
        }

        /* correct namespaces */
        lllyxml_correct_elem_ns(ctx, dup, 1, 0);

        if (recursive) {
            /* duplicate children */
            lllyxml_dup_elem(ctx, elem->child, dup, 1, 1);
        }

        /* set result (first sibling) */
        if (!result) {
            result = dup;
        }

        if (!with_siblings) {
            break;
        }
    }

    return result;
}

API struct lllyxml_elem *
lllyxml_dup(struct llly_ctx *ctx, struct lllyxml_elem *root)
{
    FUN_IN;

    return lllyxml_dup_elem(ctx, root, NULL, 1, 0);
}

void
lllyxml_unlink_elem(struct llly_ctx *ctx, struct lllyxml_elem *elem, int copy_ns)
{
    struct lllyxml_elem *parent, *first;

    if (!elem) {
        return;
    }

    /* store pointers to important nodes */
    parent = elem->parent;

    /* unlink from parent */
    if (parent) {
        if (parent->child == elem) {
            /* we unlink the first child */
            /* update the parent's link */
            parent->child = elem->next;
        }
        /* forget about the parent */
        elem->parent = NULL;
    }

    if (copy_ns < 2) {
        lllyxml_correct_elem_ns(ctx, elem, copy_ns, 1);
    }

    /* unlink from siblings */
    if (elem->prev == elem) {
        /* there are no more siblings */
        return;
    }
    if (elem->next) {
        elem->next->prev = elem->prev;
    } else {
        /* unlinking the last element */
        if (parent) {
            first = parent->child;
        } else {
            first = elem;
            while (first->prev->next) {
                first = first->prev;
            }
        }
        first->prev = elem->prev;
    }
    if (elem->prev->next) {
        elem->prev->next = elem->next;
    }

    /* clean up the unlinked element */
    elem->next = NULL;
    elem->prev = elem;
}

API void
lllyxml_unlink(struct llly_ctx *ctx, struct lllyxml_elem *elem)
{
    FUN_IN;

    if (!elem) {
        return;
    }

    lllyxml_unlink_elem(ctx, elem, 1);
}

void
lllyxml_free_attr(struct llly_ctx *ctx, struct lllyxml_elem *parent, struct lllyxml_attr *attr)
{
    struct lllyxml_attr *aiter, *aprev;

    if (!attr) {
        return;
    }

    if (parent) {
        /* unlink attribute from the parent's list of attributes */
        aprev = NULL;
        for (aiter = parent->attr; aiter; aiter = aiter->next) {
            if (aiter == attr) {
                break;
            }
            aprev = aiter;
        }
        if (!aiter) {
            /* attribute to remove not found */
            return;
        }

        if (!aprev) {
            /* attribute is first in parent's list of attributes */
            parent->attr = attr->next;
        } else {
            /* reconnect previous attribute to the next */
            aprev->next = attr->next;
        }
    }
    lllydict_remove(ctx, attr->name);
    lllydict_remove(ctx, attr->value);
    free(attr);
}

void
lllyxml_free_attrs(struct llly_ctx *ctx, struct lllyxml_elem *elem)
{
    struct lllyxml_attr *a, *next;
    if (!elem || !elem->attr) {
        return;
    }

    a = elem->attr;
    do {
        next = a->next;

        lllydict_remove(ctx, a->name);
        lllydict_remove(ctx, a->value);
        free(a);

        a = next;
    } while (a);
}

static void
lllyxml_free_elem(struct llly_ctx *ctx, struct lllyxml_elem *elem)
{
    struct lllyxml_elem *e, *next;

    if (!elem) {
        return;
    }

    lllyxml_free_attrs(ctx, elem);
    LLLY_TREE_FOR_SAFE(elem->child, next, e) {
        lllyxml_free_elem(ctx, e);
    }
    lllydict_remove(ctx, elem->name);
    lllydict_remove(ctx, elem->content);
    free(elem);
}

API void
lllyxml_free(struct llly_ctx *ctx, struct lllyxml_elem *elem)
{
    FUN_IN;

    if (!elem) {
        return;
    }

    lllyxml_unlink_elem(ctx, elem, 2);
    lllyxml_free_elem(ctx, elem);
}

API void
lllyxml_free_withsiblings(struct llly_ctx *ctx, struct lllyxml_elem *elem)
{
    FUN_IN;

    struct lllyxml_elem *iter, *aux;

    if (!elem) {
        return;
    }

    /* optimization - avoid freeing (unlinking) the last node of the siblings list */
    /* so, first, free the node's predecessors to the beginning of the list ... */
    for(iter = elem->prev; iter->next; iter = aux) {
        aux = iter->prev;
        lllyxml_free(ctx, iter);
    }
    /* ... then, the node is the first in the siblings list, so free them all */
    LLLY_TREE_FOR_SAFE(elem, aux, iter) {
        lllyxml_free(ctx, iter);
    }
}

API const char *
lllyxml_get_attr(const struct lllyxml_elem *elem, const char *name, const char *ns)
{
    FUN_IN;

    struct lllyxml_attr *a;

    assert(elem);
    assert(name);

    for (a = elem->attr; a; a = a->next) {
        if (a->type != LLLYXML_ATTR_STD) {
            continue;
        }

        if (!strcmp(name, a->name)) {
            if ((!ns && !a->ns) || (ns && a->ns && !strcmp(ns, a->ns->value))) {
                return a->value;
            }
        }
    }

    return NULL;
}

int
lllyxml_add_child(struct llly_ctx *ctx, struct lllyxml_elem *parent, struct lllyxml_elem *elem)
{
    struct lllyxml_elem *e;

    assert(parent);
    assert(elem);

    /* (re)link element to parent */
    if (elem->parent) {
        lllyxml_unlink_elem(ctx, elem, 1);
    }
    elem->parent = parent;

    /* link parent to element */
    if (parent->child) {
        e = parent->child;
        elem->prev = e->prev;
        elem->next = NULL;
        elem->prev->next = elem;
        e->prev = elem;
    } else {
        parent->child = elem;
        elem->prev = elem;
        elem->next = NULL;
    }

    return EXIT_SUCCESS;
}

int
lllyxml_getutf8(struct llly_ctx *ctx, const char *buf, unsigned int *read)
{
    int c, aux;
    int i;

    c = buf[0];
    *read = 0;

    /* buf is NULL terminated string, so 0 means EOF */
    if (!c) {
        LOGVAL(ctx, LLLYE_EOF, LLLY_VLOG_NONE, NULL);
        return 0;
    }
    *read = 1;

    /* process character byte(s) */
    if ((c & 0xf8) == 0xf0) {
        /* four bytes character */
        *read = 4;

        c &= 0x07;
        for (i = 1; i <= 3; i++) {
            aux = buf[i];
            if ((aux & 0xc0) != 0x80) {
                LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "input character");
                return 0;
            }

            c = (c << 6) | (aux & 0x3f);
        }

        if (c < 0x1000 || c > 0x10ffff) {
            LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "input character");
            return 0;
        }
    } else if ((c & 0xf0) == 0xe0) {
        /* three bytes character */
        *read = 3;

        c &= 0x0f;
        for (i = 1; i <= 2; i++) {
            aux = buf[i];
            if ((aux & 0xc0) != 0x80) {
                LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "input character");
                return 0;
            }

            c = (c << 6) | (aux & 0x3f);
        }

        if (c < 0x800 || (c > 0xd7ff && c < 0xe000) || c > 0xfffd) {
            LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "input character");
            return 0;
        }
    } else if ((c & 0xe0) == 0xc0) {
        /* two bytes character */
        *read = 2;

        aux = buf[1];
        if ((aux & 0xc0) != 0x80) {
            LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "input character");
            return 0;
        }
        c = ((c & 0x1f) << 6) | (aux & 0x3f);

        if (c < 0x80) {
            LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "input character");
            return 0;
        }
    } else if (!(c & 0x80)) {
        /* one byte character */
        if (c < 0x20 && c != 0x9 && c != 0xa && c != 0xd) {
            /* invalid character */
            LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "input character");
            return 0;
        }
    } else {
        /* invalid character */
        LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "input character");
        return 0;
    }

    return c;
}

/* logs directly */
static int
parse_ignore(struct llly_ctx *ctx, const char *data, const char *endstr, unsigned int *len)
{
    unsigned int slen;
    const char *c = data;

    slen = strlen(endstr);

    while (*c && strncmp(c, endstr, slen)) {
        c++;
    }
    if (!*c) {
        LOGVAL(ctx, LLLYE_XML_MISS, LLLY_VLOG_NONE, NULL, "closing sequence", endstr);
        return EXIT_FAILURE;
    }
    c += slen;

    *len = c - data;
    return EXIT_SUCCESS;
}

/* logs directly, fails when return == NULL and *len == 0 */
static char *
parse_text(struct llly_ctx *ctx, const char *data, char delim, unsigned int *len)
{
#define BUFSIZE 1024

    char buf[BUFSIZE];
    char *result = NULL, *aux;
    unsigned int r;
    int o, size = 0;
    int cdsect = 0;
    int32_t n;

    for (*len = o = 0; cdsect || data[*len] != delim; o++) {
        if (!data[*len] || (!cdsect && !strncmp(&data[*len], "]]>", 3))) {
            LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "element content, \"]]>\" found");
            goto error;
        }

loop:

        if (o > BUFSIZE - 4) {
            /* add buffer into the result */
            if (result) {
                size = size + o;
                result = llly_realloc(result, size + 1);
            } else {
                size = o;
                result = malloc((size + 1) * sizeof *result);
            }
            LLLY_CHECK_ERR_RETURN(!result, LOGMEM(ctx), NULL);
            memcpy(&result[size - o], buf, o);

            /* write again into the beginning of the buffer */
            o = 0;
        }

        if (cdsect || !strncmp(&data[*len], "<![CDATA[", 9)) {
            /* CDSect */
            if (!cdsect) {
                cdsect = 1;
                *len += 9;
            }
            if (data[*len] && !strncmp(&data[*len], "]]>", 3)) {
                *len += 3;
                cdsect = 0;
                o--;            /* we don't write any data in this iteration */
            } else {
                buf[o] = data[*len];
                (*len)++;
            }
        } else if (data[*len] == '&') {
            (*len)++;
            if (data[*len] != '#') {
                /* entity reference - only predefined refs are supported */
                if (!strncmp(&data[*len], "lt;", 3)) {
                    buf[o] = '<';
                    *len += 3;
                } else if (!strncmp(&data[*len], "gt;", 3)) {
                    buf[o] = '>';
                    *len += 3;
                } else if (!strncmp(&data[*len], "amp;", 4)) {
                    buf[o] = '&';
                    *len += 4;
                } else if (!strncmp(&data[*len], "apos;", 5)) {
                    buf[o] = '\'';
                    *len += 5;
                } else if (!strncmp(&data[*len], "quot;", 5)) {
                    buf[o] = '\"';
                    *len += 5;
                } else {
                    LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "entity reference (only predefined references are supported)");
                    goto error;
                }
            } else {
                /* character reference */
                (*len)++;
                if (isdigit(data[*len])) {
                    for (n = 0; isdigit(data[*len]); (*len)++) {
                        n = (10 * n) + (data[*len] - '0');
                    }
                    if (data[*len] != ';') {
                        LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "character reference, missing semicolon");
                        goto error;
                    }
                } else if (data[(*len)++] == 'x' && isxdigit(data[*len])) {
                    for (n = 0; isxdigit(data[*len]); (*len)++) {
                        if (isdigit(data[*len])) {
                            r = (data[*len] - '0');
                        } else if (data[*len] > 'F') {
                            r = 10 + (data[*len] - 'a');
                        } else {
                            r = 10 + (data[*len] - 'A');
                        }
                        n = (16 * n) + r;
                    }
                } else {
                    LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "character reference");
                    goto error;

                }
                r = pututf8(ctx, &buf[o], n);
                if (!r) {
                    LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "character reference value");
                    goto error;
                }
                o += r - 1;     /* o is ++ in for loop */
                (*len)++;
            }
        } else {
            r = copyutf8(ctx, &buf[o], &data[*len]);
            if (!r) {
                goto error;
            }

            o += r - 1;     /* o is ++ in for loop */
            (*len) = (*len) + r;
        }
    }

    if (delim == '<' && !strncmp(&data[*len], "<![CDATA[", 9)) {
        /* ignore loop's end condition on beginning of CDSect */
        goto loop;
    }
#undef BUFSIZE

    if (o) {
        if (result) {
            size = size + o;
            aux = realloc(result, size + 1);
            result = aux;
        } else {
            size = o;
            result = malloc((size + 1) * sizeof *result);
        }
        LLLY_CHECK_ERR_RETURN(!result, LOGMEM(ctx), NULL);
        memcpy(&result[size - o], buf, o);
    }
    if (result) {
        result[size] = '\0';
    } else {
        size = 0;
        result = strdup("");
        LLLY_CHECK_ERR_RETURN(!result, LOGMEM(ctx), NULL)
    }

    return result;

error:
    *len = 0;
    free(result);
    return NULL;
}

/* logs directly */
static struct lllyxml_attr *
parse_attr(struct llly_ctx *ctx, const char *data, unsigned int *len, struct lllyxml_elem *parent)
{
    const char *c = data, *start, *delim;
    char *prefix = NULL, xml_flag, *str;
    int uc;
    struct lllyxml_attr *attr = NULL, *a;
    unsigned int size;

    /* check if it is attribute or namespace */
    if (!strncmp(c, "xmlns", 5)) {
        /* namespace */
        attr = calloc(1, sizeof (struct lllyxml_ns));
        LLLY_CHECK_ERR_RETURN(!attr, LOGMEM(ctx), NULL);

        attr->type = LLLYXML_ATTR_NS;
        ((struct lllyxml_ns *)attr)->parent = parent;
        c += 5;
        if (*c != ':') {
            /* default namespace, prefix will be empty */
            goto equal;
        }
        c++;                    /* go after ':' to the prefix value */
    } else {
        /* attribute */
        attr = calloc(1, sizeof *attr);
        LLLY_CHECK_ERR_RETURN(!attr, LOGMEM(ctx), NULL);

        attr->type = LLLYXML_ATTR_STD;
    }

    /* process name part of the attribute */
    start = c;
    uc = lllyxml_getutf8(ctx, c, &size);
    if (!is_xmlnamestartchar(uc)) {
        LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "NameStartChar of the attribute");
        free(attr);
        return NULL;
    }
    xml_flag = 4;
    if (*c == 'x') {
        xml_flag = 1;
    }
    c += size;
    uc = lllyxml_getutf8(ctx, c, &size);
    while (is_xmlnamechar(uc)) {
        if (attr->type == LLLYXML_ATTR_STD) {
            if ((*c == ':') && (xml_flag != 3)) {
                /* attribute in a namespace (but disregard the special "xml" namespace) */
                start = c + 1;

                /* look for the prefix in namespaces */
                prefix = malloc((c - data + 1) * sizeof *prefix);
                LLLY_CHECK_ERR_GOTO(!prefix, LOGMEM(ctx), error);
                memcpy(prefix, data, c - data);
                prefix[c - data] = '\0';
                attr->ns = lllyxml_get_ns(parent, prefix);
            } else if (((*c == 'm') && (xml_flag == 1)) ||
                    ((*c == 'l') && (xml_flag == 2))) {
                ++xml_flag;
            } else {
                xml_flag = 4;
            }
        }
        c += size;
        uc = lllyxml_getutf8(ctx, c, &size);
    }

    /* store the name */
    size = c - start;
    attr->name = lllydict_insert(ctx, start, size);

equal:
    /* check Eq mark that can be surrounded by whitespaces */
    ign_xmlws(c);
    if (*c != '=') {
        LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "attribute definition, \"=\" expected");
        goto error;
    }
    c++;
    ign_xmlws(c);

    /* process value part of the attribute */
    if (!*c || (*c != '"' && *c != '\'')) {
        LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "attribute value, \" or \' expected");
        goto error;
    }
    delim = c;
    str = parse_text(ctx, ++c, *delim, &size);
    if (!str && !size) {
        goto error;
    }
    attr->value = lllydict_insert_zc(ctx, str);

    *len = c + size + 1 - data; /* +1 is delimiter size */

    /* put attribute into the parent's attributes list */
    if (parent->attr) {
        /* go to the end of the list */
        for (a = parent->attr; a->next; a = a->next);
        /* and append new attribute */
        a->next = attr;
    } else {
        /* add the first attribute in the list */
        parent->attr = attr;
    }

    free(prefix);
    return attr;

error:
    lllyxml_free_attr(ctx, NULL, attr);
    free(prefix);
    return NULL;
}

/* logs directly */
struct lllyxml_elem *
lllyxml_parse_elem(struct llly_ctx *ctx, const char *data, unsigned int *len, struct lllyxml_elem *parent, int options)
{
    const char *c = data, *start, *e;
    const char *lws;    /* leading white space for handling mixed content */
    int uc;
    char *str;
    char *prefix = NULL;
    unsigned int prefix_len = 0;
    struct lllyxml_elem *elem = NULL, *child;
    struct lllyxml_attr *attr;
    unsigned int size;
    int nons_flag = 0, closed_flag = 0;

    *len = 0;

    if (*c != '<') {
        return NULL;
    }

    /* locate element name */
    c++;
    e = c;

    uc = lllyxml_getutf8(ctx, e, &size);
    if (!is_xmlnamestartchar(uc)) {
        LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "NameStartChar of the element");
        return NULL;
    }
    e += size;
    uc = lllyxml_getutf8(ctx, e, &size);
    while (is_xmlnamechar(uc)) {
        if (*e == ':') {
            if (prefix_len) {
                LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_NONE, NULL, "element name, multiple colons found");
                goto error;
            }
            /* element in a namespace */
            start = e + 1;

            /* look for the prefix in namespaces */
            prefix_len = e - c;
            LLLY_CHECK_ERR_GOTO(prefix, LOGVAL(ctx, LLLYE_XML_INCHAR, LLLY_VLOG_NONE, NULL, e), error);
            prefix = malloc((prefix_len + 1) * sizeof *prefix);
            LLLY_CHECK_ERR_GOTO(!prefix, LOGMEM(ctx), error);
            memcpy(prefix, c, prefix_len);
            prefix[prefix_len] = '\0';
            c = start;
        }
        e += size;
        uc = lllyxml_getutf8(ctx, e, &size);
    }
    if (!*e) {
        LOGVAL(ctx, LLLYE_EOF, LLLY_VLOG_NONE, NULL);
        free(prefix);
        return NULL;
    }

    /* allocate element structure */
    elem = calloc(1, sizeof *elem);
    LLLY_CHECK_ERR_RETURN(!elem, free(prefix); LOGMEM(ctx), NULL);

    elem->next = NULL;
    elem->prev = elem;
    if (parent) {
        lllyxml_add_child(ctx, parent, elem);
    }

    /* store the name into the element structure */
    elem->name = lllydict_insert(ctx, c, e - c);
    c = e;

process:
    ign_xmlws(c);
    if (!strncmp("/>", c, 2)) {
        /* we are done, it was EmptyElemTag */
        c += 2;
        elem->content = lllydict_insert(ctx, "", 0);
        closed_flag = 1;
    } else if (*c == '>') {
        /* process element content */
        c++;
        lws = NULL;

        while (*c) {
            if (!strncmp(c, "</", 2)) {
                if (lws && !elem->child) {
                    /* leading white spaces were actually content */
                    goto store_content;
                }

                /* Etag */
                c += 2;
                /* get name and check it */
                e = c;
                uc = lllyxml_getutf8(ctx, e, &size);
                if (!is_xmlnamestartchar(uc)) {
                    LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_XML, elem, "NameStartChar of the element");
                    goto error;
                }
                e += size;
                uc = lllyxml_getutf8(ctx, e, &size);
                while (is_xmlnamechar(uc)) {
                    if (*e == ':') {
                        /* element in a namespace */
                        start = e + 1;

                        /* look for the prefix in namespaces */
                        if (!prefix || memcmp(prefix, c, e - c)) {
                            LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_XML, elem,
                                   "Invalid (different namespaces) opening (%s) and closing element tags.", elem->name);
                            goto error;
                        }
                        c = start;
                    }
                    e += size;
                    uc = lllyxml_getutf8(ctx, e, &size);
                }
                if (!*e) {
                    LOGVAL(ctx, LLLYE_EOF, LLLY_VLOG_NONE, NULL);
                    goto error;
                }

                /* check that it corresponds to opening tag */
                size = e - c;
                str = malloc((size + 1) * sizeof *str);
                LLLY_CHECK_ERR_GOTO(!str, LOGMEM(ctx), error);
                memcpy(str, c, e - c);
                str[e - c] = '\0';
                if (size != strlen(elem->name) || memcmp(str, elem->name, size)) {
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_XML, elem,
                           "Invalid (mixed names) opening (%s) and closing (%s) element tags.", elem->name, str);
                    free(str);
                    goto error;
                }
                free(str);
                c = e;

                ign_xmlws(c);
                if (*c != '>') {
                    LOGVAL(ctx, LLLYE_SPEC, LLLY_VLOG_XML, elem, "Data after closing element tag \"%s\".", elem->name);
                    goto error;
                }
                c++;
                if (!(elem->flags & LLLYXML_ELEM_MIXED) && !elem->content) {
                    /* there was no content, but we don't want NULL (only if mixed content) */
                    elem->content = lllydict_insert(ctx, "", 0);
                }
                closed_flag = 1;
                break;

            } else if (!strncmp(c, "<?", 2)) {
                if (lws) {
                    /* leading white spaces were only formatting */
                    lws = NULL;
                }
                /* PI - ignore it */
                c += 2;
                if (parse_ignore(ctx, c, "?>", &size)) {
                    goto error;
                }
                c += size;
            } else if (!strncmp(c, "<!--", 4)) {
                if (lws) {
                    /* leading white spaces were only formatting */
                    lws = NULL;
                }
                /* Comment - ignore it */
                c += 4;
                if (parse_ignore(ctx, c, "-->", &size)) {
                    goto error;
                }
                c += size;
            } else if (!strncmp(c, "<![CDATA[", 9)) {
                /* CDSect */
                goto store_content;
            } else if (*c == '<') {
                if (lws) {
                    if (elem->flags & LLLYXML_ELEM_MIXED) {
                        /* we have a mixed content */
                        goto store_content;
                    } else {
                        /* leading white spaces were only formatting */
                        lws = NULL;
                    }
                }
                if (elem->content) {
                    /* we have a mixed content */
                    if (options & LLLYXML_PARSE_NOMIXEDCONTENT) {
                        LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_XML, elem, "XML element with mixed content");
                        goto error;
                    }
                    child = calloc(1, sizeof *child);
                    LLLY_CHECK_ERR_GOTO(!child, LOGMEM(ctx), error);
                    child->content = elem->content;
                    elem->content = NULL;
                    lllyxml_add_child(ctx, elem, child);
                    elem->flags |= LLLYXML_ELEM_MIXED;
                }
                child = lllyxml_parse_elem(ctx, c, &size, elem, options);
                if (!child) {
                    goto error;
                }
                c += size;      /* move after processed child element */
            } else if (is_xmlws(*c)) {
                lws = c;
                ign_xmlws(c);
            } else {
store_content:
                /* store text content */
                if (lws) {
                    /* process content including the leading white spaces */
                    c = lws;
                    lws = NULL;
                }
                str = parse_text(ctx, c, '<', &size);
                if (!str && !size) {
                    goto error;
                }
                elem->content = lllydict_insert_zc(ctx, str);
                c += size;      /* move after processed text content */

                if (elem->child) {
                    /* we have a mixed content */
                    if (options & LLLYXML_PARSE_NOMIXEDCONTENT) {
                        LOGVAL(ctx, LLLYE_XML_INVAL, LLLY_VLOG_XML, elem, "XML element with mixed content");
                        goto error;
                    }
                    child = calloc(1, sizeof *child);
                    LLLY_CHECK_ERR_GOTO(!child, LOGMEM(ctx), error);
                    child->content = elem->content;
                    elem->content = NULL;
                    lllyxml_add_child(ctx, elem, child);
                    elem->flags |= LLLYXML_ELEM_MIXED;
                }
            }
        }
    } else {
        /* process attribute */
        attr = parse_attr(ctx, c, &size, elem);
        if (!attr) {
            goto error;
        }
        c += size;              /* move after processed attribute */

        /* check namespace */
        if (attr->type == LLLYXML_ATTR_NS) {
            if ((!prefix || !prefix[0]) && !attr->name) {
                if (attr->value) {
                    /* default prefix */
                    elem->ns = (struct lllyxml_ns *)attr;
                } else {
                    /* xmlns="" -> no namespace */
                    nons_flag = 1;
                }
            } else if (prefix && prefix[0] && attr->name && !strncmp(attr->name, prefix, prefix_len + 1)) {
                /* matching namespace with prefix */
                elem->ns = (struct lllyxml_ns *)attr;
            }
        }

        /* go back to finish element processing */
        goto process;
    }

    *len = c - data;

    if (!closed_flag) {
        LOGVAL(ctx, LLLYE_XML_MISS, LLLY_VLOG_XML, elem, "closing element tag", elem->name);
        goto error;
    }

    if (!elem->ns && !nons_flag && parent) {
        elem->ns = lllyxml_get_ns(parent, prefix_len ? prefix : NULL);
    }
    free(prefix);
    return elem;

error:
    lllyxml_free(ctx, elem);
    free(prefix);
    return NULL;
}

/* logs directly */
API struct lllyxml_elem *
lllyxml_parse_mem(struct llly_ctx *ctx, const char *data, int options)
{
    FUN_IN;

    const char *c = data;
    unsigned int len;
    struct lllyxml_elem *root, *first = NULL, *next;

    if (!ctx) {
        LOGARG;
        return NULL;
    }

repeat:
    /* process document */
    while (1) {
        if (!*c) {
            /* eof */
            return first;
        } else if (is_xmlws(*c)) {
            /* skip whitespaces */
            ign_xmlws(c);
        } else if (!strncmp(c, "<?", 2)) {
            /* XMLDecl or PI - ignore it */
            c += 2;
            if (parse_ignore(ctx, c, "?>", &len)) {
                goto error;
            }
            c += len;
        } else if (!strncmp(c, "<!--", 4)) {
            /* Comment - ignore it */
            c += 2;
            if (parse_ignore(ctx, c, "-->", &len)) {
                goto error;
            }
            c += len;
        } else if (!strncmp(c, "<!", 2)) {
            /* DOCTYPE */
            /* TODO - standalone ignore counting < and > */
            LOGERR(ctx, LLLY_EINVAL, "DOCTYPE not supported in XML documents.");
            goto error;
        } else if (*c == '<') {
            /* element - process it in next loop to strictly follow XML
             * format
             */
            break;
        } else {
            LOGVAL(ctx, LLLYE_XML_INCHAR, LLLY_VLOG_NONE, NULL, c);
            goto error;
        }
    }

    root = lllyxml_parse_elem(ctx, c, &len, NULL, options);
    if (!root) {
        goto error;
    } else if (!first) {
        first = root;
    } else {
        first->prev->next = root;
        root->prev = first->prev;
        first->prev = root;
    }
    c += len;

    /* ignore the rest of document where can be comments, PIs and whitespaces,
     * note that we are not detecting syntax errors in these parts
     */
    ign_xmlws(c);
    if (*c) {
        if (options & LLLYXML_PARSE_MULTIROOT) {
            goto repeat;
        } else {
            LOGWRN(ctx, "There are some not parsed data:\n%s", c);
        }
    }

    return first;

error:
    LLLY_TREE_FOR_SAFE(first, next, root) {
        lllyxml_free(ctx, root);
    }
    return NULL;
}

API struct lllyxml_elem *
lllyxml_parse_path(struct llly_ctx *ctx, const char *filename, int options)
{
    FUN_IN;

    struct lllyxml_elem *elem = NULL;
    size_t length;
    int fd;
    char *addr;

    if (!filename || !ctx) {
        LOGARG;
        return NULL;
    }

    fd = open(filename, O_RDONLY);
    if (fd == -1) {
        LOGERR(ctx, LLLY_EINVAL,"Opening file \"%s\" failed.", filename);
        return NULL;
    }
    if (lllyp_mmap(ctx, fd, 0, &length, (void **)&addr)) {
        LOGERR(ctx, LLLY_ESYS, "Mapping file descriptor into memory failed (%s()).", __func__);
        goto error;
    } else if (!addr) {
        /* empty XML file */
        goto error;
    }

    elem = lllyxml_parse_mem(ctx, addr, options);
    lllyp_munmap(addr, length);
    close(fd);

    return elem;

error:
    if (fd != -1) {
        close(fd);
    }

    return NULL;
}

int
lllyxml_dump_text(struct lllyout *out, const char *text, LLLYXML_DATA_TYPE type)
{
    unsigned int i, n;

    if (!text) {
        return 0;
    }

    for (i = n = 0; text[i]; i++) {
        switch (text[i]) {
        case '&':
            n += llly_print(out, "&amp;");
            break;
        case '<':
            n += llly_print(out, "&lt;");
            break;
        case '>':
            /* not needed, just for readability */
            n += llly_print(out, "&gt;");
            break;
        case '"':
            if (type == LLLYXML_DATA_ATTR) {
                n += llly_print(out, "&quot;");
                break;
            }
            /* falls through */
        default:
            llly_write(out, &text[i], 1);
            n++;
        }
    }

    return n;
}

static int
dump_elem(struct lllyout *out, const struct lllyxml_elem *e, int level, int options, int last_elem)
{
    int size = 0;
    struct lllyxml_attr *a;
    struct lllyxml_elem *child;
    const char *delim, *delim_outer;
    int indent;

    if (!e->name) {
        /* mixed content */
        if (e->content) {
            return lllyxml_dump_text(out, e->content, LLLYXML_DATA_ELEM);
        } else {
            return 0;
        }
    }

    delim = delim_outer = (options & LLLYXML_PRINT_FORMAT) ? "\n" : "";
    indent = 2 * level;
    if ((e->flags & LLLYXML_ELEM_MIXED) || (e->parent && (e->parent->flags & LLLYXML_ELEM_MIXED))) {
        delim = "";
    }
    if (e->parent && (e->parent->flags & LLLYXML_ELEM_MIXED)) {
        delim_outer = "";
        indent = 0;
    }
    if (last_elem && (options & LLLYXML_PRINT_NO_LAST_NEWLINE)) {
        delim_outer = "";
    }

    if (!(options & (LLLYXML_PRINT_OPEN | LLLYXML_PRINT_CLOSE | LLLYXML_PRINT_ATTRS)) || (options & LLLYXML_PRINT_OPEN))  {
        /* opening tag */
        if (e->ns && e->ns->prefix) {
            size += llly_print(out, "%*s<%s:%s", indent, "", e->ns->prefix, e->name);
        } else {
            size += llly_print(out, "%*s<%s", indent, "", e->name);
        }
    } else if (options & LLLYXML_PRINT_CLOSE) {
        indent = 0;
        goto close;
    }

    /* attributes */
    for (a = e->attr; a; a = a->next) {
        if (a->type == LLLYXML_ATTR_NS) {
            if (a->name) {
                size += llly_print(out, " xmlns:%s=\"%s\"", a->name, a->value ? a->value : "");
            } else {
                size += llly_print(out, " xmlns=\"%s\"", a->value ? a->value : "");
            }
        } else if (a->ns && a->ns->prefix) {
            size += llly_print(out, " %s:%s=\"%s\"", a->ns->prefix, a->name, a->value);
        } else {
            size += llly_print(out, " %s=\"%s\"", a->name, a->value);
        }
    }

    /* apply options */
    if ((options & LLLYXML_PRINT_CLOSE) && (options & LLLYXML_PRINT_OPEN)) {
        size += llly_print(out, "/>%s", delim);
        return size;
    } else if (options & LLLYXML_PRINT_OPEN) {
        llly_print(out, ">");
        return ++size;
    } else if (options & LLLYXML_PRINT_ATTRS) {
        return size;
    }

    if (!e->child && (!e->content || !e->content[0])) {
        size += llly_print(out, "/>%s", delim);
        return size;
    } else if (e->content && e->content[0]) {
        llly_print(out, ">");
        size++;

        size += lllyxml_dump_text(out, e->content, LLLYXML_DATA_ELEM);

        if (e->ns && e->ns->prefix) {
            size += llly_print(out, "</%s:%s>%s", e->ns->prefix, e->name, delim);
        } else {
            size += llly_print(out, "</%s>%s", e->name, delim);
        }
        return size;
    } else {
        size += llly_print(out, ">%s", delim);
    }

    /* go recursively */
    LLLY_TREE_FOR(e->child, child) {
        if (options & LLLYXML_PRINT_FORMAT) {
            size += dump_elem(out, child, level + 1, LLLYXML_PRINT_FORMAT, 0);
        } else {
            size += dump_elem(out, child, level, 0, 0);
        }
    }

close:
    /* closing tag */
    if (e->ns && e->ns->prefix) {
        size += llly_print(out, "%*s</%s:%s>%s", indent, "", e->ns->prefix, e->name, delim_outer);
    } else {
        size += llly_print(out, "%*s</%s>%s", indent, "", e->name, delim_outer);
    }

    return size;
}

static int
dump_siblings(struct lllyout *out, const struct lllyxml_elem *e, int options)
{
    const struct lllyxml_elem *start, *iter, *next;
    int ret = 0;

    if (e->parent) {
        start = e->parent->child;
    } else {
        start = e;
        while(start->prev && start->prev->next) {
            start = start->prev;
        }
    }

    LLLY_TREE_FOR_SAFE(start, next, iter) {
        ret += dump_elem(out, iter, 0, options, (next ? 0 : 1));
    }

    return ret;
}

API int
lllyxml_print_file(FILE *stream, const struct lllyxml_elem *elem, int options)
{
    FUN_IN;

    struct lllyout out;

    if (!stream || !elem) {
        return 0;
    }

    memset(&out, 0, sizeof out);

    out.type = LLLYOUT_STREAM;
    out.method.f = stream;

    if (options & LLLYXML_PRINT_SIBLINGS) {
        return dump_siblings(&out, elem, options);
    } else {
        return dump_elem(&out, elem, 0, options, 1);
    }
}

API int
lllyxml_print_fd(int fd, const struct lllyxml_elem *elem, int options)
{
    FUN_IN;

    struct lllyout out;

    if (fd < 0 || !elem) {
        return 0;
    }

    memset(&out, 0, sizeof out);

    out.type = LLLYOUT_FD;
    out.method.fd = fd;

    if (options & LLLYXML_PRINT_SIBLINGS) {
        return dump_siblings(&out, elem, options);
    } else {
        return dump_elem(&out, elem, 0, options, 1);
    }
}

API int
lllyxml_print_mem(char **strp, const struct lllyxml_elem *elem, int options)
{
    FUN_IN;

    struct lllyout out;
    int r;

    if (!strp || !elem) {
        return 0;
    }

    memset(&out, 0, sizeof out);

    out.type = LLLYOUT_MEMORY;

    if (options & LLLYXML_PRINT_SIBLINGS) {
        r = dump_siblings(&out, elem, options);
    } else {
        r = dump_elem(&out, elem, 0, options, 1);
    }

    *strp = out.method.mem.buf;
    return r;
}

API int
lllyxml_print_clb(ssize_t (*writeclb)(void *arg, const void *buf, size_t count), void *arg, const struct lllyxml_elem *elem, int options)
{
    FUN_IN;

    struct lllyout out;

    if (!writeclb || !elem) {
        return 0;
    }

    memset(&out, 0, sizeof out);

    out.type = LLLYOUT_CALLBACK;
    out.method.clb.f = writeclb;
    out.method.clb.arg = arg;

    if (options & LLLYXML_PRINT_SIBLINGS) {
        return dump_siblings(&out, elem, options);
    } else {
        return dump_elem(&out, elem, 0, options, 1);
    }
}
