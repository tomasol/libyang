/**
 * @file test_state_lists.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief Cmoka tests for handling state leaf-lists and lists.
 *
 * Copyright (c) 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <stdarg.h>
#include <cmocka.h>
#include <assert.h>

#include "libyang.h"
#include "tree_internal.h"
#include "tests/config.h"
#include "hash_table.h"

struct state {
    struct llly_ctx *ctx;
    struct lllyd_node *root1, *root2;
};

const char *schemafile = TESTS_DIR"/data/files/state-lists.yang";
const char *datafile = TESTS_DIR"/data/files/state-lists1.xml";

#ifdef LLLY_ENABLED_CACHE

static void
lllyd_hash_check(struct lllyd_node *node)
{
    struct lllyd_node *iter;
    uint32_t orig_hash, i;

    if (!node) {
        return;
    }

    orig_hash = node->hash;
    node->hash = 0;
    lllyd_hash(node);
    assert(node->hash == orig_hash);

    if (node->schema->nodetype & (LLLYS_CONTAINER | LLLYS_LIST | LLLYS_RPC | LLLYS_ACTION | LLLYS_NOTIF | LLLYS_INPUT | LLLYS_OUTPUT)) {
        for (i = 0, iter = node->child; iter; ++i, iter = iter->next) {
            if ((iter->schema->nodetype == LLLYS_LIST) && !lllyd_list_has_keys(iter)) {
                --i;
            }
        }

        if (i >= LLLY_CACHE_HT_MIN_CHILDREN) {
            assert(node->ht && (node->ht->used == i));
            LLLY_TREE_FOR(node->child, iter) {
                if ((iter->schema->nodetype != LLLYS_LIST) || lllyd_list_has_keys(iter)) {
                    assert(!lllyht_find(node->ht, &iter, iter->hash, NULL));
                }
            }
        } else {
            assert(!node->ht);
        }

        LLLY_TREE_FOR(node->child, iter) {
            lllyd_hash_check(iter);
        }
    }

    if ((node->schema->nodetype != LLLYS_LIST) || lllyd_list_has_keys(node)) {
        assert(node->hash);
    } else {
        assert(!node->hash);
    }
}

static void
test_hash(void **state)
{
    struct lllyd_node *root, *node;
    struct state *st = (*state);

    root = lllyd_new_path(NULL, st->ctx, "/state-lists:cont/l/leaf1", "cc", 0, 0);
    assert_non_null(root);
    lllyd_hash_check(root);

    node = lllyd_new_path(root, NULL, "/state-lists:cont/l[1]/lcont/l2/leaf4", "cc", 0, 0);
    assert_non_null(root);
    assert_string_equal(node->schema->name, "lcont");
    lllyd_hash_check(root);

    assert_int_equal(lllyd_insert(st->root1, root->child), 0);
    lllyd_free(root);
    lllyd_hash_check(st->root1);

    lllyd_free(st->root1->child->next->next->next);
    lllyd_hash_check(st->root1);

    lllyd_free(st->root1->child->child->next->next->child->next->child);
    lllyd_hash_check(st->root1);

    lllyd_free(st->root1->child->child->next);
    lllyd_hash_check(st->root1);
}

#endif

static int
setup_f(void **state)
{
    struct state *st;

    (*state) = st = calloc(1, sizeof *st);
    if (!st) {
        fprintf(stderr, "Memory allocation error");
        return -1;
    }

    /* libyang context */
    st->ctx = llly_ctx_new(NULL, 0);
    if (!st->ctx) {
        fprintf(stderr, "Failed to create context.\n");
        return -1;
    }

    /* schema */
    if (!lllys_parse_path(st->ctx, schemafile, LLLYS_IN_YANG)) {
        fprintf(stderr, "Failed to load data model \"%s\".\n", schemafile);
        return -1;
    }

    /* data */
    st->root1 = lllyd_parse_path(st->ctx, datafile, LLLYD_XML, LLLYD_OPT_GET);
    st->root2 = lllyd_dup(st->root1, 1);
    if (!st->root1 || !st->root2) {
        fprintf(stderr, "Failed to load initial data file.\n");
        return -1;
    }

    return 0;
}

static int
teardown_f(void **state)
{
    struct state *st = (*state);

#ifdef LLLY_ENABLED_CACHE
    lllyd_hash_check(st->root1);
    lllyd_hash_check(st->root2);
#endif
    lllyd_free(st->root1);
    lllyd_free(st->root2);
    llly_ctx_destroy(st->ctx, NULL);
    free(st);
    (*state) = NULL;

    return 0;
}

static void
test_merge_same(void **state)
{
    char *str1;
    struct state *st = (*state);
    const char str2[] =
    "<cont xmlns=\"urn:state-lists\">\n"
    "  <l>\n"
    "    <leaf1>aa</leaf1>\n"
    "    <leaf2>10</leaf2>\n"
    "    <lcont>\n"
    "      <leaf3/>\n"
    "      <l2>\n"
    "        <leaf4>aa</leaf4>\n"
    "        <leaf5>aa</leaf5>\n"
    "      </l2>\n"
    "    </lcont>\n"
    "  </l>\n"
    "  <l>\n"
    "    <leaf1>b</leaf1>\n"
    "    <leaf2>20</leaf2>\n"
    "    <lcont>\n"
    "      <l2>\n"
    "        <leaf5>bb</leaf5>\n"
    "      </l2>\n"
    "    </lcont>\n"
    "  </l>\n"
    "  <ll>abab</ll>\n"
    "  <ll>baba</ll>\n"
    "  <l/>\n"
    "  <l/>\n"
    "  <ll>abab</ll>\n"
    "  <ll>baba</ll>\n"
    "</cont>\n";

    /* merging 2 exact same data trees, the result should always be again the same data tree */
    assert_int_equal(lllyd_merge(st->root1, st->root2, 0), 0);

    lllyd_print_mem(&str1, st->root1, LLLYD_XML, LLLYP_FORMAT);
    assert_non_null(str1);

    assert_string_equal(str1, str2);
    free(str1);
}

static void
test_merge_equal_leaflist(void **state)
{
    struct lllyd_node *node;
    char *str1;
    struct state *st = (*state);
    const char str2[] =
    "<cont xmlns=\"urn:state-lists\">\n"
    "  <l>\n"
    "    <leaf1>aa</leaf1>\n"
    "    <leaf2>10</leaf2>\n"
    "    <lcont>\n"
    "      <leaf3/>\n"
    "      <l2>\n"
    "        <leaf4>aa</leaf4>\n"
    "        <leaf5>aa</leaf5>\n"
    "      </l2>\n"
    "    </lcont>\n"
    "  </l>\n"
    "  <l>\n"
    "    <leaf1>b</leaf1>\n"
    "    <leaf2>20</leaf2>\n"
    "    <lcont>\n"
    "      <l2>\n"
    "        <leaf5>bb</leaf5>\n"
    "      </l2>\n"
    "    </lcont>\n"
    "  </l>\n"
    "  <ll>abab</ll>\n"
    "  <ll>baba</ll>\n"
    "  <l/>\n"
    "  <l/>\n"
    "  <ll>abab</ll>\n"
    "  <ll>baba</ll>\n"
    "  <ll>abab</ll>\n"
    "</cont>\n";

    /* we added a leaf-list, an exact same one is already there */
    node = lllyd_new_path(st->root2, NULL, "/state-lists:cont/ll", "abab", 0, 0);
    assert_non_null(node);
    assert_string_equal(node->schema->name, "ll");

    assert_int_equal(lllyd_merge(st->root1, st->root2, 0), 0);

    lllyd_print_mem(&str1, st->root1, LLLYD_XML, LLLYP_FORMAT);
    assert_non_null(str1);

    assert_string_equal(str1, str2);
    free(str1);
}

static void
test_merge_equal_list(void **state)
{
    struct lllyd_node *node;
    char *str1;
    struct state *st = (*state);
    const char str2[] =
    "<cont xmlns=\"urn:state-lists\">\n"
    "  <l>\n"
    "    <leaf1>aa</leaf1>\n"
    "    <leaf2>10</leaf2>\n"
    "    <lcont>\n"
    "      <leaf3/>\n"
    "      <l2>\n"
    "        <leaf4>aa</leaf4>\n"
    "        <leaf5>aa</leaf5>\n"
    "      </l2>\n"
    "    </lcont>\n"
    "  </l>\n"
    "  <l>\n"
    "    <leaf1>b</leaf1>\n"
    "    <leaf2>20</leaf2>\n"
    "    <lcont>\n"
    "      <l2>\n"
    "        <leaf5>bb</leaf5>\n"
    "      </l2>\n"
    "    </lcont>\n"
    "  </l>\n"
    "  <ll>abab</ll>\n"
    "  <ll>baba</ll>\n"
    "  <l/>\n"
    "  <l/>\n"
    "  <ll>abab</ll>\n"
    "  <ll>baba</ll>\n"
    "  <l>\n"
    "    <leaf1>aa</leaf1>\n"
    "    <leaf2>10</leaf2>\n"
    "    <lcont>\n"
    "      <leaf3/>\n"
    "      <l2>\n"
    "        <leaf4>aa</leaf4>\n"
    "        <leaf5>aa</leaf5>\n"
    "      </l2>\n"
    "    </lcont>\n"
    "  </l>\n"
    "</cont>\n";

    /* we added a list, an exact same one is already there */
    node = lllyd_dup(st->root1->child, 1);
    assert_non_null(node);
    assert_string_equal(node->schema->name, "l");

    assert_int_equal(lllyd_insert(st->root2, node), 0);

    assert_int_equal(lllyd_merge(st->root1, st->root2, 0), 0);

    lllyd_print_mem(&str1, st->root1, LLLYD_XML, LLLYP_FORMAT);
    assert_non_null(str1);

    assert_string_equal(str1, str2);
    free(str1);
}

static void
test_merge_nonequal_list(void **state)
{
    struct lllyd_node *node;
    char *str1;
    struct state *st = (*state);
    const char str2[] =
    "<cont xmlns=\"urn:state-lists\">\n"
    "  <l>\n"
    "    <leaf1>aa</leaf1>\n"
    "    <leaf2>10</leaf2>\n"
    "    <lcont>\n"
    "      <leaf3/>\n"
    "      <l2>\n"
    "        <leaf4>aa</leaf4>\n"
    "        <leaf5>aa</leaf5>\n"
    "      </l2>\n"
    "    </lcont>\n"
    "  </l>\n"
    "  <l>\n"
    "    <leaf1>b</leaf1>\n"
    "    <leaf2>20</leaf2>\n"
    "    <lcont>\n"
    "      <l2>\n"
    "        <leaf5>bb</leaf5>\n"
    "      </l2>\n"
    "    </lcont>\n"
    "  </l>\n"
    "  <ll>abab</ll>\n"
    "  <ll>baba</ll>\n"
    "  <l/>\n"
    "  <l/>\n"
    "  <ll>abab</ll>\n"
    "  <ll>baba</ll>\n"
    "  <l>\n"
    "    <leaf1>b</leaf1>\n"
    "    <leaf2>20</leaf2>\n"
    "    <lcont>\n"
    "      <l2>\n"
    "        <leaf5>cc</leaf5>\n"
    "      </l2>\n"
    "    </lcont>\n"
    "  </l>\n"
    "</cont>\n";

    /* now one of the keyless lists is different, the whole instance should be in the diff */
    node = lllyd_dup(st->root1->child->next, 1);
    assert_non_null(node);
    assert_string_equal(node->schema->name, "l");

    assert_int_equal(lllyd_insert(st->root2, node), 0);

    node = lllyd_new_path(st->root2, NULL, "/state-lists:cont/l[5]/lcont/l2[1]/leaf5", "cc", 0, LLLYD_PATH_OPT_UPDATE);
    assert_non_null(node);
    assert_string_equal(node->schema->name, "leaf5");

    assert_int_equal(lllyd_merge(st->root1, st->root2, 0), 0);

    lllyd_print_mem(&str1, st->root1, LLLYD_XML, LLLYP_FORMAT);
    assert_non_null(str1);

    assert_string_equal(str1, str2);
    free(str1);
}

static void
test_diff_same(void **state)
{
    struct lllyd_difflist *diff;
    struct state *st = (*state);

    /* diffing 2 exact same data trees, the result should be no differences */
    diff = lllyd_diff(st->root1, st->root2, 0);
    assert_non_null(diff);
    assert_int_equal(diff->type[0], LLLYD_DIFF_END);
    lllyd_free_diff(diff);
}

static void
test_diff_equal_leaflist(void **state)
{
    struct lllyd_node *node;
    struct lllyd_difflist *diff;
    struct state *st = (*state);

    /* we added a leaf-list, an exact same one is already there */
    node = lllyd_new_path(st->root2, NULL, "/state-lists:cont/ll", "abab", 0, 0);
    assert_non_null(node);
    assert_string_equal(node->schema->name, "ll");

    diff = lllyd_diff(st->root1, st->root2, 0);
    assert_non_null(diff);
    assert_int_equal(diff->type[0], LLLYD_DIFF_CREATED);
    assert_string_equal(diff->second[0]->schema->name, "ll");
    assert_string_equal(((struct lllyd_node_leaf_list *)diff->second[0])->value_str, "abab");
    assert_int_equal(diff->type[1], LLLYD_DIFF_END);
    lllyd_free_diff(diff);
}

static void
test_diff_equal_list(void **state)
{
    struct lllyd_node *node;
    struct lllyd_difflist *diff;
    struct state *st = (*state);

    /* we added a list, an exact same one is already there */
    node = lllyd_dup(st->root1->child, 1);
    assert_non_null(node);
    assert_string_equal(node->schema->name, "l");

    assert_int_equal(lllyd_insert(st->root2, node), 0);

    diff = lllyd_diff(st->root1, st->root2, 0);
    assert_non_null(diff);
    assert_int_equal(diff->type[0], LLLYD_DIFF_CREATED);
    assert_string_equal(diff->second[0]->schema->name, "l");
    assert_int_equal(lllyd_list_pos(diff->second[0]), 5);
    assert_int_equal(diff->type[1], LLLYD_DIFF_END);
    lllyd_free_diff(diff);
}

static void
test_diff_nonequal_list(void **state)
{
    struct lllyd_node *node;
    struct lllyd_difflist *diff;
    struct state *st = (*state);

    /* now one of the keyless lists is different, the whole instance should be in the diff */
    node = lllyd_dup(st->root1->child->next, 1);
    assert_non_null(node);
    assert_string_equal(node->schema->name, "l");

    assert_int_equal(lllyd_insert(st->root2, node), 0);

    node = lllyd_new_path(st->root2, NULL, "/state-lists:cont/l[5]/lcont/l2[1]/leaf5", "cc", 0, LLLYD_PATH_OPT_UPDATE);
    assert_non_null(node);
    assert_string_equal(node->schema->name, "leaf5");

    diff = lllyd_diff(st->root1, st->root2, 0);
    assert_int_equal(diff->type[0], LLLYD_DIFF_CREATED);
    assert_string_equal(diff->second[0]->schema->name, "l");
    assert_int_equal(lllyd_list_pos(diff->second[0]), 5);
    assert_int_equal(diff->type[1], LLLYD_DIFF_END);
    lllyd_free_diff(diff);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
#ifdef LLLY_ENABLED_CACHE
                    cmocka_unit_test_setup_teardown(test_hash, setup_f, teardown_f),
#endif
                    cmocka_unit_test_setup_teardown(test_merge_same, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_merge_equal_leaflist, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_merge_equal_list, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_merge_nonequal_list, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_diff_same, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_diff_equal_leaflist, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_diff_equal_list, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_diff_nonequal_list, setup_f, teardown_f),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
