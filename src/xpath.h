/**
 * @file xpath.h
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief YANG XPath evaluation functions header
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#ifndef _XPATH_H
#define _XPATH_H

#include <stdint.h>

#include "libyang.h"
#include "tree_schema.h"
#include "tree_data.h"

/*
 * XPath evaluator fully compliant with http://www.w3.org/TR/1999/REC-xpath-19991116/
 * except the following restrictions in the grammar.
 *
 * PARSED GRAMMAR
 *
 * Full axes are not supported, abbreviated forms must be used,
 * variables are not supported, "id()" function is not supported,
 * and processing instruction and comment nodes are not supported,
 * which is also reflected in the grammar. Undefined rules and
 * constants are tokens.
 *
 * Modified full grammar:
 *
 * [1] Expr ::= OrExpr // just an alias
 *
 * [2] LocationPath ::= RelativeLocationPath | AbsoluteLocationPath
 * [3] AbsoluteLocationPath ::= '/' RelativeLocationPath? | '//' RelativeLocationPath
 * [4] RelativeLocationPath ::= Step | RelativeLocationPath '/' Step | RelativeLocationPath '//' Step
 * [5] Step ::= '@'? NodeTest Predicate* | '.' | '..'
 * [6] NodeTest ::= NameTest | NodeType '(' ')'
 * [7] Predicate ::= '[' Expr ']'
 * [8] PrimaryExpr ::= '(' Expr ')' | Literal | Number | FunctionCall
 * [9] FunctionCall ::= FunctionName '(' ( Expr ( ',' Expr )* )? ')'
 * [10] PathExpr ::= LocationPath | PrimaryExpr Predicate*
 *                 | PrimaryExpr Predicate* '/' RelativeLocationPath
 *                 | PrimaryExpr Predicate* '//' RelativeLocationPath
 * [11] OrExpr ::= AndExpr | OrExpr 'or' AndExpr
 * [12] AndExpr ::= EqualityExpr | AndExpr 'and' EqualityExpr
 * [13] EqualityExpr ::= RelationalExpr | EqualityExpr '=' RelationalExpr
 *                     | EqualityExpr '!=' RelationalExpr
 * [14] RelationalExpr ::= AdditiveExpr
 *                       | RelationalExpr '<' AdditiveExpr
 *                       | RelationalExpr '>' AdditiveExpr
 *                       | RelationalExpr '<=' AdditiveExpr
 *                       | RelationalExpr '>=' AdditiveExpr
 * [15] AdditiveExpr ::= MultiplicativeExpr
 *                     | AdditiveExpr '+' MultiplicativeExpr
 *                     | AdditiveExpr '-' MultiplicativeExpr
 * [16] MultiplicativeExpr ::= UnaryExpr
 *                     | MultiplicativeExpr '*' UnaryExpr
 *                     | MultiplicativeExpr 'div' UnaryExpr
 *                     | MultiplicativeExpr 'mod' UnaryExpr
 * [17] UnaryExpr ::= UnionExpr | '-' UnaryExpr
 * [18] UnionExpr ::= PathExpr | UnionExpr '|' PathExpr
 */

/* expression tokens allocation */
#define LLLYXP_EXPR_SIZE_START 10
#define LLLYXP_EXPR_SIZE_STEP 5

/* XPath matches allocation */
#define LLLYXP_SET_SIZE_START 2
#define LLLYXP_SET_SIZE_STEP 2

/* building string when casting */
#define LLLYXP_STRING_CAST_SIZE_START 64
#define LLLYXP_STRING_CAST_SIZE_STEP 16

/**
 * @brief Tokens that can be in an XPath expression.
 */
enum lllyxp_token {
    LLLYXP_TOKEN_NONE = 0,
    LLLYXP_TOKEN_PAR1,          /* '(' */
    LLLYXP_TOKEN_PAR2,          /* ')' */
    LLLYXP_TOKEN_BRACK1,        /* '[' */
    LLLYXP_TOKEN_BRACK2,        /* ']' */
    LLLYXP_TOKEN_DOT,           /* '.' */
    LLLYXP_TOKEN_DDOT,          /* '..' */
    LLLYXP_TOKEN_AT,            /* '@' */
    LLLYXP_TOKEN_COMMA,         /* ',' */
    /* LLLYXP_TOKEN_DCOLON,      * '::' * axes not supported */
    LLLYXP_TOKEN_NAMETEST,      /* NameTest */
    LLLYXP_TOKEN_NODETYPE,      /* NodeType */
    LLLYXP_TOKEN_FUNCNAME,      /* FunctionName */
    LLLYXP_TOKEN_OPERATOR_LOG,  /* Operator 'and', 'or' */
    LLLYXP_TOKEN_OPERATOR_COMP, /* Operator '=', '!=', '<', '<=', '>', '>=' */
    LLLYXP_TOKEN_OPERATOR_MATH, /* Operator '+', '-', '*', 'div', 'mod', '-' (unary) */
    LLLYXP_TOKEN_OPERATOR_UNI,  /* Operator '|' */
    LLLYXP_TOKEN_OPERATOR_PATH, /* Operator '/', '//' */
    /* LLLYXP_TOKEN_AXISNAME,    * AxisName * axes not supported */
    LLLYXP_TOKEN_LITERAL,       /* Literal - with either single or double quote */
    LLLYXP_TOKEN_NUMBER         /* Number */
};

/**
 * @brief XPath (sub)expressions that can be repeated.
 */
enum lllyxp_expr_type {
    LLLYXP_EXPR_NONE = 0,
    LLLYXP_EXPR_OR,
    LLLYXP_EXPR_AND,
    LLLYXP_EXPR_EQUALITY,
    LLLYXP_EXPR_RELATIONAL,
    LLLYXP_EXPR_ADDITIVE,
    LLLYXP_EXPR_MULTIPLICATIVE,
    LLLYXP_EXPR_UNARY,
    LLLYXP_EXPR_UNION,
};

/**
 * @brief Structure holding a parsed XPath expression.
 */
struct lllyxp_expr {
    enum lllyxp_token *tokens; /* array of tokens */
    uint16_t *expr_pos;      /* array of pointers to the expression in expr (idx of the beginning) */
    uint16_t *tok_len;       /* array of token lengths in expr */
    enum lllyxp_expr_type **repeat; /* array of expression types that this token begins and is repeated ended with 0,
                                     more in the comment after this declaration */
    uint16_t used;           /* used array items */
    uint16_t size;           /* allocated array items */

    char *expr;              /* the original XPath expression */
};

/*
 * lllyxp_expr repeat
 *
 * This value is NULL for all the tokens that do not begin an
 * expression which can be repeated. Otherwise it is an array
 * of expression types that this token begins. These values
 * are used during evaluation to know whether we need to
 * duplicate the current context or not and to decide what
 * the current expression is (for example, if we are only
 * starting the parsing and the first token has no repeat,
 * we do not parse it as an OrExpr but directly as PathExpr).
 * Examples:
 *
 * Expression: "/ *[key1 and key2 or key1 < key2]"
 * Tokens: '/',  '*',  '[',  NameTest,  'and', NameTest, 'or', NameTest,        '<',  NameTest, ']'
 * Repeat: NULL, NULL, NULL, [AndExpr,  NULL,  NULL,     NULL, [RelationalExpr, NULL, NULL,     NULL
 *                            OrExpr,                           0],
 *                            0],
 *
 * Expression: "//node[key and node2]/key | /cont"
 * Tokens: '//',       'NameTest', '[',  'NameTest', 'and', 'NameTest', ']',  '/',  'NameTest', '|',  '/',  'NameTest'
 * Repeat: [UnionExpr, NULL,       NULL, [AndExpr,   NULL,  NULL,       NULL, NULL, NULL,       NULL, NULL, NULL
 *          0],                           0],
 *
 * Operators between expressions which this concerns:
 *     'or', 'and', '=', '!=', '<', '>', '<=', '>=', '+', '-', '*', 'div', 'mod', '|'
 */

/**
 * @brief Supported types of (partial) XPath results.
 */
enum lllyxp_set_type {
    LLLYXP_SET_EMPTY = 0,
    LLLYXP_SET_NODE_SET,
    LLLYXP_SET_SNODE_SET,
    LLLYXP_SET_BOOLEAN,
    LLLYXP_SET_NUMBER,
    LLLYXP_SET_STRING
};

#ifdef LLLY_ENABLED_CACHE

/**
 * @brief Item stored in an XPath set hash table.
 */
struct lllyxp_set_hash_node {
    struct lllyd_node *node;
    enum lllyxp_node_type type;
} _PACKED;

#endif

/**
 * @brief XPath set - (partial) result.
 */
struct lllyxp_set {
    enum lllyxp_set_type type;
    union {
        struct lllyxp_set_node {
            struct lllyd_node *node;
            enum lllyxp_node_type type;
            uint32_t pos;
        } *nodes;
        struct lllyxp_set_snode {
            struct lllys_node *snode;
            enum lllyxp_node_type type;
            /* 0 - snode was traversed, but not currently in the context,
             * 1 - snode currently in context,
             * 2 - snode in context and just added, so skip it for the current operation,
             * >=3 - snode is not in context because we are in a predicate and this snode was used/will be used later */
            uint32_t in_ctx;
        } *snodes;
        struct lllyxp_set_attr {
            struct lllyd_attr *attr;
            enum lllyxp_node_type type;
            uint32_t pos; /* if node_type is LLLYXP_SET_NODE_ATTR, it is the parent node position */
        } *attrs;
        char *str;
        long double num;
        int bool;
    } val;

    /* this is valid only for type LLLYXP_SET_NODE_SET and LLLYXP_SET_SNODE_SET */
    uint32_t used;
    uint32_t size;
#ifdef LLLY_ENABLED_CACHE
    struct hash_table *ht;
#endif
    /* this is valid only for type LLLYXP_SET_NODE_SET */
    uint32_t ctx_pos;
    uint32_t ctx_size;
};

/**
 * @brief Evaluate the XPath expression \p expr on data. Be careful when using this function, the result can often
 * be confusing without thorough understanding of XPath evaluation rules defined in RFC 6020.
 *
 * @param[in] expr XPath expression to evaluate. Must be in JSON format (prefixes are model names).
 * @param[in] cur_node Current (context) data node. If the node has #LLLYD_VAL_INUSE flag, it is considered dummy (intended
 * for but not restricted to evaluation with the LLLYXP_WHEN flag).
 * @param[in] cur_node_type Current (context) data node type. For every standard case use #LLLYXP_NODE_ELEM. But there are
 * cases when the context node \p cur_node is actually supposed to be the XML root, there is no such data node. So, in
 * this case just pass the first top-level node into \p cur_node and use an enum value for this kind of root
 * (#LLLYXP_NODE_ROOT_CONFIG if \p cur_node has config true, otherwise #LLLYXP_NODE_ROOT). #LLLYXP_NODE_TEXT and #LLLYXP_NODE_ATTR can also be used,
 * but there are no use-cases in YANG.
 * @param[in] local_mod Local module relative to the \p expr. Used only to determine the internal canonical value for identities.
 * @param[out] set Result set. Must be valid and in the same libyang context as \p cur_node.
 * To be safe, always either zero or cast the \p set to empty. After done using, either cast
 * the \p set to empty (if allocated statically) or free it (if allocated dynamically) to
 * prevent memory leaks.
 * @param[in] options Whether to apply some evaluation restrictions.
 * LLLYXP_MUST - apply must data tree access restrictions.
 * LLLYXP_WHEN - apply when data tree access restrictions and consider LLLYD_WHEN flags in data nodes.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on unresolved when dependency, -1 on error.
 */
int lllyxp_eval(const char *expr, const struct lllyd_node *cur_node, enum lllyxp_node_type cur_node_type,
              const struct lllys_module *local_mod, struct lllyxp_set *set, int options);

/**
 * @brief Get all the partial XPath nodes (atoms) that are required for \p expr to be evaluated.
 *
 * If any LLLYXP_SNODE* options is set, only fatal errors are printed, otherwise they are downgraded
 * to warnings.
 *
 * @param[in] expr XPath expression to be evaluated. Must be in JSON format (prefixes are model names).
 * @param[in] cur_snode Current (context) schema node.
 * @param[in] cur_snode_type Current (context) schema node type.
 * @param[out] set Result set. Must be valid and in the same libyang context as \p cur_snode.
 * To be safe, always either zero or cast the \p set to empty. After done using, either cast
 * the \p set to empty (if allocated statically) or free it (if allocated dynamically) to
 * prevent memory leaks.
 * @param[in] options Whether to apply some evaluation restrictions, one flag must always be used.
 * LLLYXP_SNODE - no special data tree access modifiers.
 * LLLYXP_SNODE_MUST - apply must data tree access restrictions.
 * LLLYXP_SNODE_WHEN - apply when data tree access restrictions.
 * LLLYXP_SNODE_OUTPUT - search RPC/action output instead input
 * @param[out] ctx_snode Actual context node for the expression (it often changes for "when" expressions).
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
int lllyxp_atomize(const char *expr, const struct lllys_node *cur_snode, enum lllyxp_node_type cur_snode_type,
                 struct lllyxp_set *set, int options, const struct lllys_node **ctx_snode);

/* these are used only internally */
#define LLLYXP_SNODE 0x04
#define LLLYXP_SNODE_MUST 0x08
#define LLLYXP_SNODE_WHEN 0x10
#define LLLYXP_SNODE_OUTPUT 0x20

#define LLLYXP_SNODE_ALL 0x3C

/**
 * @brief Works like lllyxp_atomize(), but it is executed on all the when and must expressions
 * which the node has.
 *
 * @param[in] node Node to examine.
 * @param[in,out] set Resulting set of atoms merged from all the expressions.
 * Will be cleared before use.
 * @param[in] set_ext_dep_flags Whether to set #LLLYS_XPCONF_DEP or #LLLYS_XPSTATE_DEP for conditions that
 * require foreign configuration or state subtree and also for the node itself, if it has any such condition.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
int lllyxp_node_atomize(const struct lllys_node *node, struct lllyxp_set *set, int set_ext_dep_flags);

/**
 * @brief Check syntax of all the XPath expressions of the node.
 *
 * @param[in] node Node to examine.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
int lllyxp_node_check_syntax(const struct lllys_node *node);

/**
 * @brief Cast XPath set to another type.
 *        Indirectly context position aware.
 *
 * @param[in] set Set to cast.
 * @param[in] target Target type to cast \p set into.
 * @param[in] cur_node Current (context) data node. Cannot be NULL.
 * @param[in] local_mod Local expression module.
 * @param[in] options Whether to apply some evaluation restrictions.
 *
 * @return EXIT_SUCCESS on success, -1 on error.
 */
int lllyxp_set_cast(struct lllyxp_set *set, enum lllyxp_set_type target, const struct lllyd_node *cur_node,
                  const struct lllys_module *local_mod, int options);

/**
 * @brief Free contents of an XPath \p set.
 *
 * @param[in] set Set to free.
 */
void lllyxp_set_free(struct lllyxp_set *set);

/**
 * @brief Parse an XPath expression into a structure of tokens.
 *        Logs directly.
 *
 * http://www.w3.org/TR/1999/REC-xpath-19991116/ section 3.7
 *
 * @param[in] ctx Context for errors.
 * @param[in] expr XPath expression to parse. It is duplicated.
 *
 * @return Filled expression structure or NULL on error.
 */
struct lllyxp_expr *lllyxp_parse_expr(struct llly_ctx *ctx, const char *expr);

/**
 * @brief Frees a parsed XPath expression. \p expr should not be used afterwards.
 *
 * @param[in] expr Expression to free.
 */
void lllyxp_expr_free(struct lllyxp_expr *expr);

#endif /* _XPATH_H */
