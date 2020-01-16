/**
 * @file printer.h
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Printers for libyang
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#ifndef LLLY_PRINTER_H_
#define LLLY_PRINTER_H_

#include "libyang.h"
#include "tree_schema.h"
#include "tree_internal.h"

typedef enum LLLYOUT_TYPE {
    LLLYOUT_FD,          /**< file descriptor */
    LLLYOUT_STREAM,      /**< FILE stream */
    LLLYOUT_MEMORY,      /**< memory */
    LLLYOUT_CALLBACK     /**< print via provided callback */
} LLLYOUT_TYPE;

struct lllyout {
    LLLYOUT_TYPE type;
    union {
        int fd;
        FILE *f;
        struct {
            char *buf;
            size_t len;
            size_t size;
        } mem;
        struct {
            ssize_t (*f)(void *arg, const void *buf, size_t count);
            void *arg;
        } clb;
    } method;

    /* buffer for holes */
    char *buffered;
    size_t buf_len;
    size_t buf_size;

    /* hole counter */
    size_t hole_count;
};

struct ext_substmt_info_s {
    const char *name;
    const char *arg;
    int flags;
#define SUBST_FLAG_YIN 0x1 /**< has YIN element */
#define SUBST_FLAG_ID 0x2  /**< the value is identifier -> no quotes */
};

#define LLLY_PRINT_SET errno = 0

#define LLLY_PRINT_RET(ctx) if (errno) { LOGERR(ctx, LLLY_ESYS, "Print error (%s).", strerror(errno)); return EXIT_FAILURE; } else \
        { return EXIT_SUCCESS; }

/* filled in printer.c */
extern struct ext_substmt_info_s ext_substmt_info[];

/**
 * @brief Generic printer, replacement for printf() / write() / etc
 */
int llly_print(struct lllyout *out, const char *format, ...);
void llly_print_flush(struct lllyout *out);
int llly_write(struct lllyout *out, const char *buf, size_t count);
int llly_write_skip(struct lllyout *out, size_t count, size_t *position);
int llly_write_skipped(struct lllyout *out, size_t position, const char *buf, size_t count);

/* prefix_kind: 0 - print import prefixes for foreign features, 1 - print module names, 2 - print prefixes (tree printer), 3 - print module names including revisions (JSONS printer) */
int llly_print_iffeature(struct lllyout *out, const struct lllys_module *module, struct lllys_iffeature *expr, int prefix_kind);

int yang_print_model(struct lllyout *out, const struct lllys_module *module);
int yin_print_model(struct lllyout *out, const struct lllys_module *module);
int tree_print_model(struct lllyout *out, const struct lllys_module *module, const char *target_schema_path, int line_length, int options);
int info_print_model(struct lllyout *out, const struct lllys_module *module, const char *target_schema_path);
int jsons_print_model(struct lllyout *out, const struct lllys_module *module, const char *target_schema_path);

int json_print_data(struct lllyout *out, const struct lllyd_node *root, int options);
int xml_print_data(struct lllyout *out, const struct lllyd_node *root, int options);
int xml_print_node(struct lllyout *out, int level, const struct lllyd_node *node, int toplevel, int options);
int lllyb_print_data(struct lllyout *out, const struct lllyd_node *root, int options);

int lllys_print_target(struct lllyout *out, const struct lllys_module *module, const char *target_schema_path,
                     void (*clb_print_typedef)(struct lllyout*, const struct lllys_tpdf*, int*),
                     void (*clb_print_identity)(struct lllyout*, const struct lllys_ident*, int*),
                     void (*clb_print_feature)(struct lllyout*, const struct lllys_feature*, int*),
                     void (*clb_print_type)(struct lllyout*, const struct lllys_type*, int*),
                     void (*clb_print_grouping)(struct lllyout*, const struct lllys_node*, int*),
                     void (*clb_print_container)(struct lllyout*, const struct lllys_node*, int*),
                     void (*clb_print_choice)(struct lllyout*, const struct lllys_node*, int*),
                     void (*clb_print_leaf)(struct lllyout*, const struct lllys_node*, int*),
                     void (*clb_print_leaflist)(struct lllyout*, const struct lllys_node*, int*),
                     void (*clb_print_list)(struct lllyout*, const struct lllys_node*, int*),
                     void (*clb_print_anydata)(struct lllyout*, const struct lllys_node*, int*),
                     void (*clb_print_case)(struct lllyout*, const struct lllys_node*, int*),
                     void (*clb_print_notif)(struct lllyout*, const struct lllys_node*, int*),
                     void (*clb_print_rpc)(struct lllyout*, const struct lllys_node*, int*),
                     void (*clb_print_action)(struct lllyout*, const struct lllys_node*, int*),
                     void (*clb_print_input)(struct lllyout*, const struct lllys_node*, int*),
                     void (*clb_print_output)(struct lllyout*, const struct lllys_node*, int*));

/**
 * get know if the node is supposed to be printed (mostly according to the specified with-default mode)
 * return 1 - print, 0 - do not print
 */
int lllyd_toprint(const struct lllyd_node *node, int options);

/* 0 - same, 1 - different */
int nscmp(const struct lllyd_node *node1, const struct lllyd_node *node2);

#endif /* LLLY_PRINTER_H_ */
