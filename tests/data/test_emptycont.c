/**
 * @file test_emptycont.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Cmocka tests for auto delete of empty containers.
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
    const char *schemafile = TESTS_DIR"/data/files/emptycont.yin";

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

    /* schema */
    st->mod = lllys_parse_path(st->ctx, schemafile, LLLYS_IN_YIN);
    if (!st->mod) {
        fprintf(stderr, "Failed to load data model \"%s\".\n", schemafile);
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
test_parse(void **state)
{
    struct state *st = (*state);
    const char *xml = "<topleaf xmlns=\"urn:libyang:tests:emptycont\">X</topleaf>"
                      "<top xmlns=\"urn:libyang:tests:emptycont\"><a>A</a><b><b1>B</b1></b><c><c1>C</c1></c></top>";

    st->dt = lllyd_parse_mem(st->ctx, xml, LLLYD_XML, LLLYD_OPT_CONFIG);
    assert_ptr_not_equal(st->dt, NULL);

    lllyd_print_mem(&(st->xml), st->dt, LLLYD_XML, LLLYP_WITHSIBLINGS);
    assert_string_equal(st->xml, xml);
}

static void
test_parse_noautodel(void **state)
{
    struct state *st = (*state);
    const char *xml = "<topleaf xmlns=\"urn:libyang:tests:emptycont\">X</topleaf>"
                      "<top xmlns=\"urn:libyang:tests:emptycont\"><b><b1>B</b1></b><c><c1>C</c1></c></top>";

    st->dt = lllyd_parse_mem(st->ctx, xml, LLLYD_XML, LLLYD_OPT_CONFIG | LLLYD_OPT_WHENAUTODEL);
    assert_null(st->dt);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_NOWHEN);
}

static void
test_parse_autodel(void **state)
{
    struct state *st = (*state);
    const char *xml = "<topleaf xmlns=\"urn:libyang:tests:emptycont\">X</topleaf>"
                      "<top xmlns=\"urn:libyang:tests:emptycont\"><a>A</a></top>";

    /* all is fine, b container present */
    st->dt = lllyd_parse_mem(st->ctx, xml, LLLYD_XML, LLLYD_OPT_CONFIG);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_print_mem(&(st->xml), st->dt, LLLYD_XML, LLLYP_WITHSIBLINGS | LLLYP_WD_ALL);
    assert_string_equal(st->xml, "<topleaf xmlns=\"urn:libyang:tests:emptycont\">X</topleaf><top xmlns=\"urn:libyang:tests:emptycont\"><a>A</a><b/></top>");

    /* no need for the autodel flag, b container must always be autodeleted */
    assert_string_equal(st->dt->schema->name, "topleaf");
    st->dt = st->dt->next;
    lllyd_free(st->dt->prev);

    assert_int_equal(lllyd_validate(&st->dt, LLLYD_OPT_CONFIG, NULL), 0);
    free(st->xml);
    lllyd_print_mem(&(st->xml), st->dt, LLLYD_XML, LLLYP_WITHSIBLINGS | LLLYP_WD_ALL);
    assert_string_equal(st->xml, "<top xmlns=\"urn:libyang:tests:emptycont\"><a>A</a></top>");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
                    cmocka_unit_test_setup_teardown(test_parse, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_parse_noautodel, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_parse_autodel, setup_f, teardown_f),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
