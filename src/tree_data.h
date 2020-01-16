/**
 * @file tree_data.h
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief libyang representation of data trees.
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#ifndef LLLY_TREE_DATA_H_
#define LLLY_TREE_DATA_H_

#include <stddef.h>
#include <stdint.h>

#include "libyang.h"
#include "tree_schema.h"
#include "xml.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup datatree Data Tree
 * @{
 *
 * Data structures and functions to manipulate and access instance data tree.
 */

/**
 * @brief Data input/output formats supported by libyang [parser](@ref howtodataparsers) and
 * [printer](@ref howtodataprinters) functions.
 */
typedef enum {
    LLLYD_UNKNOWN,         /**< unknown format, used as return value in case of error */
    LLLYD_XML,             /**< XML format of the instance data */
    LLLYD_JSON,            /**< JSON format of the instance data */
    LLLYD_LYB,             /**< LLLYB format of the instance data */
} LLLYD_FORMAT;

/**
 * @brief List of possible value types stored in ::lllyd_node_anydata.
 */
typedef enum {
    LLLYD_ANYDATA_CONSTSTRING = 0x00, /**< value is constant string (const char *) which is internally duplicated for
                                         storing in the anydata structure; XML sensitive characters (such as & or \>)
                                         are automatically escaped when the anydata is printed in XML format. */
    LLLYD_ANYDATA_STRING = 0x01,      /**< value is dynamically allocated string (char*), so the data are used directly
                                         without duplication and caller is supposed to not manipulate with the data
                                         after a successful call (including calling free() on the provided data); XML
                                         sensitive characters (such as & or \>) are automatically escaped when the
                                         anydata is printed in XML format */
    LLLYD_ANYDATA_JSON = 0x02,        /**< value is string containing the data modeled by YANG and encoded as I-JSON. The
                                         string is handled as constant string. In case of using the value as input
                                         parameter, the #LLLYD_ANYDATA_JSOND can be used for dynamically allocated
                                         string. */
    LLLYD_ANYDATA_JSOND = 0x03,       /**< In case of using value as input parameter, this enumeration is supposed to be
                                         used for dynamically allocated strings (it is actually combination of
                                         #LLLYD_ANYDATA_JSON and #LLLYD_ANYDATA_STRING (and it can be also specified as
                                         ORed value of the mentioned values. */
    LLLYD_ANYDATA_SXML = 0x04,        /**< value is string containing the serialized XML data. The string is handled as
                                         constant string. In case of using the value as input parameter, the
                                         #LLLYD_ANYDATA_SXMLD can be used for dynamically allocated string. */
    LLLYD_ANYDATA_SXMLD = 0x05,       /**< In case of using serialized XML value as input parameter, this enumeration is
                                         supposed to be used for dynamically allocated strings (it is actually
                                         combination of #LLLYD_ANYDATA_SXML and #LLLYD_ANYDATA_STRING (and it can be also
                                         specified as ORed value of the mentioned values). */
    LLLYD_ANYDATA_XML = 0x08,         /**< value is struct lllyxml_elem*, the structure is directly connected into the
                                         anydata node without duplication, caller is supposed to not manipulate with the
                                         data after a successful call (including calling lllyxml_free() on the provided
                                         data) */
    LLLYD_ANYDATA_DATATREE = 0x10,    /**< value is struct lllyd_node* (first sibling), the structure is directly connected
                                         into the anydata node without duplication, caller is supposed to not manipulate
                                         with the data after a successful call (including calling lllyd_free() on the
                                         provided data) */
    LLLYD_ANYDATA_LYB = 0x20,         /**< value is a memory with serialized data tree in LLLYB format. The data are handled
                                         as a constant string. In case of using the value as input parameter,
                                         the #LLLYD_ANYDATA_LYBD can be used for dynamically allocated string. */
    LLLYD_ANYDATA_LYBD = 0x21,        /**< In case of using LLLYB value as input parameter, this enumeration is
                                         supposed to be used for dynamically allocated strings (it is actually
                                         combination of #LLLYD_ANYDATA_LYB and #LLLYD_ANYDATA_STRING (and it can be also
                                         specified as ORed value of the mentioned values). */
} LLLYD_ANYDATA_VALUETYPE;

/**
 * @brief node's value representation
 */
typedef union lllyd_value_u {
    const char *binary;          /**< base64 encoded, NULL terminated string */
    struct lllys_type_bit **bit;   /**< bitmap of pointers to the schema definition of the bit value that are set,
                                      its size is always the number of defined bits in the schema */
    int8_t bln;                  /**< 0 as false, 1 as true */
    int64_t dec64;               /**< decimal64: value = dec64 / 10^fraction-digits  */
    struct lllys_type_enum *enm;   /**< pointer to the schema definition of the enumeration value */
    struct lllys_ident *ident;     /**< pointer to the schema definition of the identityref value */
    struct lllyd_node *instance;   /**< pointer to the instance-identifier target, note that if the tree was modified,
                                      the target (address) can be invalid - the pointer is correctly checked and updated
                                      by lllyd_validate() */
    int8_t int8;                 /**< 8-bit signed integer */
    int16_t int16;               /**< 16-bit signed integer */
    int32_t int32;               /**< 32-bit signed integer */
    int64_t int64;               /**< 64-bit signed integer */
    struct lllyd_node *leafref;    /**< pointer to the referenced leaf/leaflist instance in data tree */
    const char *string;          /**< string */
    uint8_t uint8;               /**< 8-bit unsigned integer */
    uint16_t uint16;             /**< 16-bit signed integer */
    uint32_t uint32;             /**< 32-bit signed integer */
    uint64_t uint64;             /**< 64-bit signed integer */
    void *ptr;                   /**< arbitrary data stored using a type plugin */
} lllyd_val;

/**
 * @brief Attribute structure.
 *
 * The structure provides information about attributes of a data element. Such attributes must map to
 * annotations as specified in RFC 7952. The only exception is the filter type (in NETCONF get operations)
 * and edit-config's operation attributes. In XML, they are represented as standard XML attributes. In JSON,
 * they are represented as JSON elements starting with the '@' character (for more information, see the
 * YANG metadata RFC.
 *
 */
struct lllyd_attr {
    struct lllyd_node *parent;         /**< data node where the attribute is placed */
    struct lllyd_attr *next;           /**< pointer to the next attribute of the same element */
    struct lllys_ext_instance_complex *annotation; /**< pointer to the attribute/annotation's definition */
    const char *name;                /**< attribute name */
    const char *value_str;           /**< string representation of value (for comparison, printing,...), always corresponds to value_type */
    lllyd_val value;                   /**< node's value representation, always corresponds to schema->type.base */
    LLLY_DATA_TYPE _PACKED value_type; /**< type of the value in the node, mainly for union to avoid repeating of type detection */
    uint8_t value_flags;             /**< value type flags */
};

/**
 * @defgroup validityflags Validity flags
 * @ingroup datatree
 *
 * Validity flags for data nodes.
 *
 * @{
 */
#define LLLYD_VAL_OK       0x00    /**< Node is successfully validated including whole subtree */
#define LLLYD_VAL_DUP      0x01    /**< Instance duplication must be checked again, applicable only to ::lllys_node_list and
                                      ::lllys_node_leaf_list data nodes */
#define LLLYD_VAL_UNIQUE   0x02    /**< Unique value(s) changed, applicable only to ::lllys_node_list data nodes */
#define LLLYD_VAL_MAND     0x04    /**< Some child added/removed and it is needed to perform check for mandatory
                                      node or min/max constraints of direct list/leaflist children, applicable only
                                      to ::lllys_node_list and ::lllys_node_container data nodes, but if on any other node
                                      except ::lllys_node_leaflist, it means checking that data node for duplicities.
                                      Additionally, it can be set on truly any node type and then status references
                                      are checked for this node if flag #LLLYD_OPT_OBSOLETE is used. */
#define LLLYD_VAL_LEAFREF  0x08    /**< Node is a leafref, which needs to be resolved (it is invalid, new possible
                                      resolvent, or something similar) */
#define LLLYD_VAL_INUSE    0x80    /**< Internal flag for note about various processing on data, should be used only
                                      internally and removed before libyang returns the node to the caller */
/**
 * @}
 */

/**
 * @brief Generic structure for a data node, directly applicable to the data nodes defined as #LLLYS_CONTAINER, #LLLYS_LIST
 * and #LLLYS_CHOICE.
 *
 * Completely fits to containers and choices and is compatible (can be used interchangeably except the #child member)
 * with all other lllyd_node_* structures. All data nodes are provides as ::lllyd_node structure by default.
 * According to the schema's ::lllys_node#nodetype member, the specific object is supposed to be cast to
 * ::lllyd_node_leaf_list or ::lllyd_node_anydata structures. This structure fits only to #LLLYS_CONTAINER, #LLLYS_LIST and
 * #LLLYS_CHOICE values.
 *
 * To traverse all the child elements or attributes, use #LLLY_TREE_FOR or #LLLY_TREE_FOR_SAFE macro. To traverse
 * the whole subtree, use #LLLY_TREE_DFS_BEGIN macro.
 */
struct lllyd_node {
    struct lllys_node *schema;         /**< pointer to the schema definition of this node */
    uint8_t validity;                /**< [validity flags](@ref validityflags) */
    uint8_t dflt:1;                  /**< flag for implicit default node */
    uint8_t when_status:3;           /**< bit for checking if the when-stmt condition is resolved - internal use only,
                                          do not use this value! */

    struct lllyd_attr *attr;           /**< pointer to the list of attributes of this node */
    struct lllyd_node *next;           /**< pointer to the next sibling node (NULL if there is no one) */
    struct lllyd_node *prev;           /**< pointer to the previous sibling node \note Note that this pointer is
                                          never NULL. If there is no sibling node, pointer points to the node
                                          itself. In case of the first node, this pointer points to the last
                                          node in the list. */
    struct lllyd_node *parent;         /**< pointer to the parent node, NULL in case of root node */

#ifdef LLLY_ENABLED_LYD_PRIV
    void *priv;                      /**< private user data, not used by libyang */
#endif

#ifdef LLLY_ENABLED_CACHE
    uint32_t hash;                   /**< hash of this particular node (module name + schema name + key string values if list) */
    struct hash_table *ht;           /**< hash table with all the direct children (except keys for a list, lists without keys) */
#endif

    struct lllyd_node *child;          /**< pointer to the first child node \note Since other lllyd_node_*
                                          structures represent end nodes, this member
                                          is replaced in those structures. Therefore, be careful with accessing
                                          this member without having information about the node type from the schema's
                                          ::lllys_node#nodetype member. */
};

/**
 * @brief Structure for data nodes defined as #LLLYS_LEAF or #LLLYS_LEAFLIST.
 *
 * Extension for ::lllyd_node structure. It replaces the ::lllyd_node#child member by
 * three new members (#value, #value_str and #value_type) to provide
 * information about the value. The first five members (#schema, #attr, #next,
 * #prev and #parent) are compatible with the ::lllyd_node's members.
 *
 * To traverse through all the child elements or attributes, use #LLLY_TREE_FOR or #LLLY_TREE_FOR_SAFE macro.
 */
struct lllyd_node_leaf_list {
    struct lllys_node *schema;         /**< pointer to the schema definition of this node which is ::lllys_node_leaflist
                                          structure */
    uint8_t validity;                /**< [validity flags](@ref validityflags) */
    uint8_t dflt:1;                  /**< flag for implicit default node */
    uint8_t when_status:3;           /**< bit for checking if the when-stmt condition is resolved - internal use only,
                                          do not use this value! */

    struct lllyd_attr *attr;           /**< pointer to the list of attributes of this node */
    struct lllyd_node *next;           /**< pointer to the next sibling node (NULL if there is no one) */
    struct lllyd_node *prev;           /**< pointer to the previous sibling node \note Note that this pointer is
                                          never NULL. If there is no sibling node, pointer points to the node
                                          itself. In case of the first node, this pointer points to the last
                                          node in the list. */
    struct lllyd_node *parent;         /**< pointer to the parent node, NULL in case of root node */

#ifdef LLLY_ENABLED_LYD_PRIV
    void *priv;                      /**< private user data, not used by libyang */
#endif

#ifdef LLLY_ENABLED_CACHE
    uint32_t hash;                   /**< hash of this particular node (module name + schema name + string value if leaf-list) */
#endif

    /* struct lllyd_node *child; should be here, but is not */

    /* leaflist's specific members */
    const char *value_str;           /**< string representation of value (for comparison, printing,...), always corresponds to value_type */
    lllyd_val value;                   /**< node's value representation, always corresponds to schema->type.base */
    LLLY_DATA_TYPE _PACKED value_type; /**< type of the value in the node, mainly for union to avoid repeating of type detection */
    uint8_t value_flags;             /**< value type flags */
};

/**
 * @brief Flags for values
 */
#define LLLY_VALUE_UNRES 0x01   /**< flag for unresolved leafref or instance-identifier,
                                   leafref - value union is filled as if being the target node's type,
                                   instance-identifier - value union should not be accessed */
#define LLLY_VALUE_USER 0x02    /**< flag for a user type stored value */
/* 0x80 is reserved for internal use */

/**
 * @brief Anydata value union
 */
typedef union {
    const char *str;             /**< string value, in case of printing as XML, characters like '<' or '&' are escaped */
    char *mem;                   /**< raw memory (used for LLLYB format) */
    struct lllyxml_elem *xml;      /**< xml tree */
    struct lllyd_node *tree;       /**< libyang data tree, does not change the root's parent, so it is not possible
                                      to get from the data tree into the anydata/anyxml */
} lllyd_anydata_value;

/**
 * @brief Structure for data nodes defined as #LLLYS_ANYDATA or #LLLYS_ANYXML.
 *
 * Extension for ::lllyd_node structure - replaces the ::lllyd_node#child member by new #value member. The first five
 * members (#schema, #attr, #next, #prev and #parent) are compatible with the ::lllyd_node's members.
 *
 * To traverse through all the child elements or attributes, use #LLLY_TREE_FOR or #LLLY_TREE_FOR_SAFE macro.
 */
struct lllyd_node_anydata {
    struct lllys_node *schema;         /**< pointer to the schema definition of this node which is ::lllys_node_anydata
                                          structure */
    uint8_t validity;                /**< [validity flags](@ref validityflags) */
    uint8_t dflt:1;                  /**< flag for implicit default node */
    uint8_t when_status:3;           /**< bit for checking if the when-stmt condition is resolved - internal use only,
                                          do not use this value! */

    struct lllyd_attr *attr;           /**< pointer to the list of attributes of this node */
    struct lllyd_node *next;           /**< pointer to the next sibling node (NULL if there is no one) */
    struct lllyd_node *prev;           /**< pointer to the previous sibling node \note Note that this pointer is
                                          never NULL. If there is no sibling node, pointer points to the node
                                          itself. In case of the first node, this pointer points to the last
                                          node in the list. */
    struct lllyd_node *parent;         /**< pointer to the parent node, NULL in case of root node */

#ifdef LLLY_ENABLED_LYD_PRIV
    void *priv;                      /**< private user data, not used by libyang */
#endif

#ifdef LLLY_ENABLED_CACHE
    uint32_t hash;                   /**< hash of this particular node (module name + schema name) */
#endif

    /* struct lllyd_node *child; should be here, but is not */

    /* anyxml's specific members */
    LLLYD_ANYDATA_VALUETYPE value_type;/**< type of the stored anydata value */
    lllyd_anydata_value value;/**< stored anydata value */
};

/**
 * @brief list of possible types of differences in #lllyd_difflist
 */
typedef enum {
    LLLYD_DIFF_END = 0,        /**< end of the differences list */
    LLLYD_DIFF_DELETED,        /**< deleted node
                                  - Node is present in the first tree, but not in the second tree.
                                  - To make both trees the same the node in lllyd_difflist::first can be deleted from the
                                    first tree. The pointer at the same index in the lllyd_difflist::second array is
                                    NULL.
                                  - If the deleted node has some children, these do not appear in the resulting diff
                                    separately. In other words, a deleted node is considered deleted with all
                                    its children. */
    LLLYD_DIFF_CHANGED,        /**< value of a leaf or anyxml is changed, the lllyd_difflist::first and lllyd_difflist::second
                                  points to the leaf/anyxml instances in the first and the second tree respectively. */
    LLLYD_DIFF_MOVEDAFTER1,    /**< user-ordered (leaf-)list item was moved.
                                  - To make both trees the same, all #LLLYD_DIFF_MOVEDAFTER1 transactions must be applied
                                  to the first tree in the strict order they appear in the difflist. The
                                  lllyd_difflist::first points to the first tree node being moved and the
                                  lllyd_difflist::second points to the first tree node after which the first node is
                                  supposed to be moved. If the second pointer is NULL, the node is being moved into
                                  the beginning as the first node of the (leaf-)list instances. */
    LLLYD_DIFF_CREATED,        /**< newly created node
                                  - Node is present in the second tree, but not in the first tree.
                                  - To make both trees the same the node in lllyd_difflist::second is supposed to be
                                    inserted (copied via lllyd_dup()) into the node (as a child) at the same index in the
                                    lllyd_difflist::first array (where is its parent). If the lllyd_difflist::first at the
                                    index is NULL, the missing node is top-level.
                                  - If the created node has some children, these do not appear in the resulting diff
                                    separately. In other words, a created node is considered created with all
                                    its children. */
    LLLYD_DIFF_MOVEDAFTER2     /**< similar to LLLYD_DIFF_MOVEDAFTER1, but this time the moved item is in the second tree.
                                  This type is always used in combination with (as a successor of) #LLLYD_DIFF_CREATED
                                  as an instruction to move the newly created node to a specific position. If it is not
                                  present, it means that even the parent of the user-ordered instances did not exist
                                  (or was empty) so it is safe to just create the instances in the same order. Note,
                                  that due to applicability to the second tree, the meaning of lllyd_difflist:first and
                                  lllyd_difflist:second is inverse in comparison to #LLLYD_DIFF_MOVEDAFTER1. The
                                  lllyd_difflist::second points to the (previously) created node in the second tree and
                                  the lllyd_difflist::first points to the predecessor node in the second tree. If the
                                  predecessor is NULL, the node is supposed to bes the first sibling. */
} LLLYD_DIFFTYPE;

/**
 * @brief Structure for the result of lllyd_diff(), describing differences between two data trees.
 */
struct lllyd_difflist {
    LLLYD_DIFFTYPE *type;      /**< array of the differences types, terminated by #LLLYD_DIFF_END value. */
    struct lllyd_node **first; /**< array of nodes in the first tree for the specific type of difference, see the
                                  description of #LLLYD_DIFFTYPE values for more information. */
    struct lllyd_node **second;/**< array of nodes in the second tree for the specific type of difference, see the
                                  description of #LLLYD_DIFFTYPE values for more information. */
};

/**
 * @brief Free the result of lllyd_diff(). It frees the structure of the lllyd_diff() result, not the referenced nodes.
 *
 * @param[in] diff The lllyd_diff() result to free.
 */
void lllyd_free_diff(struct lllyd_difflist *diff);

/**
 * @brief Compare two data trees and provide list of differences.
 *
 * Note, that the \p first and the \p second must have the same schema parent (or they must be top-level elements).
 * In case of using #LLLYD_OPT_NOSIBLINGS, they both must be instances of the same schema node.
 *
 * Order of the resulting set follows these rules:
 * - To change the first tree into the second tree, the resulting transactions are supposed to be applied in the order
 *   they appear in the result. First, the changed (#LLLYD_DIFF_CHANGED) nodes are described followed by the deleted
 *   (#LLLYD_DIFF_DELETED) nodes. Then, the moving of the user-ordered nodes present in both trees (#LLLYD_DIFF_MOVEDAFTER1)
 *   follows and the last transactions in the results are the newly created (#LLLYD_DIFF_CREATED) nodes. These nodes are
 *   supposed to be added as the last siblings, but in some case they can need additional move. In such a case, the
 *   #LLLYD_DIFF_MOVEDAFTER2 transactions can appear.
 * - The order of the changed (#LLLYD_DIFF_CHANGED) and created (#LLLYD_DIFF_CREATED) follows the nodes order in the
 *   second tree - the current siblings are processed first and then the children are processed. Note, that this is
 *   actually not the BFS:
 *
 *           1     2
 *          / \   / \
 *         3   4 7   8
 *        / \
 *       5   6
 *
 * - The order of the deleted (#LLLYD_DIFF_DELETED) nodes is the DFS:
 *
 *           1     6
 *          / \   / \
 *         2   5 7   8
 *        / \
 *       3   4
 *
 * To change the first tree into the second one, it is necessary to follow the order of transactions described in
 * the result. Note, that it is not possible just to use the transactions in the reverse order to transform the
 * second tree into the first one. The transactions can be generalized (to be used on a different instance of the
 * first tree) using lllyd_path() to get identifiers for the nodes used in the transactions.
 *
 * @param[in] first The first (sub)tree to compare. Without #LLLYD_OPT_NOSIBLINGS option, all siblings are
 *            taken into comparison. If NULL, all the \p second nodes are supposed to be top level and they will
 *            be marked as #LLLYD_DIFF_CREATED.
 * @param[in] second The second (sub)tree to compare. Without #LLLYD_OPT_NOSIBLINGS option, all siblings are
 *            taken into comparison. If NULL, all the \p first nodes will be marked as #LLLYD_DIFF_DELETED.
 * @param[in] options The @ref diffoptions are accepted.
 * @return NULL on error, the list of differences on success. In case the trees are the same, the first item in the
 *         lllyd_difflist::type array is #LLLYD_DIFF_END. The returned structure is supposed to be freed by lllyd_free_diff().
 */
struct lllyd_difflist *lllyd_diff(struct lllyd_node *first, struct lllyd_node *second, int options);

/**
 * @defgroup diffoptions Diff options
 * @ingroup datatree
 *
 * @{
 */
/* LLLYD_DIFFOPT_NOSIBLINGS value is the same as LLLYD_OPT_NOSIBLINGS due to backward compatibility. The LLLYD_OPT_NOSIBLINGS
 * was used previously as an option for lllyd_diff(). */
#define LLLYD_DIFFOPT_NOSIBLINGS   0x0800 /**< The both trees to diff have to instantiate the same schema node so only the
                                             single subtree is compared. */
#define LLLYD_DIFFOPT_WITHDEFAULTS 0x0001 /**< Take default nodes with their values into account and handle them as part
                                             of both trees. Summary of the modified behavior:
                                             - deleted node is replaced with implicit default node - #LLLYD_DIFF_CHANGED instead delete
                                             - created node replaces an implicit default node - #LLLYD_DIFF_CHANGED instead create
                                             - in both cases even if the values match - #LLLYD_DIFF_CHANGED is still returned, because dlft flag was changed
                                             Note that in this case, applying the resulting
                                             transactions on the first tree does not result to the exact second tree,
                                             because instead of having implicit default nodes you are going to have
                                             explicit default nodes. */
/**@} diffoptions */

/**
 * @brief Build data path (usable as path, see @ref howtoxpath) of the data node.
 * @param[in] node Data node to be processed. Note that the node should be from a complete data tree, having a subtree
 *            (after using lllyd_unlink()) can cause generating invalid paths.
 * @return NULL on error, on success the buffer for the resulting path is allocated and caller is supposed to free it
 * with free().
 */
char *lllyd_path(const struct lllyd_node *node);

/**
 * @defgroup parseroptions Data parser options
 * @ingroup datatree
 *
 * Various options to change the data tree parsers behavior.
 *
 * Default behavior:
 * - in case of XML, parser reads all data from its input (file, memory, XML tree) including the case of not well-formed
 * XML document (multiple top-level elements) and if there is an unknown element, it is skipped including its subtree
 * (see the next point). This can be changed by the #LLLYD_OPT_NOSIBLINGS option which make parser to read only a single
 * tree (with a single root element) from its input.
 * - parser silently ignores the data without a matching node in schema trees. If the caller want to stop
 * parsing in case of presence of unknown data, the #LLLYD_OPT_STRICT can be used. The strict mode is useful for
 * NETCONF servers, since NETCONF clients should always send data according to the capabilities announced by the server.
 * On the other hand, the default non-strict mode is useful for clients receiving data from NETCONF server since
 * clients are not required to understand everything the server does. Of course, the optimal strategy for clients is
 * to use filtering to get only the required data. Having an unknown element of the known namespace is always an error.
 * The behavior can be changed by #LLLYD_OPT_STRICT option.
 * - using obsolete statements (status set to obsolete) just generates a warning, but the processing continues. The
 * behavior can be changed by #LLLYD_OPT_OBSOLETE option.
 * - parser expects that the provided data provides complete datastore content (both the configuration and state data)
 * and performs data validation according to all YANG rules. This can be a problem in case of representing NETCONF's
 * subtree filter data, edit-config's data or other type of data set - such data do not represent a complete data set
 * and some of the validation rules can fail. Therefore there are other options (within lower 8 bits) to make parser
 * to accept such a data.
 * - when parser evaluates when-stmt condition to false, a validation error is raised. If the
 * #LLLYD_OPT_WHENAUTODEL is used, the invalid node is silently removed instead of an error. The option (and also this default
 * behavior) takes effect only in case of #LLLYD_OPT_DATA or #LLLYD_OPT_CONFIG type of data.
 * @{
 */

#define LLLYD_OPT_DATA       0x00 /**< Default type of data - complete datastore content with configuration as well as
                                     state data. To handle possibly missing (but by default required) ietf-yang-library
                                     data, use #LLLYD_OPT_DATA_NO_YANGLIB or #LLLYD_OPT_DATA_ADD_YANGLIB options. */
#define LLLYD_OPT_CONFIG     0x01 /**< A configuration datastore - complete datastore without state data.
                                     Validation modifications:
                                     - status data are not allowed */
#define LLLYD_OPT_GET        0x02 /**< Data content from a NETCONF reply message to the NETCONF \<get\> operation.
                                     Validation modifications:
                                     - mandatory nodes can be omitted
                                     - leafrefs and instance-identifier resolution is allowed to fail
                                     - list's keys/unique nodes are not required (so duplication is not checked)
                                     - must and when evaluation skipped */
#define LLLYD_OPT_GETCONFIG  0x04 /**< Data content from a NETCONF reply message to the NETCONF \<get-config\> operation
                                     Validation modifications:
                                     - mandatory nodes can be omitted
                                     - leafrefs and instance-identifier resolution is allowed to fail
                                     - list's keys/unique nodes are not required (so duplication is not checked)
                                     - must and when evaluation skipped
                                     - status data are not allowed */
#define LLLYD_OPT_EDIT       0x08 /**< Content of the NETCONF \<edit-config\>'s config element.
                                     Validation modifications:
                                     - mandatory nodes can be omitted
                                     - leafrefs and instance-identifier resolution is allowed to fail
                                     - must and when evaluation skipped
                                     - status data are not allowed */
#define LLLYD_OPT_RPC        0x10 /**< Data represents RPC or action input parameters. In case of an action, **only**
                                     the parent nodes are expected ([RFC ref](https://tools.ietf.org/html/rfc7950#section-7.15.2)).
                                     For validation an additional data tree with the references should be provided. */
#define LLLYD_OPT_RPCREPLY   0x20 /**< Data represents RPC or action output parameters (maps to NETCONF <rpc-reply> data). */
#define LLLYD_OPT_NOTIF      0x40 /**< Data represents an event notification data. In case of a nested notification, **only**
                                     the parent nodes are expected ([RFC ref](https://tools.ietf.org/html/rfc7950#section-7.16.2)).
                                     For validation an additional data tree with the references should be provided. */
#define LLLYD_OPT_NOTIF_FILTER 0x80 /**< Data represents a filtered event notification data.
                                       Validation modification:
                                       - the only requirement is that the data tree matches the schema tree */
#define LLLYD_OPT_TYPEMASK   0x10000ff /**< Mask to filter data type options. Always only a single data type option (only
                                          single bit from the lower 8 bits) can be set. */

/* 0x100 reserved, used internally */
#define LLLYD_OPT_STRICT     0x0200 /**< Instead of silent ignoring data without schema definition, raise an error. */
#define LLLYD_OPT_DESTRUCT   0x0400 /**< Free the provided XML tree during parsing the data. With this option, the
                                       provided XML tree is affected and all successfully parsed data are freed.
                                       This option is applicable only to lllyd_parse_xml() function. */
#define LLLYD_OPT_OBSOLETE   0x0800 /**< Raise an error when an obsolete statement (status set to obsolete) is used. */
#define LLLYD_OPT_NOSIBLINGS 0x1000 /**< Parse only a single XML tree from the input. This option applies only to
                                       XML input data. */
#define LLLYD_OPT_TRUSTED    0x2000 /**< Data comes from a trusted source and it is not needed to validate them. Data
                                       are connected with the schema, but the most validation checks (mandatory nodes,
                                       list instance uniqueness, etc.) are not performed. This option does not make
                                       sense for lllyd_validate() so it is ignored by this function. */
#define LLLYD_OPT_WHENAUTODEL 0x4000 /**< Automatically delete subtrees with false when-stmt condition. The flag is
                                        applicable only in combination with #LLLYD_OPT_DATA and #LLLYD_OPT_CONFIG flags.
                                        If used, libyang will not generate a validation error. */
#define LLLYD_OPT_NOEXTDEPS  0x8000 /**< Allow external dependencies (external leafrefs, instance-identifiers, must,
                                       and when) to not be resolved/satisfied during validation. */
#define LLLYD_OPT_DATA_NO_YANGLIB  0x10000 /**< Ignore (possibly) missing ietf-yang-library data. Applicable only with #LLLYD_OPT_DATA. */
#define LLLYD_OPT_DATA_ADD_YANGLIB 0x20000 /**< Add missing ietf-yang-library data into the validated data tree. Applicable
                                              only with #LLLYD_OPT_DATA. If some ietf-yang-library data are present, they are
                                              preserved and option is ignored. */
#define LLLYD_OPT_VAL_DIFF 0x40000 /**< Flag only for validation, store all the data node changes performed by the validation
                                      in a diff structure. */
#define LLLYD_OPT_LYB_MOD_UPDATE 0x80000 /**< Allow to parse data using an updated revision of a module, relevant only for LLLYB format. */
#define LLLYD_OPT_DATA_TEMPLATE 0x1000000 /**< Data represents YANG data template. */

/**@} parseroptions */

/**
 * @brief Parse (and validate) data from memory.
 *
 * In case of LLLY_XML format, the data string is parsed completely. It means that when it contains
 * a non well-formed XML with multiple root elements, all those sibling XML trees are parsed. The
 * returned data node is a root of the first tree with other trees connected via the next pointer.
 * This behavior can be changed by #LLLYD_OPT_NOSIBLINGS option.
 *
 * @param[in] ctx Context to connect with the data tree being built here.
 * @param[in] data Serialized data in the specified format.
 * @param[in] format Format of the input data to be parsed.
 * @param[in] options Parser options, see @ref parseroptions. \p format LLLYD_LYB uses #LLLYD_OPT_TRUSTED implicitly.
 * @param[in] ... Variable arguments depend on \p options. If they include:
 *                - #LLLYD_OPT_DATA:
 *                - #LLLYD_OPT_CONFIG:
 *                - #LLLYD_OPT_GET:
 *                - #LLLYD_OPT_GETCONFIG:
 *                - #LLLYD_OPT_EDIT:
 *                  - no variable arguments expected.
 *                - #LLLYD_OPT_RPC:
 *                - #LLLYD_OPT_NOTIF:
 *                  - struct ::lllyd_node *data_tree - additional **validated** top-level siblings of a data tree that
 *                    will be used when checking any references ("when", "must" conditions, leafrefs, ...)
 *                    **in the action/nested notification subtree** that require some nodes outside their subtree.
 *                    It is assumed that **all parents** of the action/nested notification exist as required
 *                    ([RFC ref](https://tools.ietf.org/html/rfc8342#section-6.2)).
 *                - #LLLYD_OPT_RPCREPLY:
 *                  - const struct ::lllyd_node *rpc_act - pointer to the whole RPC or (top-level) action operation
 *                    data tree (the request) of the reply.
 *                  - const struct ::lllyd_node *data_tree - additional **validated** top-level siblings of a data tree that
 *                    will be used when checking any references ("when", "must" conditions, leafrefs, ...)
 *                    that require some nodes outside their subtree.
 * @return Pointer to the built data tree or NULL in case of empty \p data. To free the returned structure,
 *         use lllyd_free(). In these cases, the function sets #llly_errno to LLLY_SUCCESS. In case of error,
 *         #llly_errno contains appropriate error code (see #LLLY_ERR).
 */
struct lllyd_node *lllyd_parse_mem(struct llly_ctx *ctx, const char *data, LLLYD_FORMAT format, int options, ...);

/**
 * @brief Read (and validate) data from the given file descriptor.
 *
 * \note Current implementation supports only reading data from standard (disk) file, not from sockets, pipes, etc.
 *
 * In case of LLLY_XML format, the file content is parsed completely. It means that when it contains
 * a non well-formed XML with multiple root elements, all those sibling XML trees are parsed. The
 * returned data node is a root of the first tree with other trees connected via the next pointer.
 * This behavior can be changed by #LLLYD_OPT_NOSIBLINGS option.
 *
 * @param[in] ctx Context to connect with the data tree being built here.
 * @param[in] fd The standard file descriptor of the file containing the data tree in the specified format.
 * @param[in] format Format of the input data to be parsed.
 * @param[in] options Parser options, see @ref parseroptions. \p format LLLYD_LYB uses #LLLYD_OPT_TRUSTED implicitly.
 * @param[in] ... Variable arguments depend on \p options. If they include:
 *                - #LLLYD_OPT_DATA:
 *                - #LLLYD_OPT_CONFIG:
 *                - #LLLYD_OPT_GET:
 *                - #LLLYD_OPT_GETCONFIG:
 *                - #LLLYD_OPT_EDIT:
 *                  - no variable arguments expected.
 *                - #LLLYD_OPT_RPC:
 *                - #LLLYD_OPT_NOTIF:
 *                  - struct ::lllyd_node *data_tree - additional **validated** top-level siblings of a data tree that
 *                    will be used when checking any references ("when", "must" conditions, leafrefs, ...)
 *                    **in the action/nested notification subtree** that require some nodes outside their subtree.
 *                    It is assumed that **all parents** of the action/nested notification exist as required
 *                    ([RFC ref](https://tools.ietf.org/html/rfc8342#section-6.2)).
 *                - #LLLYD_OPT_RPCREPLY:
 *                  - const struct ::lllyd_node *rpc_act - pointer to the whole RPC or action operation data
 *                    tree (the request) of the reply.
 *                  - const struct ::lllyd_node *data_tree - additional **validated** top-level siblings of a data tree that
 *                    will be used when checking any references ("when", "must" conditions, leafrefs, ...)
 *                    that require some nodes outside their subtree.
 * @return Pointer to the built data tree or NULL in case of empty file. To free the returned structure,
 *         use lllyd_free(). In these cases, the function sets #llly_errno to LLLY_SUCCESS. In case of error,
 *         #llly_errno contains appropriate error code (see #LLLY_ERR).
 */
struct lllyd_node *lllyd_parse_fd(struct llly_ctx *ctx, int fd, LLLYD_FORMAT format, int options, ...);

/**
 * @brief Read (and validate) data from the given file path.
 *
 * In case of LLLY_XML format, the file content is parsed completely. It means that when it contains
 * a non well-formed XML with multiple root elements, all those sibling XML trees are parsed. The
 * returned data node is a root of the first tree with other trees connected via the next pointer.
 * This behavior can be changed by #LLLYD_OPT_NOSIBLINGS option.
 *
 * @param[in] ctx Context to connect with the data tree being built here.
 * @param[in] path Path to the file containing the data tree in the specified format.
 * @param[in] format Format of the input data to be parsed.
 * @param[in] options Parser options, see @ref parseroptions. \p format LLLYD_LYB uses #LLLYD_OPT_TRUSTED implicitly.
 * @param[in] ... Variable arguments depend on \p options. If they include:
 *                - #LLLYD_OPT_DATA:
 *                - #LLLYD_OPT_CONFIG:
 *                - #LLLYD_OPT_GET:
 *                - #LLLYD_OPT_GETCONFIG:
 *                - #LLLYD_OPT_EDIT:
 *                  - no variable arguments expected.
 *                - #LLLYD_OPT_RPC:
 *                - #LLLYD_OPT_NOTIF:
 *                  - struct ::lllyd_node *data_tree - additional **validated** top-level siblings of a data tree that
 *                    will be used when checking any references ("when", "must" conditions, leafrefs, ...)
 *                    **in the action/nested notification subtree** that require some nodes outside their subtree.
 *                    It is assumed that **all parents** of the action/nested notification exist as required
 *                    ([RFC ref](https://tools.ietf.org/html/rfc8342#section-6.2)).
 *                - #LLLYD_OPT_RPCREPLY:
 *                  - const struct ::lllyd_node *rpc_act - pointer to the whole RPC or action operation data
 *                    tree (the request) of the reply.
 *                  - const struct ::lllyd_node *data_tree - additional **validated** top-level siblings of a data tree that
 *                    will be used when checking any references ("when", "must" conditions, leafrefs, ...)
 *                    that require some nodes outside their subtree.
 * @return Pointer to the built data tree or NULL in case of empty file. To free the returned structure,
 *         use lllyd_free(). In these cases, the function sets #llly_errno to LLLY_SUCCESS. In case of error,
 *         #llly_errno contains appropriate error code (see #LLLY_ERR).
 */
struct lllyd_node *lllyd_parse_path(struct llly_ctx *ctx, const char *path, LLLYD_FORMAT format, int options, ...);

/**
 * @brief Parse (and validate) XML tree.
 *
 * The output data tree is parsed from the given XML tree previously parsed by one of the
 * lllyxml_read* functions.
 *
 * If there are some sibling elements of the \p root (data were read with #LLLYXML_PARSE_MULTIROOT option
 * or the provided root is a root element of a subtree), all the sibling nodes (previous as well as
 * following) are processed as well. The returned data node is a root of the first tree with other
 * trees connected via the next pointer. This behavior can be changed by #LLLYD_OPT_NOSIBLINGS option.
 *
 * When the function is used with #LLLYD_OPT_DESTRUCT, all the successfully parsed data including the
 * XML \p root and all its siblings (if #LLLYD_OPT_NOSIBLINGS is not used) are freed. Only with
 * #LLLYD_OPT_DESTRUCT option the \p root pointer is changed - if all the data are parsed, it is set
 * to NULL, otherwise it will hold the XML tree without the successfully parsed elements.
 *
 * The context must be the same as the context used to parse XML tree by lllyxml_read* function.
 *
 * @param[in] ctx Context to connect with the data tree being built here.
 * @param[in,out] root XML tree to parse (convert) to data tree. By default, parser do not change the XML tree. However,
 *            when #LLLYD_OPT_DESTRUCT is specified in \p options, parser frees all successfully parsed data.
 * @param[in] options Parser options, see @ref parseroptions.
 * @param[in] ... Variable arguments depend on \p options. If they include:
 *                - #LLLYD_OPT_DATA:
 *                - #LLLYD_OPT_CONFIG:
 *                - #LLLYD_OPT_GET:
 *                - #LLLYD_OPT_GETCONFIG:
 *                - #LLLYD_OPT_EDIT:
 *                  - no variable arguments expected.
 *                - #LLLYD_OPT_RPC:
 *                - #LLLYD_OPT_NOTIF:
 *                  - struct ::lllyd_node *data_tree - additional **validated** top-level siblings of a data tree that
 *                    will be used when checking any references ("when", "must" conditions, leafrefs, ...)
 *                    **in the action/nested notification subtree** that require some nodes outside their subtree.
 *                    It is assumed that **all parents** of the action/nested notification exist as required
 *                    ([RFC ref](https://tools.ietf.org/html/rfc8342#section-6.2)).
 *                - #LLLYD_OPT_RPCREPLY:
 *                  - const struct ::lllyd_node *rpc_act - pointer to the whole RPC or action operation data
 *                    tree (the request) of the reply.
 *                  - const struct ::lllyd_node *data_tree - additional **validated** top-level siblings of a data tree that
 *                    will be used when checking any references ("when", "must" conditions, leafrefs, ...)
 *                    that require some nodes outside their subtree.
 * @return Pointer to the built data tree or NULL in case of empty \p root. To free the returned structure,
 *         use lllyd_free(). In these cases, the function sets #llly_errno to LLLY_SUCCESS. In case of error,
 *         #llly_errno contains appropriate error code (see #LLLY_ERR).
 */
struct lllyd_node *lllyd_parse_xml(struct llly_ctx *ctx, struct lllyxml_elem **root, int options,...);

/**
 * @brief Create a new container node in a data tree.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating top level element.
 * @param[in] module Module with the node being created.
 * @param[in] name Schema node name of the new data node. The node can be #LLLYS_CONTAINER, #LLLYS_LIST,
 * #LLLYS_NOTIF, #LLLYS_RPC, or #LLLYS_ACTION.
 * @return New node, NULL on error.
 */
struct lllyd_node *lllyd_new(struct lllyd_node *parent, const struct lllys_module *module, const char *name);

/**
 * @brief Create a new leaf or leaflist node in a data tree with a string value that is converted to
 * the actual value.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating top level element.
 * @param[in] module Module with the node being created.
 * @param[in] name Schema node name of the new data node.
 * @param[in] val_str String form of the value of the node being created. In case the type is #LLLY_TYPE_INST
 * or #LLLY_TYPE_IDENT, JSON node-id format is expected (nodes are prefixed with module names, not XML namespaces).
 * @return New node, NULL on error.
 */
struct lllyd_node *lllyd_new_leaf(struct lllyd_node *parent, const struct lllys_module *module, const char *name,
                              const char *val_str);

/**
 * @brief Change value of a leaf node.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * Despite the prototype allows to provide a leaflist node as \p leaf parameter, only leafs are accepted.
 * Also, the leaf will never be default after calling this function successfully.
 *
 * @param[in] leaf A leaf node to change.
 * @param[in] val_str String form of the new value to be set to the \p leaf. In case the type is #LLLY_TYPE_INST
 * or #LLLY_TYPE_IDENT, JSON node-id format is expected (nodes are prefixed with module names, not XML namespaces).
 * @return 0 if the leaf was changed successfully (either its value changed or at least its default flag was cleared),
 *         <0 on error,
 *         1 if the (canonical) value matched the original one and no value neither default flag change occurred.
 */
int lllyd_change_leaf(struct lllyd_node_leaf_list *leaf, const char *val_str);

/**
 * @brief Create a new anydata or anyxml node in a data tree.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * This function is supposed to be a replacement for the lllyd_new_anyxml_str() and lllyd_new_anyxml_xml().
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating top level element.
 * @param[in] module Module with the node being created.
 * @param[in] name Schema node name of the new data node. The schema node determines if the anydata or anyxml node
 *            is created.
 * @param[in] value Pointer to the value data to be stored in the anydata/anyxml node. The type of the data is
 *            determined according to the \p value_type parameter.
 * @param[in] value_type Type of the provided data \p value.
 * @return New node, NULL on error.
 */
struct lllyd_node *lllyd_new_anydata(struct lllyd_node *parent, const struct lllys_module *module, const char *name,
                                 void *value, LLLYD_ANYDATA_VALUETYPE value_type);

/**
 * @brief Create a new container node in a data tree. Ignore RPC/action input nodes and instead use RPC/action output ones.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating top level element.
 * @param[in] module Module with the node being created.
 * @param[in] name Schema node name of the new data node. The node should only be #LLLYS_CONTAINER or #LLLYS_LIST,
 * but accepted are also #LLLYS_NOTIF, #LLLYS_RPC, or #LLLYS_ACTION.
 * @return New node, NULL on error.
 */
struct lllyd_node *lllyd_new_output(struct lllyd_node *parent, const struct lllys_module *module, const char *name);

/**
 * @brief Create a new leaf or leaflist node in a data tree with a string value that is converted to
 * the actual value. Ignore RPC/action input nodes and instead use RPC/action output ones.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating top level element.
 * @param[in] module Module with the node being created.
 * @param[in] name Schema node name of the new data node.
 * @param[in] val_str String form of the value of the node being created. In case the type is #LLLY_TYPE_INST
 * or #LLLY_TYPE_IDENT, JSON node-id format is expected (nodes are prefixed with module names, not XML namespaces).
 * @return New node, NULL on error.
 */
struct lllyd_node *lllyd_new_output_leaf(struct lllyd_node *parent, const struct lllys_module *module, const char *name,
                                     const char *val_str);

/**
 * @brief Create a new anydata or anyxml node in a data tree. Ignore RPC/action input nodes and instead use
 * RPC/action output ones.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating top level element.
 * @param[in] module Module with the node being created.
 * @param[in] name Schema node name of the new data node. The schema node determines if the anydata or anyxml node
 *            is created.
 * @param[in] value Pointer to the value data to be stored in the anydata/anyxml node. The type of the data is
 *            determined according to the \p value_type parameter. Data are supposed to be dynamically allocated.
 *            Since it is directly attached into the created data node, caller is supposed to not manipulate with
 *            the data after a successful call (including calling free() on the provided data).
 * @param[in] value_type Type of the provided data \p value.
 * @return New node, NULL on error.
 */
struct lllyd_node *lllyd_new_output_anydata(struct lllyd_node *parent, const struct lllys_module *module, const char *name,
                                        void *value, LLLYD_ANYDATA_VALUETYPE value_type);

/**
 * @brief Create a new yang-data template in a data tree. It creates container, which name is in third parameter.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * @param[in] module Module with the node being created.
 * @param[in] name_template Yang-data template name. This name is used for searching of yang-data instance.
 * @param[in] name Schema node name of the new data node. This node is container.
 * @return New node, NULL on error.
 */
struct lllyd_node *lllyd_new_yangdata(const struct lllys_module *module, const char *name_template, const char *name);

/**
 * @defgroup pathoptions Data path creation options
 * @ingroup datatree
 *
 * Various options to change lllyd_new_path() behavior.
 *
 * Default behavior:
 * - if the target node already exists (and is not default), an error is returned.
 * - the whole path to the target node is created (with any missing parents) if necessary.
 * - RPC output schema children are completely ignored in all modules. Input is searched and nodes created normally.
 * @{
 */

#define LLLYD_PATH_OPT_UPDATE   0x01 /**< If the target node exists, is a leaf, and it is updated with a new value or its
                                        default flag is changed, it is returned. If the target node exists and is not
                                        a leaf or generally no change occurs in the \p data_tree, NULL is returned and no error set. */
#define LLLYD_PATH_OPT_NOPARENT 0x02 /**< If any parents of the target node do not exist, return an error instead of implicitly
                                        creating them. */
#define LLLYD_PATH_OPT_OUTPUT   0x04 /**< Changes the behavior to ignoring RPC/action input schema nodes and using only output ones. */
#define LLLYD_PATH_OPT_DFLT     0x08 /**< The created node (nodes, if also creating the parents) is a default one. If working with
                                        data tree of type #LLLYD_OPT_DATA, #LLLYD_OPT_CONFIG, #LLLYD_OPT_RPC, #LLLYD_OPT_RPCREPLY, or
                                        #LLLYD_OPT_NOTIF, this flag is never needed and therefore should not be used. However, if
                                        the tree is #LLLYD_OPT_GET, #LLLYD_OPT_GETCONFIG, or #LLLYD_OPT_EDIT, the default nodes are not
                                        created during validation and using this flag one can set them (see @ref howtodatawd). */
#define LLLYD_PATH_OPT_NOPARENTRET 0x10 /**< Changes the return value in the way that even if some parents were created in
                                        addition to the path-referenced node, the path-referenced node will always be returned. */
#define LLLYD_PATH_OPT_EDIT     0x20 /**< Allows the creation of special leaves without value. These leaves are valid if used
                                        in a NETCONF edit-config with delete/remove operation. */

/** @} pathoptions */

/**
 * @brief Create a new data node based on a simple XPath.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * The new node is normally inserted at the end, either as the last child of a parent or as the last sibling
 * if working with top-level elements. However, when manipulating RPC input or output, schema ordering is
 * required and always guaranteed.
 *
 * If \p path points to a list key and the list does not exist, the key value from the predicate is used
 * and \p value is ignored.
 *
 * @param[in] data_tree Existing data tree to add to/modify (including siblings). If creating RPCs/actions, there
 * should only be one RPC/action and either input or output, not both. Can be NULL.
 * @param[in] ctx Context to use. Mandatory if \p data_tree is NULL.
 * @param[in] path Simple data path (see @ref howtoxpath). List nodes can have predicates, one for each list key
 * in the correct order and with its value as well or using specific instance position, leaves and leaf-lists
 * can have predicates too that have preference over \p value. When specifying an identityref value in a predicate,
 * you MUST use the module name as the value prefix!
 * @param[in] value Value of the new leaf/lealf-list (const char*). If creating anydata or anyxml, the following
 * \p value_type parameter is required to be specified correctly. If creating nodes of other types, the
 * parameter is ignored.
 * @param[in] value_type Type of the provided \p value parameter in case of creating anydata or anyxml node.
 * @param[in] options Bitmask of options flags, see @ref pathoptions.
 * @return First created (or updated with #LLLYD_PATH_OPT_UPDATE) node,
 * NULL if #LLLYD_PATH_OPT_UPDATE was used and the full path exists or the leaf original value matches \p value,
 * NULL and llly_errno is set on error.
 */
struct lllyd_node *lllyd_new_path(struct lllyd_node *data_tree, const struct llly_ctx *ctx, const char *path, void *value,
                              LLLYD_ANYDATA_VALUETYPE value_type, int options);

/**
 * @brief Learn the relative instance position of a list or leaf-list within other instances of the
 * same schema node.
 *
 * @param[in] node List or leaf-list to get the position of.
 * @return 0 on error or positive integer of the instance position.
 */
unsigned int lllyd_list_pos(const struct lllyd_node *node);

/**
 * @defgroup dupoptions Data duplication options
 * @ingroup datatree
 *
 * Various options to change lllyd_dup() behavior.
 *
 * Default behavior:
 * - only the specified node is duplicated without siblings, parents, or children.
 * - all the attributes of the duplicated nodes are also duplicated.
 * @{
 */

#define LLLYD_DUP_OPT_RECURSIVE    0x01 /**< Duplicate not just the node but also all the children. */
#define LLLYD_DUP_OPT_NO_ATTR      0x02 /**< Do not duplicate attributes of any node. */
#define LLLYD_DUP_OPT_WITH_PARENTS 0x04 /**< If a nested node is being duplicated, duplicate also all the parents.
                                           Keys are also duplicated for lists. Return value does not change! */
#define LLLYD_DUP_OPT_WITH_KEYS    0x08 /**< If a lits key is being duplicated non-recursively, duplicate its keys.
                                           Ignored if used with #LLLYD_DUP_OPT_RECURSIVE. Return value does not change! */
#define LLLYD_DUP_OPT_WITH_WHEN    0x10 /**< Also copy any when evaluation state flags. This is useful in case the copied
                                           nodes are actually still part of the same datastore meaning no dependency data
                                           could have changed. Otherwise nothing is assumed about the copied node when
                                           state and it is evaluated from scratch during validation. */

/** @} dupoptions */

/**
 * @brief Create a copy of the specified data tree \p node. Schema references are kept the same. Use carefully,
 * since libyang silently creates default nodes, it is always better to use lllyd_dup_withsiblings() to duplicate
 * the complete data tree.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * @param[in] node Data tree node to be duplicated.
 * @param[in] options Bitmask of options flags, see @ref dupoptions.
 * @return Created copy of the provided data \p node.
 */
struct lllyd_node *lllyd_dup(const struct lllyd_node *node, int options);

/**
 * @brief Create a copy of the specified data tree and all its siblings (preceding as well as following).
 * Schema references are kept the same.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * @param[in] node Data tree sibling node to be duplicated.
 * @param[in] options Bitmask of options flags, see @ref dupoptions.
 * @return Created copy of the provided data \p node and all of its siblings.
 */
struct lllyd_node *lllyd_dup_withsiblings(const struct lllyd_node *node, int options);

/**
 * @brief Create a copy of the specified data tree \p node in the different context. All the
 * schema references and strings are re-mapped into the specified context.
 *
 * If the target context does not contain the schemas used in the source data tree, error
 * is raised and the new data tree is not created.
 *
 * @param[in] node Data tree node to be duplicated.
 * @param[in] options Bitmask of options flags, see @ref dupoptions.
 * @param[in] ctx Target context for the duplicated data.
 * @return Created copy of the provided data \p node.
 */
struct lllyd_node *lllyd_dup_to_ctx(const struct lllyd_node *node, int options, struct llly_ctx *ctx);

/**
 * @brief Merge a (sub)tree into a data tree.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * Missing nodes are merged, leaf values updated.
 *
 * If \p target and \p source do not share the top-level schema node, even if they
 * are from different modules, \p source parents up to top-level node will be created and
 * linked to the \p target (but only containers can be created this way, lists need keys,
 * so if lists are missing, an error will be returned).
 *
 * If the source data tree is in a different context, the resulting data are placed into the context
 * of the target tree.
 *
 * @param[in] target Top-level (or an RPC output child) data tree to merge to. Must be valid.
 * @param[in] source Data tree to merge \p target with. Must be valid (at least as a subtree).
 * @param[in] options Bitmask of the following option flags:
 * - #LLLYD_OPT_DESTRUCT - spend \p source in the function, otherwise \p source is left untouched,
 * - #LLLYD_OPT_NOSIBLINGS - merge only the \p source subtree (ignore siblings), otherwise merge
 * \p source and all its succeeding siblings (preceding ones are still ignored!),
 * - #LLLYD_OPT_EXPLICIT - when merging an explicitly set node and a default node, always put
 * the explicit node into \p target, otherwise the node which is in \p source is used.
 * @return 0 on success, nonzero in case of an error.
 */
int lllyd_merge(struct lllyd_node *target, const struct lllyd_node *source, int options);

/**
 * @brief Same as lllyd_merge(), but moves the resulting data into the specified context.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * @param[in] trg Top-level (or an RPC output child) data tree to merge to. Must be valid. If its context
 *            differs from the specified \p ctx of the result, the provided data tree is freed and the new
 *            tree in the required context is returned on success. To keep the \p trg tree, convert it to the
 *            target context using lllyd_dup_to_ctx() and then call lllyd_merge() instead of lllyd_merge_to_ctx().
 * @param[in] src Data tree to merge \p target with. Must be valid (at least as a subtree).
 * @param[in] options Bitmask of the following option flags:
 * - #LLLYD_OPT_DESTRUCT - spend \p source in the function, otherwise \p source is left untouched,
 * - #LLLYD_OPT_NOSIBLINGS - merge only the \p source subtree (ignore siblings), otherwise merge
 * \p source and all its succeeding siblings (preceding ones are still ignored!),
 * - #LLLYD_OPT_EXPLICIT - when merging an explicitly set node and a default node, always put
 * the explicit node into \p target, otherwise the node which is in \p source is used.
 * @param[in] ctx Target context in which the result will be created. Note that the successful merge requires to have
 *            all the used modules in the source and target data trees loaded in the target context.
 * @return 0 on success, nonzero in case of an error.
 */
int lllyd_merge_to_ctx(struct lllyd_node **trg, const struct lllyd_node *src, int options, struct llly_ctx *ctx);

#define LLLYD_OPT_EXPLICIT 0x0100

/**
 * @brief Insert the \p node element as child to the \p parent element. The \p node is inserted as a last child of the
 * \p parent.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * - if the node is part of some other tree, it is automatically unlinked.
 * - if the node is the first node of a node list (with no parent), all the subsequent nodes are also inserted.
 * - if the key of a list is being inserted, it is placed into a correct position instead of being placed as the last
 * element.
 * - if the target tree includes the default instance of the node being inserted, the default node is silently replaced
 * by the new node.
 * - if a default node is being inserted and the target tree already contains non-default instance, the existing
 * instance is silently replaced. If it contains the exact same default node, it is replaced as well.
 * - if a non-default node is being inserted and there is already its non-default instance in the target tree, the new
 * node is inserted and it is up to the caller to solve the presence of multiple instances afterwards.
 *
 * Note that this function differs from lllyd_insert_before() and lllyd_insert_after() because the position of the
 * node being inserted is determined automatically according to the rules described above. In contrast to
 * lllyd_insert_parent(), lllyd_insert() can not be used for top-level elements since the \p parent parameter must not be
 * NULL. If inserting something larger and not fitting the mentioned use-cases (or simply if unsure), you can always
 * use lllyd_merge(), it should be able to handle any situation.
 *
 * @param[in] parent Parent node for the \p node being inserted.
 * @param[in] node The node being inserted.
 * @return 0 on success, nonzero in case of error, e.g. when the node is being inserted to an inappropriate place
 * in the data tree.
 */
int lllyd_insert(struct lllyd_node *parent, struct lllyd_node *node);

/**
 * @brief Insert the \p node element as a last sibling of the specified \p sibling element.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * - if the node is part of some other tree, it is automatically unlinked.
 * - if the node is the first node of a node list (with no parent), all the subsequent nodes are also inserted.
 * - if the key of a list is being inserted, it is placed into a correct position instead of being placed as the last
 * element.
 * - if the target tree includes the default instance of the node being inserted, the default node is silently replaced
 * by the new node.
 * - if a default node is being inserted and the target tree already contains non-default instance, the existing
 * instance is silently replaced. If it contains the exact same default node, it is replaced as well.
 * - if a non-default node is being inserted and there is already its non-default instance in the target tree, the new
 * node is inserted and it is up to the caller to solve the presence of multiple instances afterwards.
 *
 * Note that this function differs from lllyd_insert_before() and lllyd_insert_after() because the position of the
 * node being inserted is determined automatically as in the case of lllyd_insert(). In contrast to lllyd_insert(),
 * lllyd_insert_sibling() can be used to insert top-level elements. If inserting something larger and not fitting
 * the mentioned use-cases (or simply if unsure), you can always use lllyd_merge(), it should be able to handle
 * any situation.
 *
 * @param[in,out] sibling Sibling node as a reference where to insert the \p node. When function succeeds, the sibling
 * is always set to point to the first sibling node. Note that in some cases described above, the provided sibling
 * node could be removed from the tree.
 * @param[in] node The node being inserted.
 * @return 0 on success, nonzero in case of error, e.g. when the node is being inserted to an inappropriate place
 * in the data tree.
 */
int lllyd_insert_sibling(struct lllyd_node **sibling, struct lllyd_node *node);

/**
 * @brief Insert the \p node element after the \p sibling element. If \p node and \p siblings are already
 * siblings (just moving \p node position).
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * - if the target tree includes the default instance of the node being inserted, the default node is silently removed.
 * - if a default node is being inserted and the target tree already contains non-default instance, the existing
 * instance is removed. If it contains the exact same default node, it is removed as well.
 * - if a non-default node is being inserted and there is already its non-default instance in the target tree, the new
 * node is inserted and it is up to the caller to solve the presence of multiple instances afterwards.
 *
 * @param[in] sibling The data tree node before which the \p node will be inserted.
 * @param[in] node The data tree node to be inserted. If the node is connected somewhere, it is unlinked first.
 * @return 0 on success, nonzero in case of error, e.g. when the node is being inserted to an inappropriate place
 * in the data tree.
 */
int lllyd_insert_before(struct lllyd_node *sibling, struct lllyd_node *node);

/**
 * @brief Insert the \p node element after the \p sibling element. If \p node and \p siblings are already
 * siblings (just moving \p node position).
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * - if the target tree includes the default instance of the node being inserted, the default node is silently removed.
 * - if a default node is being inserted and the target tree already contains non-default instance, the existing
 * instance is removed. If it contains the exact same default node, it is removed as well.
 * - if a non-default node is being inserted and there is already its non-default instance in the target tree, the new
 * node is inserted and it is up to the caller to solve the presence of multiple instances afterwards.
 *
 * @param[in] sibling The data tree node before which the \p node will be inserted. If \p node and \p siblings
 * are already siblings (just moving \p node position), skip validation.
 * @param[in] node The data tree node to be inserted. If the node is connected somewhere, it is unlinked first.
 * @return 0 on success, nonzero in case of error, e.g. when the node is being inserted to an inappropriate place
 * in the data tree.
 */
int lllyd_insert_after(struct lllyd_node *sibling, struct lllyd_node *node);

/**
 * @brief Order siblings according to the schema node ordering.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * If the siblings include data nodes from other modules, they are
 * sorted based on the module order in the context.
 *
 * @param[in] sibling Node, whose siblings will be sorted.
 * @param[in] recursive Whether sort all siblings of siblings, recursively.
 * @return 0 on success, nonzero in case of an error.
 */
int lllyd_schema_sort(struct lllyd_node *sibling, int recursive);

/**
 * @brief Search in the given data for instances of nodes matching the provided path.
 *
 * Learn more about the path format on page @ref howtoxpath.
 *
 * @param[in] ctx_node Path context node.
 * @param[in] path Data path expression filtering the matching nodes.
 * @return Set of found data nodes. If no nodes are matching \p path or the result
 * would be a number, a string, or a boolean, the returned set is empty. In case of an error, NULL is returned.
 */
struct llly_set *lllyd_find_path(const struct lllyd_node *ctx_node, const char *path);

/**
 * @brief Search in the given data for instances of the provided schema node.
 *
 * The \p data is used to find the data root and function then searches in the whole tree and all sibling trees.
 *
 * @param[in] data A node in the data tree to search.
 * @param[in] schema Schema node of the data nodes caller want to find.
 * @return Set of found data nodes. If no data node is found, the returned set is empty.
 * In case of error, NULL is returned.
 */
struct llly_set *lllyd_find_instance(const struct lllyd_node *data, const struct lllys_node *schema);

/**
 * @brief Get the first sibling of the given node.
 *
 * @param[in] node Node which first sibling is going to be the result.
 * @return The first sibling of the given node or the node itself if it is the first child of the parent.
 */
struct lllyd_node *lllyd_first_sibling(struct lllyd_node *node);

/**
 * @brief Validate \p node data subtree.
 *
 * @param[in,out] node Data tree to be validated. In case the \p options includes #LLLYD_OPT_WHENAUTODEL, libyang
 *                     can modify the provided tree including the root \p node.
 * @param[in] options Options for the inserting data to the target data tree options, see @ref parseroptions.
 * @param[in] var_arg Variable argument depends on \p options. If they include:
 *                - #LLLYD_OPT_DATA:
 *                - #LLLYD_OPT_CONFIG:
 *                - #LLLYD_OPT_GET:
 *                - #LLLYD_OPT_GETCONFIG:
 *                - #LLLYD_OPT_EDIT:
 *                  - struct llly_ctx *ctx - context to use when \p node is NULL (for checking an empty tree),
 *                    otherwise can be NULL.
 *                - #LLLYD_OPT_RPC:
 *                - #LLLYD_OPT_RPCREPLY:
 *                - #LLLYD_OPT_NOTIF:
 *                  - struct ::lllyd_node *data_tree - additional **validated** top-level siblings of a data tree that
 *                    will be used when checking any references ("when", "must" conditions, leafrefs, ...)
 *                    **in the operation subtree** that require some nodes outside their subtree.
 *                    It is assumed that **all parents** of the action/nested notification exist as required
 *                    ([RFC ref](https://tools.ietf.org/html/rfc8342#section-6.2)).
 * @param[in] ... Used only if options include #LLLYD_OPT_VAL_DIFF. In that case a (struct lllyd_difflist **)
 *                is expected into which all data node changes performed by the validation will be stored.
 *                Needs to be properly freed. Meaning of diff type is following:
 *                   - LLLYD_DIFF_CREATED:
 *                      - first - Path identifying the parent node (format of lllyd_path()).
 *                      - second - Duplicated subtree of the created nodes.
 *                   - LLLYD_DIFF_DELETED:
 *                      - first - Unlinked subtree of the deleted nodes.
 *                      - second - Path identifying the original parent (format of lllyd_path()).
 * @return 0 on success, nonzero in case of an error.
 */
int lllyd_validate(struct lllyd_node **node, int options, void *var_arg, ...);

/**
 * @brief Validate \p node data tree but only subtrees that belong to the schema found in \p modules. All other
 *        schemas are effectively disabled for the validation.
 *
 * @param[in,out] node Data tree to be validated. In case the \p options includes #LLLYD_OPT_WHENAUTODEL, libyang
 *                     can modify the provided tree including the root \p node.
 * @param[in] modules List of module names to validate.
 * @param[in] mod_count Number of modules in \p modules.
 * @param[in] options Options for the inserting data to the target data tree options, see @ref parseroptions.
 *                    Accepted data type values include #LLLYD_OPT_DATA, #LLLYD_OPT_CONFIG, #LLLYD_OPT_GET,
 *                    #LLLYD_OPT_GETCONFIG, and #LLLYD_OPT_EDIT.
 * @param[in] ... Used only if options include #LLLYD_OPT_VAL_DIFF. In that case a (struct lllyd_difflist **)
 *                is expected into which all data node changes performed by the validation will be stored.
 *                Needs to be properly freed. Meaning of diff type is following:
 *                   - LLLYD_DIFF_CREATED:
 *                      - first - Path identifying the parent node (format of lllyd_path()).
 *                      - second - Duplicated subtree of the created nodes.
 *                   - LLLYD_DIFF_DELETED:
 *                      - first - Unlinked subtree of the deleted nodes.
 *                      - second - Path identifying the original parent (format of lllyd_path()).
 * @return 0 on success, nonzero in case of an error.
 */
int lllyd_validate_modules(struct lllyd_node **node, const struct lllys_module **modules, int mod_count, int options, ...);

/**
 * @brief Free special diff that was returned by lllyd_validate() or lllyd_validate_modules().
 *
 * @param[in] diff Diff to free.
 */
void lllyd_free_val_diff(struct lllyd_difflist *diff);

/**
 * @brief Check restrictions applicable to the particular leaf/leaf-list on the given string value.
 *
 * Validates the value only using the types' restrictions. Do not check the rest of restrictions dependent on the
 * data tree (must, when statements or uniqueness of the leaf-list item).
 *
 * The format of the data must follow rules for the lexical representation of the specific YANG type. Note
 * that if there are some extensions of the lexical representation for the YANG module (default value), they are
 * not supported by this function - it strictly follows rules for the lexical representations in data trees.
 *
 * @param[in] node Schema node of the leaf or leaf-list eventually holding the \p value.
 * @param[in] value Value to be checked (NULL is checked as empty string).
 * @return EXIT_SUCCESS if the \p value conforms to the restrictions, EXIT_FAILURE otherwise.
 */
int lllyd_validate_value(struct lllys_node *node, const char *value);

/**
 * @brief Get know if the node contain (despite implicit or explicit) default value.
 *
 * @param[in] node The leaf or leaf-list to check. Note, that leaf-list is marked as default only when the complete
 *                 and only the default set is present (node's siblings are also checked).
 * @return 1 if the node contains the default value, 0 otherwise.
 */
int lllyd_wd_default(struct lllyd_node_leaf_list *node);

/**
 * @brief Unlink the specified data subtree. All referenced namespaces are copied.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * Note, that the node's connection with the schema tree is kept. Therefore, in case of
 * reconnecting the node to a data tree using lllyd_paste() it is necessary to paste it
 * to the appropriate place in the data tree following the schema.
 *
 * @param[in] node Data tree node to be unlinked (together with all children).
 * @return 0 for success, nonzero for error
 */
int lllyd_unlink(struct lllyd_node *node);

/**
 * @brief Free (and unlink) the specified data subtree. Use carefully, since libyang silently creates default nodes,
 * it is always better to use lllyd_free_withsiblings() to free the complete data tree.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * @param[in] node Root of the (sub)tree to be freed.
 */
void lllyd_free(struct lllyd_node *node);

/**
 * @brief Free (and unlink) the specified data tree and all its siblings (preceding as well as following).
 *
 * If used on a top-level node it means that the whole data tree is being freed and unnecessary operations
 * are skipped. Always use this function for freeing a whole data tree to achieve better performance.
 *
 * __PARTIAL CHANGE__ - validate after the final change on the data tree (see @ref howtodatamanipulators).
 *
 * @param[in] node One of the siblings root element of the (sub)trees to be freed.
 */
void lllyd_free_withsiblings(struct lllyd_node *node);

/**
 * @brief Insert attribute into the data node.
 *
 * @param[in] parent Data node where to place the attribute
 * @param[in] mod An alternative way to specify attribute's module (namespace) used in case the \p name does
 *            not include prefix. If neither prefix in the \p name nor mod is specified, the attribute's
 *            module is inherited from the \p parent node. It is not allowed to have attributes with no
 *            module (namespace).
 * @param[in] name Attribute name. The string can include the attribute's module (namespace) as the name's
 *            prefix (prefix:name). Prefix must be the name of one of the schema in the \p parent's context.
 *            If the prefix is not specified, the \p mod parameter is used. If neither of these parameters is
 *            usable, attribute inherits module (namespace) from the \p parent node. It is not allowed to
 *            have attributes with no module (namespace).
 * @param[in] value Attribute value
 * @return pointer to the created attribute (which is already connected in \p parent) or NULL on error.
 */
struct lllyd_attr *lllyd_insert_attr(struct lllyd_node *parent, const struct lllys_module *mod, const char *name,
                                 const char *value);

/**
 * @brief Destroy data attribute
 *
 * If the attribute to destroy is a member of a node attribute list, it is necessary to
 * provide the node itself as \p parent to keep the list consistent.
 *
 * @param[in] ctx Context where the attribute was created (usually it is the context of the \p parent)
 * @param[in] parent Parent node where the attribute is placed
 * @param[in] attr Attribute to destroy
 * @param[in] recursive Zero to destroy only the attribute, non-zero to destroy also all the subsequent attributes
 *            in the list.
 */
void lllyd_free_attr(struct llly_ctx *ctx, struct lllyd_node *parent, struct lllyd_attr *attr, int recursive);

/**
 * @brief Return main module of the data tree node.
 *
 * In case of regular YANG module, it returns ::lllys_node#module pointer,
 * but in case of submodule, it returns pointer to the main module.
 *
 * @param[in] node Data tree node to be examined
 * @return pointer to the main module (schema structure), NULL in case of error.
 */
struct lllys_module *lllyd_node_module(const struct lllyd_node *node);

/**
 * @brief Get the type structure of a leaf.
 *
 * In case of a union, the correct specific type is found.
 * In case of a leafref, the final (if there is a chain of leafrefs) target's type is found.
 *
 * @param[in] leaf Leaf to examine.
 * @return Found type, NULL on error.
 */
const struct lllys_type *lllyd_leaf_type(const struct lllyd_node_leaf_list *leaf);

/**
* @brief Print data tree in the specified format.
*
* @param[out] strp Pointer to store the resulting dump.
* @param[in] root Root node of the data tree to print. It can be actually any (not only real root)
* node of the data tree to print the specific subtree.
* @param[in] format Data output format.
* @param[in] options [printer flags](@ref printerflags). \p format LLLYD_LYB accepts only #LLLYP_WITHSIBLINGS option.
* @return 0 on success, 1 on failure (#llly_errno is set).
*/
int lllyd_print_mem(char **strp, const struct lllyd_node *root, LLLYD_FORMAT format, int options);

/**
 * @brief Print data tree in the specified format.
 *
 * @param[in] fd File descriptor where to print the data.
 * @param[in] root Root node of the data tree to print. It can be actually any (not only real root)
 * node of the data tree to print the specific subtree.
 * @param[in] format Data output format.
 * @param[in] options [printer flags](@ref printerflags). \p format LLLYD_LYB accepts only #LLLYP_WITHSIBLINGS option.
 * @return 0 on success, 1 on failure (#llly_errno is set).
 */
int lllyd_print_fd(int fd, const struct lllyd_node *root, LLLYD_FORMAT format, int options);

/**
 * @brief Print data tree in the specified format.
 *
 * @param[in] f File stream where to print the data.
 * @param[in] root Root node of the data tree to print. It can be actually any (not only real root)
 * node of the data tree to print the specific subtree.
 * @param[in] format Data output format.
 * @param[in] options [printer flags](@ref printerflags). \p format LLLYD_LYB accepts only #LLLYP_WITHSIBLINGS option.
 * @return 0 on success, 1 on failure (#llly_errno is set).
 */
int lllyd_print_file(FILE *f, const struct lllyd_node *root, LLLYD_FORMAT format, int options);

/**
 * @brief Print data tree in the specified format.
 *
 * @param[in] path File path where to print the data.
 * @param[in] root Root node of the data tree to print. It can be actually any (not only real root)
 * node of the data tree to print the specific subtree.
 * @param[in] format Data output format.
 * @param[in] options [printer flags](@ref printerflags). \p format LLLYD_LYB accepts only #LLLYP_WITHSIBLINGS option.
 * @return 0 on success, 1 on failure (#llly_errno is set).
 */
int lllyd_print_path(const char *path, const struct lllyd_node *root, LLLYD_FORMAT format, int options);

/**
 * @brief Print data tree in the specified format.
 *
 * @param[in] writeclb Callback function to write the data (see write(1)).
 * @param[in] root Root node of the data tree to print. It can be actually any (not only real root)
 * node of the data tree to print the specific subtree.
 * @param[in] arg Optional caller-specific argument to be passed to the \p writeclb callback.
 * @param[in] format Data output format.
 * @param[in] options [printer flags](@ref printerflags). \p format LLLYD_LYB accepts only #LLLYP_WITHSIBLINGS option.
 * @return 0 on success, 1 on failure (#llly_errno is set).
 */
int lllyd_print_clb(ssize_t (*writeclb)(void *arg, const void *buf, size_t count), void *arg,
                  const struct lllyd_node *root, LLLYD_FORMAT format, int options);

/**
 * @brief Get the double value of a decimal64 leaf/leaf-list.
 *
 * YANG decimal64 type enables higher precision numbers than IEEE 754 double-precision
 * format, so this conversion does not have to be lossless.
 *
 * @param[in] node Leaf/leaf-list of type decimal64.
 * @return Closest double equivalent to the decimal64 value.
 */
double lllyd_dec64_to_double(const struct lllyd_node *node);

/**
 * @brief Get the length of a printed LLLYB data tree.
 *
 * @param[in] data LLLYB data.
 * @return \p data length or -1 on error.
 */
int lllyd_lyb_data_length(const char *data);

#ifdef LLLY_ENABLED_LYD_PRIV

/**
 * @brief Set a schema private pointer to a user pointer.
 *
 * @param[in] node Data node, whose private field will be assigned.
 * @param[in] priv Arbitrary user-specified pointer.
 * @return Previous private object of the \p node (NULL if this is the first call on the \p node). Note, that
 * the caller is in this case responsible (if it is necessary) for freeing the replaced private object. In case
 * of invalid (NULL) \p node, NULL is returned and #llly_errno is set to #LLLY_EINVAL.
 */
void *lllyd_set_private(const struct lllyd_node *node, void *priv);

#endif

/**@} */

#ifdef __cplusplus
}
#endif

#endif /* LLLY_TREE_DATA_H_ */
