/**
 * @file extensions.h
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief libyang support for YANG extension implementations.
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#ifndef LLLY_EXTENSIONS_H_
#define LLLY_EXTENSIONS_H_

#include "libyang.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup extensions
 * @{
 */

/**
 * @brief Extensions API version
 */
#define LLLYEXT_API_VERSION 1

/**
 * @brief Macro to store version of extension plugins API in the plugins.
 * It is matched when the plugin is being loaded by libyang.
 */
#ifdef STATIC
#define LLLYEXT_VERSION_CHECK
#else
#define LLLYEXT_VERSION_CHECK int lllyext_api_version = LLLYEXT_API_VERSION;
#endif

/**
 * @brief Extension instance structure parent enumeration
 */
typedef enum {
    LLLYEXT_PAR_MODULE,              /**< ::lllys_module or ::lllys_submodule */
    LLLYEXT_PAR_NODE,                /**< ::lllys_node (and the derived structures) */
    LLLYEXT_PAR_TPDF,                /**< ::lllys_tpdf */
    LLLYEXT_PAR_TYPE,                /**< ::lllys_type */
    LLLYEXT_PAR_TYPE_BIT,            /**< ::lllys_type_bit */
    LLLYEXT_PAR_TYPE_ENUM,           /**< ::lllys_type_enum */
    LLLYEXT_PAR_FEATURE,             /**< ::lllys_feature */
    LLLYEXT_PAR_RESTR,               /**< ::lllys_restr - YANG's must, range, length and pattern statements */
    LLLYEXT_PAR_WHEN,                /**< ::lllys_when */
    LLLYEXT_PAR_IDENT,               /**< ::lllys_ident */
    LLLYEXT_PAR_EXT,                 /**< ::lllys_ext */
    LLLYEXT_PAR_EXTINST,             /**< ::lllys_ext_instance */
    LLLYEXT_PAR_REFINE,              /**< ::lllys_refine */
    LLLYEXT_PAR_DEVIATION,           /**< ::lllys_deviation */
    LLLYEXT_PAR_DEVIATE,             /**< ::lllys_deviate */
    LLLYEXT_PAR_IMPORT,              /**< ::lllys_import */
    LLLYEXT_PAR_INCLUDE,             /**< ::lllys_include */
    LLLYEXT_PAR_REVISION,            /**< ::lllys_revision */
    LLLYEXT_PAR_IFFEATURE            /**< ::lllys_iffeature */
} LLLYEXT_PAR;

/**
 * @brief List of substatement without extensions storage. If the module contains extension instances in these
 * substatements, they are stored with the extensions of the parent statement and flag to show to which substatement
 * they belongs to.
 *
 * For example, if the extension is supposed to be instantiated as a child to the description statement, libyang
 * stores the description just as its value. So, for example in case of the module's description, the description's
 * extension instance is actually stored in the lllys_module's extensions list with the ::lllys_ext_instance#insubstmt set to
 * #LLLYEXT_SUBSTMT_DESCRIPTION, ::lllys_ext_instance#parent_type is LLLYEXT_PAR_MODULE and the ::lllys_ext_instance#parent
 * points to the ::lllys_module structure.
 *
 * The values are (convertible) subset of #LLLY_STMT
 */
typedef enum {
    LLLYEXT_SUBSTMT_ALL = -1,      /**< special value for the lllys_ext_iter() */
    LLLYEXT_SUBSTMT_SELF = 0,      /**< extension of the structure itself, not substatement's */
    LLLYEXT_SUBSTMT_ARGUMENT,      /**< extension of the argument statement, can appear in lllys_ext */
    LLLYEXT_SUBSTMT_BASE,          /**< extension of the base statement, can appear (repeatedly) in lllys_type and lllys_ident */
    LLLYEXT_SUBSTMT_BELONGSTO,     /**< extension of the belongs-to statement, can appear in lllys_submodule */
    LLLYEXT_SUBSTMT_CONTACT,       /**< extension of the contact statement, can appear in lllys_module */
    LLLYEXT_SUBSTMT_DEFAULT,       /**< extension of the default statement, can appear in lllys_node_leaf, lllys_node_leaflist,
                                      lllys_node_choice and lllys_deviate */
    LLLYEXT_SUBSTMT_DESCRIPTION,   /**< extension of the description statement, can appear in lllys_module, lllys_submodule,
                                      lllys_node, lllys_import, lllys_include, lllys_ext, lllys_feature, lllys_tpdf, lllys_restr,
                                      lllys_ident, lllys_deviation, lllys_type_enum, lllys_type_bit, lllys_when and lllys_revision */
    LLLYEXT_SUBSTMT_ERRTAG,        /**< extension of the error-app-tag statement, can appear in lllys_restr */
    LLLYEXT_SUBSTMT_ERRMSG,        /**< extension of the error-message statement, can appear in lllys_restr */
    LLLYEXT_SUBSTMT_KEY,           /**< extension of the key statement, can appear in lllys_node_list */
    LLLYEXT_SUBSTMT_NAMESPACE,     /**< extension of the namespace statement, can appear in lllys_module */
    LLLYEXT_SUBSTMT_ORGANIZATION,  /**< extension of the organization statement, can appear in lllys_module and lllys_submodule */
    LLLYEXT_SUBSTMT_PATH,          /**< extension of the path statement, can appear in lllys_type */
    LLLYEXT_SUBSTMT_PREFIX,        /**< extension of the prefix statement, can appear in lllys_module, lllys_submodule (for
                                      belongs-to's prefix) and lllys_import */
    LLLYEXT_SUBSTMT_PRESENCE,      /**< extension of the presence statement, can appear in lllys_node_container */
    LLLYEXT_SUBSTMT_REFERENCE,     /**< extension of the reference statement, can appear in lllys_module, lllys_submodule,
                                      lllys_node, lllys_import, lllys_include, lllys_revision, lllys_tpdf, lllys_restr, lllys_ident,
                                      lllys_ext, lllys_feature, lllys_deviation, lllys_type_enum, lllys_type_bit and lllys_when */
    LLLYEXT_SUBSTMT_REVISIONDATE,  /**< extension of the revision-date statement, can appear in lllys_import and lllys_include */
    LLLYEXT_SUBSTMT_UNITS,         /**< extension of the units statement, can appear in lllys_tpdf, lllys_node_leaf,
                                      lllys_node_leaflist and lllys_deviate */
    LLLYEXT_SUBSTMT_VALUE,         /**< extension of the value statement, can appear in lllys_type_enum */
    LLLYEXT_SUBSTMT_VERSION,       /**< extension of the yang-version statement, can appear in lllys_module and lllys_submodule */
    LLLYEXT_SUBSTMT_MODIFIER,      /**< extension of the modifier statement, can appear in lllys_restr */
    LLLYEXT_SUBSTMT_REQINSTANCE,   /**< extension of the require-instance statement, can appear in lllys_type */
    LLLYEXT_SUBSTMT_YINELEM,       /**< extension of the yin-element statement, can appear in lllys_ext */
    LLLYEXT_SUBSTMT_CONFIG,        /**< extension of the config statement, can appear in lllys_node and lllys_deviate */
    LLLYEXT_SUBSTMT_MANDATORY,     /**< extension of the mandatory statement, can appear in lllys_node_leaf, lllys_node_choice,
                                      lllys_node_anydata and lllys_deviate */
    LLLYEXT_SUBSTMT_ORDEREDBY,     /**< extension of the ordered-by statement, can appear in lllys_node_list and lllys_node_leaflist */
    LLLYEXT_SUBSTMT_STATUS,        /**< extension of the status statement, can appear in lllys_tpdf, lllys_node, lllys_ident,
                                      lllys_ext, lllys_feature, lllys_type_enum and lllys_type_bit */
    LLLYEXT_SUBSTMT_DIGITS,        /**< extension of the fraction-digits statement, can appear in lllys_type */
    LLLYEXT_SUBSTMT_MAX,           /**< extension of the max-elements statement, can appear in lllys_node_list,
                                      lllys_node_leaflist and lllys_deviate */
    LLLYEXT_SUBSTMT_MIN,           /**< extension of the min-elements statement, can appear in lllys_node_list,
                                      lllys_node_leaflist and lllys_deviate */
    LLLYEXT_SUBSTMT_POSITION,      /**< extension of the position statement, can appear in lllys_type_bit */
    LLLYEXT_SUBSTMT_UNIQUE,        /**< extension of the unique statement, can appear in lllys_node_list and lllys_deviate */
} LLLYEXT_SUBSTMT;

/**
 * @brief Callback to check that the extension can be instantiated inside the provided node
 *
 * @param[in] parent The parent of the instantiated extension.
 * @param[in] parent_type The type of the structure provided as \p parent.
 * @param[in] substmt_type libyang does not store all the extension instances in the structures where they are
 *                         instantiated in the module. In some cases (see #LLLYEXT_SUBSTMT) they are stored in parent
 *                         structure and marked with flag to know in which substatement of the parent the extension
 *                         was originally instantiated.
 * @return 0 - yes
 *         1 - no
 *         2 - ignore / skip without an error
 */
typedef int (*lllyext_check_position_clb)(const void *parent, LLLYEXT_PAR parent_type, LLLYEXT_SUBSTMT substmt_type);

/**
 * @brief Callback to check that the extension instance is correct - have
 * the valid argument, cardinality, etc.
 *
 * @param[in] ext Extension instance to be checked.
 * @return 0 - ok
 *         1 - error
 */
typedef int (*lllyext_check_result_clb)(struct lllys_ext_instance *ext);

/**
 * @brief Callback to decide whether the extension will be inherited into the provided schema node. The extension
 * instance is always from some of the node's parents. The inherited extension instances are marked with the
 * #LLLYEXT_OPT_INHERIT flag.
 *
 * @param[in] ext Extension instance to be inherited.
 * @param[in] node Schema node where the node is supposed to be inherited.
 * @return 0 - yes
 *         1 - no (do not process the node's children)
 *         2 - no, but continue with children
 */
typedef int (*lllyext_check_inherit_clb)(struct lllys_ext_instance *ext, struct lllys_node *node);

/**
 * @brief Callback to decide if data is valid towards to schema.
 *
 * @param[in] ext Extension instance to be checked.
 * @param[in] node Data node, which try to valid.
 *
 * @return 0 - valid
 *         1 - invalid
 */
typedef int (*lllyext_valid_data_clb)(struct lllys_ext_instance *ext, struct lllyd_node *node);

struct lllyext_plugin {
    LLLYEXT_TYPE type;                          /**< type of the extension, according to it the structure will be casted */
    uint16_t flags;                           /**< [extension flags](@ref extflags) */

    lllyext_check_position_clb check_position;  /**< callbcak for testing that the extension can be instantiated
                                                   under the provided parent. Mandatory callback. */
    lllyext_check_result_clb check_result;      /**< callback for testing if the argument value of the extension instance
                                                   is valid. Mandatory if the extension has the argument. */
    lllyext_check_inherit_clb check_inherit;    /**< callback to decide if the extension is supposed to be inherited into
                                                   the provided node, the callback is used only if the flags contains
                                                   #LLLYEXT_OPT_INHERIT flag */
    lllyext_valid_data_clb valid_data;          /**< callback to valid if data is valid toward to schema */
};

struct lllyext_plugin_complex {
    LLLYEXT_TYPE type;                          /**< type of the extension, according to it the structure will be casted */
    uint16_t flags;                           /**< [extension flags](@ref extflags) */

    lllyext_check_position_clb check_position;  /**< callbcak for testing that the extension can be instantiated
                                                   under the provided parent. Mandatory callback. */
    lllyext_check_result_clb check_result;      /**< callback for testing if the argument value of the extension instance
                                                   is valid. Mandatory if the extension has the argument. */
    lllyext_check_inherit_clb check_inherit;    /**< callback to decide if the extension is supposed to be inherited into
                                                   the provided node, the callback is used only if the flags contains
                                                   #LLLYEXT_OPT_INHERIT flag */
    lllyext_valid_data_clb valid_data;          /**< callback to valid if data is valid toward to schema */
    struct lllyext_substmt *substmt;            /**< NULL-terminated array of allowed substatements and restrictions
                                                   to their instantiation inside the extension instance */
    size_t instance_size;                     /**< size of the instance structure to allocate, the structure is
                                                   is provided as ::lllys_ext_instance_complex, but the content array
                                                   is accessed according to the substmt specification provided by
                                                   plugin */
};

struct lllyext_plugin_list {
    const char *module;          /**< name of the module where the extension is defined */
    const char *revision;        /**< optional module revision - if not specified, the plugin applies to any revision,
                                      which is not an optional approach due to a possible future revisions of the module.
                                      Instead, there should be defined multiple items in the plugins list, each with the
                                      different revision, but all with the same pointer to the plugin extension. The
                                      only valid use case for the NULL revision is the case the module has no revision. */
    const char *name;            /**< name of the extension */
    struct lllyext_plugin *plugin; /**< plugin for the extension */
};

/**
 * @brief Logging function for extension plugins, use #LLLYEXT_LOG macro instead!
 */
void lllyext_log(const struct llly_ctx *ctx, LLLY_LOG_LEVEL level, const char *plugin, const char *function, const char *format, ...);

/**
 * @brief Logging macro for extension plugins
 *
 * @param[in] ctx Context to store the error in.
 * @param[in] level #LLLY_LOG_LEVEL value with the message importance.
 * @param[in] plugin Plugin name.
 * @param[in] str Format string as in case of printf function.
 * @param[in] args Parameters to expand in format string.
 */
#define LLLYEXT_LOG(ctx, level, plugin, str, args...)       \
    lllyext_log(ctx, level, plugin, __func__, str, ##args); \

/**
 * @brief Type of object concerned by a validation error.
 * This is used to determine how to compute the path of the element at issue.
 */
typedef enum {
    LLLYEXT_VLOG_NONE = 0,
    LLLYEXT_VLOG_XML, /**< const struct ::lllyxml_elem* */
    LLLYEXT_VLOG_LYS, /**< const struct ::lllys_node* */
    LLLYEXT_VLOG_LYD, /**< const struct ::lllyd_node* */
    LLLYEXT_VLOG_STR, /**< const char* */
    LLLYEXT_VLOG_PREV, /**< Use the same path as the previous validation error */
} LLLYEXT_VLOG_ELEM;

/**
 * @brief Validation logging function for extension plugins, use #LLLYEXT_VLOG macro instead!
 */
void lllyext_vlog(const struct llly_ctx *ctx, LLLY_VECODE vecode, const char *plugin, const char *function,
                LLLYEXT_VLOG_ELEM elem_type, const void *elem, const char *format, ...);

/**
 * @brief Validation logging macro for extension plugins
 *
 * @param[in] ctx Context to store the error in.
 * @param[in] vecode #LLLY_VECODE validation error code.
 * @param[in] plugin Plugin name.
 * @param[in] elem_type #LLLYEXT_VLOG_ELEM what to expect in \p elem.
 * @param[in] elem The element at issue.
 * @param[in] str Format string as in case of printf function.
 * @param[in] args Parameters to expand in format string.
 */
#define LLLYEXT_VLOG(ctx, vecode, plugin, elem_type, elem, str, args...)    \
    lllyext_vlog(ctx, vecode, plugin, __func__, elem_type, elem, str, ##args)

/**
 * @brief Free iffeature structure. In API only for plugins that want to handle if-feature statements similarly
 * to libyang.
 *
 * @param[in] ctx libyang context.
 * @param[in] iffeature iffeature array to free.
 * @param[in] iffeature_size size of array \p iffeature.
 * @param[in] shallow Whether to make only shallow free.
 * @param[in] private_destructor Custom destructor for freeing any extension instances.
 */
void lllys_iffeature_free(struct llly_ctx *ctx, struct lllys_iffeature *iffeature, uint8_t iffeature_size, int shallow,
                        void (*private_destructor)(const struct lllys_node *node, void *priv));

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* LLLY_EXTENSIONS_H_ */
