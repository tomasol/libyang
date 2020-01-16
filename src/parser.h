/**
 * @file parser.h
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Parsers for libyang
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#ifndef LLLY_PARSER_H_
#define LLLY_PARSER_H_

#include <pcre.h>
#include <sys/mman.h>

#include "libyang.h"
#include "tree_schema.h"
#include "tree_internal.h"

#ifdef __APPLE__
# ifndef MAP_ANONYMOUS
#  define MAP_ANONYMOUS MAP_ANON
# endif
#endif

/**
 * @defgroup yin YIN format support
 * @{
 */
struct lllys_module *yin_read_module(struct llly_ctx *ctx, const char *data, const char *revision, int implement);
struct lllys_submodule *yin_read_submodule(struct lllys_module *module, const char *data, struct unres_schema *unres);

/**@} yin */

/**
 * @defgroup xmldata XML data format support
 * @{
 */
struct lllyd_node *xml_read_data(struct llly_ctx *ctx, const char *data, int options);

/**@} xmldata */

/**
 * @defgroup jsondata JSON data format support
 * @{
 */
struct lllyd_node *lllyd_parse_json(struct llly_ctx *ctx, const char *data, int options, const struct lllyd_node *rpc_act,
                                const struct lllyd_node *data_tree, const char *yang_data_name);

/**@} jsondata */

/**
 * @defgroup lllybdata LLLYB data format support
 * @{
 */
struct lllyd_node *lllyd_parse_lyb(struct llly_ctx *ctx, const char *data, int options, const struct lllyd_node *data_tree,
                               const char *yang_data_name, int *parsed);

/**@} lllybdata */

/**
 * internal options values for schema parsers
 */
#define LLLYS_PARSE_OPT_CFG_NOINHERIT 0x01 /**< do not inherit config flag */
#define LLLYS_PARSE_OPT_CFG_IGNORE    0x02 /**< ignore config flag (in rpc, actions, notifications) */
#define LLLYS_PARSE_OPT_CFG_MASK      0x03
#define LLLYS_PARSE_OPT_INGRP         0x04 /**< flag to know that parser is inside a grouping */

/* list of YANG statement strings */
extern const char *llly_stmt_str[];

enum LLLY_IDENT {
    LLLY_IDENT_SIMPLE,   /* only syntax rules */
    LLLY_IDENT_FEATURE,
    LLLY_IDENT_IDENTITY,
    LLLY_IDENT_TYPE,
    LLLY_IDENT_NODE,
    LLLY_IDENT_NAME,     /* uniqueness across the siblings */
    LLLY_IDENT_PREFIX,
    LLLY_IDENT_EXTENSION
};
int lllyp_yin_fill_ext(void *parent, LLLYEXT_PAR parent_type, LLLYEXT_SUBSTMT substmt, uint8_t substmt_index,
                     struct lllys_module *module, struct lllyxml_elem *yin, struct lllys_ext_instance ***ext,
                     uint8_t ext_index, struct unres_schema *unres);

int lllyp_yin_parse_complex_ext(struct lllys_module *mod, struct lllys_ext_instance_complex *ext,
                              struct lllyxml_elem *yin, struct unres_schema *unres);
int lllyp_yin_parse_subnode_ext(struct lllys_module *mod, void *elem, LLLYEXT_PAR elem_type,
                              struct lllyxml_elem *yin, LLLYEXT_SUBSTMT type, uint8_t i, struct unres_schema *unres);

struct lllys_type *lllyp_get_next_union_type(struct lllys_type *type, struct lllys_type *prev_type, int *found);

/* return: 0 - ret set, ok; 1 - ret not set, no log, unknown meta; -1 - ret not set, log, fatal error */
int lllyp_fill_attr(struct llly_ctx *ctx, struct lllyd_node *parent, const char *module_ns, const char *module_name,
                  const char *attr_name, const char *attr_value, struct lllyxml_elem *xml, int options, struct lllyd_attr **ret);

int lllyp_check_edit_attr(struct llly_ctx *ctx, struct lllyd_attr *attr, struct lllyd_node *parent, int *editbits);

struct lllys_type *lllyp_parse_value(struct lllys_type *type, const char **value_, struct lllyxml_elem *xml,
                                 struct lllyd_node_leaf_list *leaf, struct lllyd_attr *attr, struct lllys_module *local_mod,
                                 int store, int dflt, int trusted);

int lllyp_check_length_range(struct llly_ctx *ctx, const char *expr, struct lllys_type *type);

int lllyp_check_pattern(struct llly_ctx *ctx, const char *pattern, pcre **pcre_precomp);
int lllyp_precompile_pattern(struct llly_ctx *ctx, const char *pattern, pcre** pcre_cmp, pcre_extra **pcre_std);

int fill_yin_type(struct lllys_module *module, struct lllys_node *parent, struct lllyxml_elem *yin, struct lllys_type *type,
                  int tpdftype, struct unres_schema *unres);

int lllyp_check_status(uint16_t flags1, struct lllys_module *mod1, const char *name1,
                     uint16_t flags2, struct lllys_module *mod2, const char *name2,
                     const struct lllys_node *node);

void lllyp_del_includedup(struct lllys_module *mod, int free_subs);

int dup_typedef_check(const char *type, struct lllys_tpdf *tpdf, int size);

int dup_identities_check(const char *id, struct lllys_module *module);

/**
 * @brief Get know if the node is part of the RPC/action's input/output
 *
 * @param node Schema node to be examined.
 * @return 1 for true, 0 for false
 */
int lllyp_is_rpc_action(struct lllys_node *node);

/**
 * @brief Check validity of data parser options.
 *
 * @param options Parser options to be checked.
 * @param func name of the function where called
 * @return 0 for ok, 1 when multiple data types bits are set, or incompatible options are used together.
 */
int lllyp_data_check_options(struct llly_ctx *ctx, int options, const char *func);

int lllyp_check_identifier(struct llly_ctx *ctx, const char *id, enum LLLY_IDENT type, struct lllys_module *module, struct lllys_node *parent);
int lllyp_check_date(struct llly_ctx *ctx, const char *date);
int lllyp_check_mandatory_augment(struct lllys_node_augment *node, const struct lllys_node *target);
int lllyp_check_mandatory_choice(struct lllys_node *node);

int lllyp_check_include(struct lllys_module *module, const char *value,
                      struct lllys_include *inc, struct unres_schema *unres);
int lllyp_check_include_missing(struct lllys_module *main_module);
int lllyp_check_import(struct lllys_module *module, const char *value, struct lllys_import *imp);
int lllyp_check_circmod_add(struct lllys_module *module);
void lllyp_check_circmod_pop(struct llly_ctx *ctx);

void lllyp_sort_revisions(struct lllys_module *module);
int lllyp_rfn_apply_ext(struct lllys_module *module);
int lllyp_deviation_apply_ext(struct lllys_module *module);
int lllyp_mand_check_ext(struct lllys_ext_instance_complex *ext, const char *ext_name);

const char *lllyp_get_yang_data_template_name(const struct lllyd_node *node);
const struct lllys_node *lllyp_get_yang_data_template(const struct lllys_module *module, const char *yang_data_name, int yang_data_name_len);

void lllyp_ext_instance_rm(struct llly_ctx *ctx, struct lllys_ext_instance ***ext, uint8_t *size, uint8_t index);

/**
 * @brief Propagate imports and includes into the main module
 *
 * @param module Main module
 * @param inc Filled include structure
 * @return 0 for success, 1 for failure
 */
int lllyp_propagate_submodule(struct lllys_module *module, struct lllys_include *inc);

/* return: -1 = error, 0 = success, 1 = already there (if it was disabled, it is enabled first) */
int lllyp_ctx_check_module(struct lllys_module *module);

int lllyp_ctx_add_module(struct lllys_module *module);

/**
 * @brief Add annotations definitions of attributes and URL config used in ietf-netconf RPCs.
 */
int lllyp_add_ietf_netconf_annotations_config(struct lllys_module *mod);

/**
 * @brief mmap() wrapper for parsers. To unmap, use lllyp_munmap().
 *
 * @param[in] prot The desired memory protection as in case of mmap().
 * @param[in] fd File descriptor for getting data.
 * @param[in] addsize Number of additional bytes to be allocated (and zeroed) after the implicitly added
 *                    string-terminating NULL byte.
 * @param[out] length length of the allocated memory.
 * @param[out] addr Pointer to the memory where the file data is mapped.
 * @return 0 on success, non-zero on error.
 */
int lllyp_mmap(struct llly_ctx *ctx, int fd, size_t addsize, size_t *length, void **addr);

/**
 * @brief Unmap function for the data mapped by lllyp_mmap()
 */
int lllyp_munmap(void *addr, size_t length);

/**
 * Store UTF-8 character specified as 4byte integer into the dst buffer.
 * Returns number of written bytes (4 max), expects that dst has enough space.
 *
 * UTF-8 mapping:
 * 00000000 -- 0000007F:    0xxxxxxx
 * 00000080 -- 000007FF:    110xxxxx 10xxxxxx
 * 00000800 -- 0000FFFF:    1110xxxx 10xxxxxx 10xxxxxx
 * 00010000 -- 001FFFFF:    11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
 *
 */
unsigned int pututf8(struct llly_ctx *ctx, char *dst, int32_t value);
unsigned int copyutf8(struct llly_ctx *ctx, char *dst, const char *src);

/**
 * @brief Find a module. First, imports from \p module with matching \p prefix, \p name, or both are checked,
 * \p module itself is also compared, and lastly a callback is used if allowed.
 *
 * @param[in] module Module with imports.
 * @param[in] prefix Module prefix to search for.
 * @param[in] pref_len Module \p prefix length. If 0, the whole prefix is used, if not NULL.
 * @param[in] name Module name to search for.
 * @param[in] name_len Module \p name length. If 0, the whole name is used, if not NULL.
 * @param[in] in_data Whether to use data callback if not found after trying all the rest.
 * Import callback is never used because there is no use-case for that.
 *
 * @return Matching module, NULL if not found.
 */
const struct lllys_module *lllyp_get_module(const struct lllys_module *module, const char *prefix, int pref_len,
                                               const char *name, int name_len, int in_data);

/**
 * @brief Find an import from \p module with matching namespace, the \p module itself is also considered.
 *
 * @param[in] module Module with imports.
 * @param[in] ns Namespace to be found.
 */
const struct lllys_module *lllyp_get_import_module_ns(const struct lllys_module *module, const char *ns);

/*
 * Internal functions implementing YANG (extension and user type) plugin support
 * - implemented in plugins.c
 */

/**
 * @brief If available, get the extension plugin for the specified extension
 *
 * @param[in] name Name of the extension
 * @param[in] module Name of the extension's module
 * @param[in] revision Revision of the extension's module
 * @return pointer to the extension plugin structure, NULL if no plugin available
 */
struct lllyext_plugin *ext_get_plugin(const char *name, const char *module, const char *revision);

/**
 * @brief Try to store a value as a user type defined by a plugin.
 *
 * @param[in] mod Module of the type.
 * @param[in] type_name Type (typedef) name.
 * @param[in,out] value_str Stored string value, can be overwritten by the user store callback.
 * @param[in,out] value Filled value to be overwritten by the user store callback.
 * @return 0 on successful storing, 1 if the type is not a user type, -1 on error.
 */
int lllytype_store(const struct lllys_module *mod, const char *type_name, const char **value_str, lllyd_val *value);

/**
 * @brief Free a user type stored value.
 *
 * @param[in] type Type of the value.
 * @param[in] value Value union to free.
 * @param[in] value_str String value of the value.
 */
void lllytype_free(const struct lllys_type *type, lllyd_val value, const char *value_str);

#endif /* LLLY_PARSER_H_ */
