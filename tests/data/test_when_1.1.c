/**
 * @file test_when_1.1.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief Cmocka tests for resolving YANG 1.1 when-stmt constraints.
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
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
    const struct lllys_module *mod;
    struct lllyd_node *dt;
    char *xml;
};

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
        goto error;
    }

    return 0;

error:
    llly_ctx_destroy(st->ctx, NULL);
    free(st);
    (*state) = NULL;

    return -1;
}

static int
teardown_f(void **state)
{
    struct state *st = (*state);

    lllyd_free_withsiblings(st->dt);
    llly_ctx_destroy(st->ctx, NULL);
    free(st->xml);
    free(st);
    (*state) = NULL;

    return 0;
}

static void
test_unlink_uses(void **state)
{
    struct state *st = (struct state *)*state;

    /* schema */
    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/when-unlink.yin", LLLYS_IN_YIN);
    assert_ptr_not_equal(st->mod, NULL);

    st->dt = lllyd_new_path(NULL, st->ctx, "/when-unlink:top/e", "val_e", 0, 0);
    assert_ptr_not_equal(st->dt, NULL);

    assert_int_equal(lllyd_validate(&(st->dt), LLLYD_OPT_CONFIG, NULL), 0);

    lllyd_print_mem(&(st->xml), st->dt, LLLYD_XML, LLLYP_WITHSIBLINGS);
    assert_string_equal(st->xml, "<top xmlns=\"urn:libyang:tests:when-unlink\"><e>val_e</e></top>");
}

static void
test_unlink_choice(void **state)
{
    struct state *st = (struct state *)*state;

    /* schema */
    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/when-unlink.yin", LLLYS_IN_YIN);
    assert_ptr_not_equal(st->mod, NULL);

    st->dt = lllyd_new_path(NULL, st->ctx, "/when-unlink:top/cas2", NULL, 0, 0);
    assert_ptr_not_equal(st->dt, NULL);

    assert_int_equal(lllyd_validate(&(st->dt), LLLYD_OPT_CONFIG, NULL), 0);

    lllyd_print_mem(&(st->xml), st->dt, LLLYD_XML, LLLYP_WITHSIBLINGS);
    assert_string_equal(st->xml, "<top xmlns=\"urn:libyang:tests:when-unlink\"><cas2/></top>");
}

static void
test_unlink_case(void **state)
{
    struct state *st = (struct state *)*state;
    struct lllyd_node *node;

    /* schema */
    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/when-unlink.yin", LLLYS_IN_YIN);
    assert_ptr_not_equal(st->mod, NULL);

    st->dt = lllyd_new_path(NULL, st->ctx, "/when-unlink:top/a", "val_a", 0, 0);
    assert_ptr_not_equal(st->dt, NULL);
    node = lllyd_new_path(st->dt, st->ctx, "/when-unlink:top/b", "val_b", 0, 0);
    assert_ptr_not_equal(node, NULL);

    assert_int_equal(lllyd_validate(&(st->dt), LLLYD_OPT_CONFIG, NULL), 0);

    lllyd_print_mem(&(st->xml), st->dt, LLLYD_XML, LLLYP_WITHSIBLINGS);
    assert_string_equal(st->xml, "<top xmlns=\"urn:libyang:tests:when-unlink\"><a>val_a</a><b>val_b</b></top>");
}

static void
test_unlink_augment(void **state)
{
    struct state *st = (struct state *)*state;
    struct lllyd_node *node;

    /* schema */
    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/when-unlink.yin", LLLYS_IN_YIN);
    assert_ptr_not_equal(st->mod, NULL);

    st->dt = lllyd_new_path(NULL, st->ctx, "/when-unlink:top/d", "1", 0, 0);
    assert_ptr_not_equal(st->dt, NULL);
    node = lllyd_new_path(st->dt, st->ctx, "/when-unlink:top/d", "2", 0, 0);
    assert_ptr_not_equal(node, NULL);

    assert_int_equal(lllyd_validate(&(st->dt), LLLYD_OPT_CONFIG, NULL), 0);

    lllyd_print_mem(&(st->xml), st->dt, LLLYD_XML, LLLYP_WITHSIBLINGS);
    assert_string_equal(st->xml, "<top xmlns=\"urn:libyang:tests:when-unlink\"><d>1</d><d>2</d></top>");
}

static void
test_dummy(void **state)
{
    struct state *st = (struct state *)*state;

    /* schema */
    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/when-dummy.yin", LLLYS_IN_YIN);
    assert_ptr_not_equal(st->mod, NULL);

    st->dt = lllyd_new_path(NULL, st->ctx, "/when-dummy:c", "value", 0, 0);
    assert_ptr_not_equal(st->dt, NULL);

    assert_int_equal(lllyd_validate(&(st->dt), LLLYD_OPT_CONFIG, NULL), 1);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_XPATH_DUMMY);
}

static void
test_dependency_noautodel(void **state)
{
    struct state *st = (struct state *)*state;
    struct lllyd_node *node;

    /* schema */
    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/when-depend.yin", LLLYS_IN_YIN);
    assert_ptr_not_equal(st->mod, NULL);

    st->dt = lllyd_new_path(NULL, st->ctx, "/when-depend:top/a", "val_a", 0, 0);
    assert_ptr_not_equal(st->dt, NULL);
    node = lllyd_new_path(st->dt, st->ctx, "/when-depend:top/b", "val_b", 0, 0);
    assert_ptr_not_equal(node, NULL);
    node = lllyd_new_path(st->dt, st->ctx, "/when-depend:top/d", "1", 0, 0);
    assert_ptr_not_equal(node, NULL);
    node = lllyd_new_path(st->dt, st->ctx, "/when-depend:top/d", "2", 0, 0);
    assert_ptr_not_equal(node, NULL);
    node = lllyd_new_path(st->dt, st->ctx, "/when-depend:top/e", "val_e", 0, 0);
    assert_ptr_not_equal(node, NULL);

    assert_int_equal(lllyd_validate(&(st->dt), LLLYD_OPT_CONFIG | LLLYD_OPT_WHENAUTODEL, NULL), 1);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_NOWHEN);
}

static void
test_dependency_circular(void **state)
{
    struct state *st = (struct state *)*state;
    struct lllyd_node *node;

    /* schema */
    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/when-circdepend.yin", LLLYS_IN_YIN);
    assert_ptr_not_equal(st->mod, NULL);

    st->dt = lllyd_new_path(NULL, st->ctx, "/when-circdepend:top/a", "val_a", 0, 0);
    assert_ptr_not_equal(st->dt, NULL);
    node = lllyd_new_path(st->dt, st->ctx, "/when-circdepend:top/b", "val_b", 0, 0);
    assert_ptr_not_equal(node, NULL);
    node = lllyd_new_path(st->dt, st->ctx, "/when-circdepend:top/d", "1", 0, 0);
    assert_ptr_not_equal(node, NULL);
    node = lllyd_new_path(st->dt, st->ctx, "/when-circdepend:top/d", "2", 0, 0);
    assert_ptr_not_equal(node, NULL);
    node = lllyd_new_path(st->dt, st->ctx, "/when-circdepend:top/e", "val_e", 0, 0);
    assert_ptr_not_equal(node, NULL);

    assert_int_equal(lllyd_validate(&(st->dt), LLLYD_OPT_CONFIG, NULL), 1);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_INWHEN);
}

static void
test_unlink_all(void **state)
{
    struct state *st = (struct state *)*state;

    /* schema */
    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/when-unlinkall.yin", LLLYS_IN_YIN);
    assert_ptr_not_equal(st->mod, NULL);

    st->dt = lllyd_new_path(NULL, st->ctx, "/when-unlinkall:a", "val_a", 0, 0);
    assert_ptr_not_equal(st->dt, NULL);

    assert_int_equal(lllyd_validate(&(st->dt), LLLYD_OPT_CONFIG, NULL), 0);

    lllyd_print_mem(&(st->xml), st->dt, LLLYD_XML, LLLYP_WITHSIBLINGS);
    assert_string_equal(st->xml, "<a xmlns=\"urn:libyang:tests:when-unlinkall\">val_a</a>");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
                    cmocka_unit_test_setup_teardown(test_unlink_uses, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_unlink_choice, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_unlink_case, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_unlink_augment, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_dummy, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_dependency_noautodel, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_dependency_circular, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_unlink_all, setup_f, teardown_f)
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
