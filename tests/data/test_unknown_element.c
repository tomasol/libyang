/**
 * @file test_unknown_element.c
 * @author Justin Wilcox <justin.wilcox@adtran.com>
 * @brief Cmocka tests for unknown elements.
 *
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
    const char *schemafile = TESTS_DIR"/data/files/unknown-element.yin";

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
test_ok_strict(void **state)
{
    struct state *st = (*state);
    const char *xml = "<known-leaf xmlns=\"urn:libyang:tests:unknown-element\">X</known-leaf>";

    st->dt = lllyd_parse_mem(st->ctx, xml, LLLYD_XML, LLLYD_OPT_CONFIG | LLLYD_OPT_STRICT);
    assert_ptr_not_equal(st->dt, NULL);
}

static void
test_unknown_namespace_xml_strict(void **state)
{
    struct state *st = (*state);
    const char *xml = "<unknown-leaf xmlns=\"urn:libyang:tests:unknown-namespace\">X</unknown-leaf>";

    st->dt = lllyd_parse_mem(st->ctx, xml, LLLYD_XML, LLLYD_OPT_CONFIG | LLLYD_OPT_STRICT);
    assert_ptr_equal(st->dt, NULL);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_INELEM);
    assert_ptr_not_equal(llly_errpath(st->ctx), NULL);
    assert_string_equal(llly_errpath(st->ctx), "/");
}

static void
test_unknown_namespace_xml_nonstrict(void **state)
{
    struct state *st = (*state);
    const char *xml = "<unknown-leaf xmlns=\"urn:libyang:tests:unknown-namespace\">X</unknown-leaf>";

    st->dt = lllyd_parse_mem(st->ctx, xml, LLLYD_XML, LLLYD_OPT_CONFIG);
    assert_ptr_not_equal(st->dt, NULL);
}


static void
test_unknown_nested_element_xml_strict(void **state)
{
    struct state *st = (*state);
    const char *xml = "<known-container xmlns=\"urn:libyang:tests:unknown-element\">"
        "<unknown-subelement>X</unknown-subelement>"
        "</known-container>";

    st->dt = lllyd_parse_mem(st->ctx, xml, LLLYD_XML, LLLYD_OPT_CONFIG | LLLYD_OPT_STRICT);
    assert_ptr_equal(st->dt, NULL);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_INELEM);
    assert_ptr_not_equal(llly_errpath(st->ctx), NULL);
    assert_string_equal(llly_errpath(st->ctx), "/unknown-element:known-container");
}

static void
test_unknown_nested_element_xml_nonstrict(void **state)
{
    struct state *st = (*state);
    const char *xml = "<known-container xmlns=\"urn:libyang:tests:unknown-element\">"
        "<unknown-subelement>X</unknown-subelement>"
        "</known-container>";

    /* Non-strict still disallows unknown subelements */
    st->dt = lllyd_parse_mem(st->ctx, xml, LLLYD_XML, LLLYD_OPT_CONFIG);
    assert_ptr_not_equal(st->dt, NULL);
}


int main(void)
{
    const struct CMUnitTest tests[] = {
                    cmocka_unit_test_setup_teardown(test_ok_strict, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_unknown_namespace_xml_strict, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_unknown_namespace_xml_nonstrict, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_unknown_nested_element_xml_strict, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_unknown_nested_element_xml_nonstrict, setup_f, teardown_f),};

    return cmocka_run_group_tests(tests, NULL, NULL);
}
