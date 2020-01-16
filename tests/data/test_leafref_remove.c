/**
 * @file test_leafref_remove.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Cmoka tests for correct removing leafref nodes from data tree.
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
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

#include "tests/config.h"
#include "libyang.h"

struct state {
    struct llly_ctx *ctx;
    struct lllyd_node *data;
};

static int
setup_f(void **state)
{
    struct state *st;
    const char *schemafile = TESTS_DIR"/data/files/leafrefs.yin";
    const char *datafile = TESTS_DIR"/data/files/leafrefs.xml";

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
    if (!lllys_parse_path(st->ctx, schemafile, LLLYS_IN_YIN)) {
        fprintf(stderr, "Failed to load data model \"%s\".\n", schemafile);
        return -1;
    }

    /* data */
    st->data = lllyd_parse_path(st->ctx, datafile, LLLYD_XML, LLLYD_OPT_CONFIG);
    if (!st->data) {
        fprintf(stderr, "Failed to load initial data file.\n");
        return -1;
    }

    return 0;
}

static int
teardown_f(void **state)
{
    struct state *st = (*state);

    lllyd_free(st->data);
    llly_ctx_destroy(st->ctx, NULL);
    free(st);
    (*state) = NULL;

    return 0;
}

static void
test_leafref_free(void **state)
{
    struct state *st = (*state);
    int r;

    lllyd_free(st->data->child->child->prev);
    r = lllyd_validate(&(st->data), LLLYD_OPT_CONFIG, NULL);
    assert_int_not_equal(r, 0);

    lllyd_new_leaf(st->data->child, NULL, "name", "jedna");
    r = lllyd_validate(&(st->data), LLLYD_OPT_CONFIG, NULL);
    assert_int_equal(r, 0);
}

static void
test_leafref_unlink(void **state)
{
    struct state *st = (*state);
    struct lllyd_node *node;
    int r;

    node = st->data->child->child->next;
    lllyd_unlink(node);
    r = lllyd_validate(&(st->data), LLLYD_OPT_CONFIG, NULL);
    assert_int_not_equal(r, 0);

    lllyd_insert(st->data->child, node);
    r = lllyd_validate(&(st->data), LLLYD_OPT_CONFIG, NULL);
    assert_int_equal(r, 0);
}

static void
test_leafref_unlink2(void **state)
{
    struct state *st = (*state);
    struct lllyd_node *node;
    int r;

    node = st->data->child;
    lllyd_unlink(node);
    r = lllyd_validate(&(st->data), LLLYD_OPT_CONFIG, NULL);
    assert_int_not_equal(r, 0);

    lllyd_insert(st->data, node);
    r = lllyd_validate(&(st->data), LLLYD_OPT_CONFIG, NULL);
    assert_int_equal(r, 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
                    cmocka_unit_test_setup_teardown(test_leafref_free, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_leafref_unlink, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_leafref_unlink2, setup_f, teardown_f), };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
