/**
 * @file parser_yang.h
 * @author Pavol Vican
 * @brief Parsers for libyang
 *
 * Copyright (c) 2015 - 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#ifndef LLLY_PARSER_YANG_H_
#define LLLY_PARSER_YANG_H_

#include <stdlib.h>
#include <string.h>

#include "libyang.h"
#include "resolve.h"
#include "common.h"
#include "tree_schema.h"
#include "context.h"

#define LLLYS_SYSTEMORDERED 0x40
#define LLLYS_ORDERED_MASK 0xC0
#define LLLYS_MIN_ELEMENTS 0x01
#define LLLYS_MAX_ELEMENTS 0x02
#define LLLYS_RPC_INPUT 0x01
#define LLLYS_RPC_OUTPUT 0x02
#define LLLYS_DATADEF 0x04
#define LLLYS_TYPE_DEF 0x08
#define LLLYS_CHOICE_DEFAULT 0x10
#define LLLYS_NO_ERASE_IDENTITY 0x20
#define LLLY_YANG_ARRAY_SIZE 8
#define YANG_REMOVE_IMPORT 0x01
#define YANG_EXIST_MODULE 0x02
#define EXT_INSTANCE_SUBSTMT 0x04

struct type_node {
    union {
        struct lllys_node_leaflist *ptr_leaflist;
        struct lllys_node_list *ptr_list;
        struct lllys_node_leaf *ptr_leaf;
        struct lllys_tpdf *ptr_tpdf;
        struct lllys_node_anydata *ptr_anydata;
        struct lllys_node_rpc_action *ptr_rpc;
        struct lllys_node_choice *ptr_choice;
    };
    uint flag;
};

struct yang_parameter {
    struct lllys_module *module;
    struct lllys_submodule *submodule;
    struct unres_schema *unres;
    struct lllys_node **node;
    char **value;
    void **data_node;
    void **actual_node;
    uint8_t flags;
};

struct yang_ext_substmt {
    char *ext_substmt;  /* pointer to string, which contains substmts without module statement */
    char **ext_modules; /* array of char *, which contains module statements */
};

struct yang_type {
    char flags;       /**< this is used to distinguish lllyxml_elem * from a YANG temporary parsing structure */
    LLLY_DATA_TYPE base;
    const char *name;
    struct lllys_type *type;
};

#include "parser_yang_bis.h"

char *yang_read_string(struct llly_ctx *ctx, const char *input, char *output, int size, int offset, int indent);

int yang_read_common(struct lllys_module *module,char *value, enum yytokentype type);

int yang_read_prefix(struct lllys_module *module, struct lllys_import *imp, char *value);

int yang_check_version(struct lllys_module *module, struct lllys_submodule *submodule, char *value, int repeat);

int yang_check_imports(struct lllys_module *module, struct unres_schema *unres);

int yang_read_description(struct lllys_module *module, void *node, char *value, char *where, enum yytokentype type);

int yang_read_reference(struct lllys_module *module, void *node, char *value, char *where, enum yytokentype type);

int yang_read_message(struct lllys_module *module,struct lllys_restr *save,char *value, char *what, int message);

int yang_read_presence(struct lllys_module *module, struct lllys_node_container *cont, char *value);

int yang_read_config(void *node, int value, enum yytokentype type);

void *yang_read_when(struct lllys_module *module, struct lllys_node *node, enum yytokentype type, char *value);

/**
 * @brief Allocate memory for node and add to the tree
 *
 * @param[in/out] node Pointer to the array.
 * @param[in] parent Pointer to the parent.
 * @param[in] root Pointer to the root of schema tree.
 * @param[in] value Name of node
 * @param[in] nodetype Type of node
 * @param[in] sizeof_struct Size of struct
 * @return Pointer to the node, NULL on error.
*/
void *yang_read_node(struct lllys_module *module, struct lllys_node *parent, struct lllys_node **root,
                     char *value, int nodetype, int sizeof_struct);

int yang_read_default(struct lllys_module *module, void *node, char *value, enum yytokentype type);

int yang_read_units(struct lllys_module *module, void *node, char *value, enum yytokentype type);

int yang_read_key(struct lllys_module *module, struct lllys_node_list *list, struct unres_schema *unres);

int yang_read_unique(struct lllys_module *module, struct lllys_node_list *list, struct unres_schema *unres);

void *yang_read_type(struct llly_ctx *ctx, void *parent, char *value, enum yytokentype type);

void *yang_read_length(struct llly_ctx *ctx, struct yang_type *stype, char *value, int is_ext_instance);

int yang_check_type(struct lllys_module *module, struct lllys_node *parent, struct yang_type *typ, struct lllys_type *type, int tpdftype, struct unres_schema *unres);

int yang_fill_type(struct lllys_module *module, struct lllys_type *type, struct yang_type *stype,
                   void *parent, struct unres_schema *unres);

void yang_free_type_union(struct llly_ctx *ctx, struct lllys_type *type);

void yang_type_free(struct llly_ctx *ctx, struct lllys_type *type);

int yang_read_leafref_path(struct lllys_module *module, struct yang_type *stype, char *value);

int yang_read_require_instance(struct llly_ctx *ctx, struct yang_type *stype, int req);

int yang_read_pattern(struct llly_ctx *ctx, struct lllys_restr *pattern, void **precomp, char *value, char modifier);

void *yang_read_range(struct llly_ctx *ctx, struct yang_type *stype, char *value, int is_ext_instance);

int yang_read_fraction(struct llly_ctx *ctx, struct yang_type *typ, uint32_t value);

int yang_read_enum(struct llly_ctx *ctx, struct yang_type *typ, struct lllys_type_enum *enm, char *value);

int yang_check_enum(struct llly_ctx *ctx, struct yang_type *typ, struct lllys_type_enum *enm, int64_t *value, int assign);

int yang_read_bit(struct llly_ctx *ctx, struct yang_type *typ, struct lllys_type_bit *bit, char *value);

int yang_check_bit(struct llly_ctx *ctx, struct yang_type *typ, struct lllys_type_bit *bit, int64_t *value, int assign);

void *yang_read_typedef(struct lllys_module *module, struct lllys_node *parent, char *value);

int yang_read_augment(struct lllys_module *module, struct lllys_node *parent, struct lllys_node_augment *aug, char *value);

void *yang_read_deviate(struct llly_ctx *ctx, struct lllys_deviation *dev, LLLYS_DEVIATE_TYPE mod);

void *yang_read_deviate_unsupported(struct llly_ctx *ctx, struct lllys_deviation *dev);

int yang_fill_unique(struct lllys_module *module, struct lllys_node_list *list, struct lllys_unique *unique, char *value, struct unres_schema *unres);

int yang_fill_iffeature(struct lllys_module *module, struct lllys_iffeature *iffeature, void *parent,
                        char *value, struct unres_schema *unres, int parent_is_feature);

void yang_free_ext_data(struct yang_ext_substmt *substmt);

void *yang_read_ext(struct lllys_module *module, void *actual, char *ext_name, char *ext_arg,
                    enum yytokentype actual_type, enum yytokentype backup_type, int is_ext_instance);

int yang_check_ext_instance(struct lllys_module *module, struct lllys_ext_instance ***ext, uint size,
                            void *parent, struct unres_schema *unres);

int yang_read_extcomplex_str(struct lllys_module *module, struct lllys_ext_instance_complex *ext, const char *arg_name,
                             const char *parent_name, char **value, int parent_stmt, LLLY_STMT stmt);

void **yang_getplace_for_extcomplex_struct(struct lllys_ext_instance_complex *ext, int *index,
                                    char *parent_name, char *node_name, LLLY_STMT stmt);

int yang_extcomplex_node(struct lllys_ext_instance_complex *ext, char *parent_name, char *node_name,
                         struct lllys_node *node, LLLY_STMT stmt);

int yang_fill_extcomplex_flags(struct lllys_ext_instance_complex *ext, char *parent_name, char *node_name,
                               LLLY_STMT stmt, uint16_t value, uint16_t mask);

int yang_fill_extcomplex_uint8(struct lllys_ext_instance_complex *ext, char *parent_name, char *node_name,
                               LLLY_STMT stmt, uint8_t value);

int yang_parse_ext_substatement(struct lllys_module *module, struct unres_schema *unres, const char *data,
                                char *ext_name, struct lllys_ext_instance_complex *ext);

int yang_fill_extcomplex_module(struct llly_ctx *ctx, struct lllys_ext_instance_complex *ext,
                                char *parent_name, char **values, int implemented);


/* **
 * @brief Parse YANG from in-memory string
 *
 * yang parser expected at the end of the input string 2 zero byte
 *
 * @param[in] module Pointer to the libyang module.
 * @param[in] submodule Pointer to the libyang submodule.
 * @param[in] unres Pointer to a unres_schema
 * @param[in] data Pointer to a NULL-terminated string containing YANG data to parse.
 * @param[in] size_data Size of input string
 * @param[in/out] node Pointer to node
 * @return 0 on success, -1 on error, 1 on module is already in context.
 */
int yang_parse_mem(struct lllys_module *module, struct lllys_submodule *submodule, struct unres_schema *unres,
                   const char *data, unsigned int size_data, struct lllys_node **node);

struct lllys_module *yang_read_module(struct llly_ctx *ctx, const char* data, unsigned int size, const char *revision, int implement);

struct lllys_submodule *yang_read_submodule(struct lllys_module *module, const char *data, unsigned int size, struct unres_schema *unres);

#endif /* LLLY_PARSER_YANG_H_ */
