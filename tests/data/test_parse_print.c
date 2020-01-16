/**
 * @file test_parse_print.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief Cmocka tests for parsing and printing both schema and data.
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include <stdarg.h>
#include <cmocka.h>

#include "tests/config.h"
#include "libyang.h"

struct state {
    struct llly_ctx *ctx;
    const struct lllys_module *mod;
    struct lllyd_node *dt;
    struct lllyd_node *rpc_act;
    int fd;
    char *str1;
    char *str2;
};

static int
setup_f(void **state)
{
    struct state *st;
    const char *schema = TESTS_DIR"/data/files/all.yin";
    const char *schemaimp = TESTS_DIR"/data/files/all-imp.yin";
    const char *schemadev = TESTS_DIR"/data/files/all-dev.yin";

    (*state) = st = calloc(1, sizeof *st);
    if (!st) {
        fprintf(stderr, "Memory allocation error");
        return -1;
    }

    /* libyang context */
    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    if (!st->ctx) {
        fprintf(stderr, "Failed to create context.\n");
        goto error;
    }

    /* schema */
    st->mod = lllys_parse_path(st->ctx, schema, LLLYS_IN_YIN);
    if (!st->mod) {
        fprintf(stderr, "Failed to load data model \"%s\".\n", schema);
        goto error;
    }
    lllys_features_enable(st->mod, "feat2");
    lllys_features_enable(st->mod, "*");

    st->mod = lllys_parse_path(st->ctx, schemaimp, LLLYS_IN_YIN);
    if (!st->mod) {
        fprintf(stderr, "Failed to load data model \"%s\".\n", schemaimp);
        goto error;
    }

    st->mod = lllys_parse_path(st->ctx, schemadev, LLLYS_IN_YIN);
    if (!st->mod) {
        fprintf(stderr, "Failed to load data model \"%s\".\n", schemadev);
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
    lllyd_free_withsiblings(st->rpc_act);
    llly_ctx_destroy(st->ctx, NULL);
    if (st->fd > 0) {
        close(st->fd);
    }
    free(st->str1);
    free(st->str2);
    free(st);
    (*state) = NULL;

    return 0;
}

static void
test_parse_print_yin_error_prefix(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-missing-prefix.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-dup-prefix.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-order-prefix.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_contact(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-order-contact.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-dup-contact.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement-contact.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_organization(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-order-organization.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-dup-organization.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement-organization.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_description(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-order-description.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-dup-description.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement-description.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_reference(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-order-reference.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-dup-reference.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement-reference.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_yang_version(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-order-yang-version.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-dup-yang-version.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_namespace(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-missing-xmlns.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-dup-namespace.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-order-namespace.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_when(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-when.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-when.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-when.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-when.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement5-when.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_container(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-container.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-container.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_leaflist(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-leaflist.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-leaflist.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-leaflist.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-leaflist.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement5-leaflist.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_leaf(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-leaf.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-leaf.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-leaf.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-leaf.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_list(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-list.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-list.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-list.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-list.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement5-list.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement6-list.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement7-list.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement8-list.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement9-list.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_choice(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-choice.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-choice.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-choice.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-choice.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement5-choice.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement6-choice.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement7-choice.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement8-choice.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement9-choice.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement10-choice.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_uses(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-uses.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-uses.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-uses.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-uses.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement5-uses.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_anydata(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-anydata.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-anydata.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-anydata.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-anydata.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement5-anydata.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_rpc(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-rpc.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-rpc.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-rpc.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-rpc.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_action(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-action.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_notification(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-notification.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-notification.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-notification.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-notification.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement5-notification.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_augment(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-augment.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-augment.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_grouping(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-grouping.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-grouping.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_revision(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-order-revision.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-revision-not-unique.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-revision.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-revision.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-revision.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-revision.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement5-revision.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_extension(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-extension.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-extension.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-extension.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-extension.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_import(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-order-import.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-import.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-import.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-import.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-import.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement5-import.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement6-import.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_include(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-order-include.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-include.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-include.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-include.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-include.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement5-include.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement6-include.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement7-include.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_identity(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-identity.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-identity.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_feature(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-feature.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);
}

static void
test_parse_print_yin_error_deviation(void **state)
{
    struct state *st = (*state);

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement1-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement2-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement3-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement4-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement5-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement6-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement7-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement8-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement9-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement10-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement11-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement12-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement13-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement14-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement15-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement16-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement17-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement18-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement19-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement20-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement21-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement22-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement23-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement24-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement25-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement26-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement27-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement28-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement29-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement30-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement31-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement32-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/error-format/error-substatement33-deviation.yin", LLLYS_IN_YIN);
    assert_ptr_equal(st->mod, NULL);

}

static void
test_parse_print_yin(void **state)
{
    struct state *st = (*state);
    struct stat s;
    int fd;

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/all.yin", LLLYS_IN_YIN);
    assert_ptr_not_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/all-dev.yin", LLLYS_IN_YIN);
    assert_ptr_not_equal(st->mod, NULL);

    fd = open(TESTS_DIR"/data/files/all-dev.yin", O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    lllys_print_mem(&(st->str2), st->mod, LLLYS_OUT_YIN, NULL, 0, 0);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;

    st->mod = llly_ctx_get_module(st->ctx, "all", NULL, 0);
    assert_ptr_not_equal(st->mod, NULL);

    fd = open(TESTS_DIR"/data/files/all.yin", O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    lllys_print_mem(&(st->str2), st->mod, LLLYS_OUT_YIN, NULL, 0, 0);

    assert_string_equal(st->str1, st->str2);
}

void
test_parse_print_yang(void **state)
{
    struct state *st = (*state);
    struct stat s;
    int fd;

    *state = st = calloc(1, sizeof *st);
    assert_ptr_not_equal(st, NULL);

    st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0);
    assert_ptr_not_equal(st->ctx, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/all.yang", LLLYS_IN_YANG);
    assert_ptr_not_equal(st->mod, NULL);

    st->mod = lllys_parse_path(st->ctx, TESTS_DIR"/data/files/all-dev.yang", LLLYS_IN_YANG);
    assert_ptr_not_equal(st->mod, NULL);

    fd = open(TESTS_DIR"/data/files/all-dev.yang", O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    lllys_print_mem(&(st->str2), st->mod, LLLYS_OUT_YANG, NULL, 0, 0);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;

    st->mod = llly_ctx_get_module(st->ctx, "all", NULL, 0);
    assert_ptr_not_equal(st->mod, NULL);

    fd = open(TESTS_DIR"/data/files/all.yang", O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    lllys_print_mem(&(st->str2), st->mod, LLLYS_OUT_YANG, NULL, 0, 0);

    assert_string_equal(st->str1, st->str2);
}

static void
test_parse_print_xml(void **state)
{
    struct state *st = (*state);
    struct stat s;
    struct llly_set *set;
    const struct lllys_module *mod;
    int fd;
    const char *data = TESTS_DIR"/data/files/all-data.xml";
    const char *rpc = TESTS_DIR"/data/files/all-rpc.xml";
    const char *rpcreply = TESTS_DIR"/data/files/all-rpcreply.xml";
    const char *act = TESTS_DIR"/data/files/all-act.xml";
    const char *actreply = TESTS_DIR"/data/files/all-actreply.xml";
    const char *notif = TESTS_DIR"/data/files/all-notif.xml";
    const char *innotif = TESTS_DIR"/data/files/all-innotif.xml";

    /* data */
    fd = open(data, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->dt = lllyd_parse_path(st->ctx, data, LLLYD_XML, LLLYD_OPT_CONFIG | LLLYD_OPT_STRICT);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_print_mem(&(st->str2), st->dt, LLLYD_XML, LLLYP_FORMAT);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;
    lllyd_free(st->dt);
    st->dt = NULL;

    /* rpc */
    fd = open(rpc, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->rpc_act = lllyd_parse_path(st->ctx, rpc, LLLYD_XML, LLLYD_OPT_RPC, NULL);
    assert_ptr_not_equal(st->rpc_act, NULL);
    lllyd_print_mem(&(st->str2), st->rpc_act, LLLYD_XML, LLLYP_FORMAT);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;

    /* rpcreply */
    fd = open(rpcreply, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    mod = llly_ctx_get_module(st->ctx, "all", NULL, 1);
    assert_ptr_not_equal(mod, NULL);
    set = lllys_find_path(mod, NULL, "/rpc1");
    assert_ptr_not_equal(set, NULL);
    assert_int_equal(set->set.s[0]->nodetype, LLLYS_RPC);
    llly_set_free(set);

    st->dt = lllyd_parse_path(st->ctx, rpcreply, LLLYD_XML, LLLYD_OPT_RPCREPLY, st->rpc_act, NULL);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_print_mem(&(st->str2), st->dt, LLLYD_XML, LLLYP_FORMAT | LLLYP_NETCONF);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;
    lllyd_free(st->dt);
    st->dt = NULL;
    lllyd_free(st->rpc_act);
    st->rpc_act = NULL;

    /* act */
    fd = open(act, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->rpc_act = lllyd_parse_path(st->ctx, act, LLLYD_XML, LLLYD_OPT_RPC, NULL);
    assert_ptr_not_equal(st->rpc_act, NULL);
    lllyd_print_mem(&(st->str2), st->rpc_act, LLLYD_XML, LLLYP_FORMAT | LLLYP_NETCONF);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;

    /* actreply */
    fd = open(actreply, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    set = lllys_find_path(mod, NULL, "/cont1/list1/act1");
    assert_ptr_not_equal(set, NULL);
    assert_int_equal(set->set.s[0]->nodetype, LLLYS_ACTION);
    llly_set_free(set);

    st->dt = lllyd_parse_path(st->ctx, actreply, LLLYD_XML, LLLYD_OPT_RPCREPLY, st->rpc_act, NULL);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_print_mem(&(st->str2), st->dt, LLLYD_XML, LLLYP_FORMAT | LLLYP_NETCONF);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;
    lllyd_free(st->dt);
    st->dt = NULL;
    lllyd_free(st->rpc_act);
    st->rpc_act = NULL;

    /* notif */
    fd = open(notif, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->dt = lllyd_parse_path(st->ctx, notif, LLLYD_XML, LLLYD_OPT_NOTIF, NULL);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_print_mem(&(st->str2), st->dt, LLLYD_XML, LLLYP_FORMAT);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;
    lllyd_free(st->dt);
    st->dt = NULL;

    /* inline notif */
    fd = open(innotif, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->dt = lllyd_parse_path(st->ctx, innotif, LLLYD_XML, LLLYD_OPT_NOTIF, NULL);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_print_mem(&(st->str2), st->dt, LLLYD_XML, LLLYP_FORMAT);

    assert_string_equal(st->str1, st->str2);
}

static void
test_parse_print_json(void **state)
{
    struct state *st = (*state);
    struct stat s;
    const struct lllys_module *mod;
    struct llly_set *set;
    int fd;
    const char *data = TESTS_DIR"/data/files/all-data.json";
    const char *rpc = TESTS_DIR"/data/files/all-rpc.json";
    const char *rpcreply = TESTS_DIR"/data/files/all-rpcreply.json";
    const char *act = TESTS_DIR"/data/files/all-act.json";
    const char *actreply = TESTS_DIR"/data/files/all-actreply.json";
    const char *notif = TESTS_DIR"/data/files/all-notif.json";
    const char *innotif = TESTS_DIR"/data/files/all-innotif.json";

    /* data */
    fd = open(data, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->dt = lllyd_parse_path(st->ctx, data, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_print_mem(&(st->str2), st->dt, LLLYD_JSON, LLLYP_FORMAT);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;
    lllyd_free(st->dt);
    st->dt = NULL;

    /* rpc */
    fd = open(rpc, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->rpc_act = lllyd_parse_path(st->ctx, rpc, LLLYD_JSON, LLLYD_OPT_RPC, NULL);
    assert_ptr_not_equal(st->rpc_act, NULL);
    lllyd_print_mem(&(st->str2), st->rpc_act, LLLYD_JSON, LLLYP_FORMAT | LLLYP_NETCONF);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;

    /* rpcreply */
    fd = open(rpcreply, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    mod = llly_ctx_get_module(st->ctx, "all", NULL, 1);
    assert_ptr_not_equal(mod, NULL);
    set = lllys_find_path(mod, NULL, "/rpc1");
    assert_ptr_not_equal(set, NULL);
    assert_int_equal(set->set.s[0]->nodetype, LLLYS_RPC);
    llly_set_free(set);

    st->dt = lllyd_parse_path(st->ctx, rpcreply, LLLYD_JSON, LLLYD_OPT_RPCREPLY, st->rpc_act, NULL);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_print_mem(&(st->str2), st->dt, LLLYD_JSON, LLLYP_FORMAT | LLLYP_NETCONF);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;
    lllyd_free(st->dt);
    st->dt = NULL;
    lllyd_free(st->rpc_act);
    st->rpc_act = NULL;

    /* act */
    fd = open(act, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->rpc_act = lllyd_parse_path(st->ctx, act, LLLYD_JSON, LLLYD_OPT_RPC, NULL);
    assert_ptr_not_equal(st->rpc_act, NULL);
    lllyd_print_mem(&(st->str2), st->rpc_act, LLLYD_JSON, LLLYP_FORMAT | LLLYP_NETCONF);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;

    /* actreply */
    fd = open(actreply, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    set = lllys_find_path(mod, NULL, "/all:cont1/list1/act1");
    assert_ptr_not_equal(set, NULL);
    assert_int_equal(set->set.s[0]->nodetype, LLLYS_ACTION);
    llly_set_free(set);

    st->dt = lllyd_parse_path(st->ctx, actreply, LLLYD_JSON, LLLYD_OPT_RPCREPLY, st->rpc_act, NULL);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_print_mem(&(st->str2), st->dt, LLLYD_JSON, LLLYP_FORMAT | LLLYP_NETCONF);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;
    lllyd_free(st->dt);
    st->dt = NULL;
    lllyd_free(st->rpc_act);
    st->rpc_act = NULL;

    /* notif */
    fd = open(notif, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->dt = lllyd_parse_path(st->ctx, notif, LLLYD_JSON, LLLYD_OPT_NOTIF, NULL);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_print_mem(&(st->str2), st->dt, LLLYD_JSON, LLLYP_FORMAT);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;
    lllyd_free(st->dt);
    st->dt = NULL;
    lllyd_free(st->rpc_act);
    st->rpc_act = NULL;

    /* inline notif */
    fd = open(innotif, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->dt = lllyd_parse_path(st->ctx, innotif, LLLYD_JSON, LLLYD_OPT_NOTIF, NULL);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_print_mem(&(st->str2), st->dt, LLLYD_JSON, LLLYP_FORMAT);

    assert_string_equal(st->str1, st->str2);
}

static void test_parse_print_keyless(void **state)
{
    struct state *st = (*state);
    struct stat s;
    int fd;
    const char *yang = TESTS_DIR"/data/files/keyless.yang";
    const char *json = TESTS_DIR"/data/files/keyless.json";
    const char *xml = TESTS_DIR"/data/files/keyless.xml";


    llly_ctx_destroy(st->ctx, NULL);
    assert_non_null(st->ctx = llly_ctx_new(TESTS_DIR"/data/files", 0));
    assert_non_null(st->mod = lllys_parse_path(st->ctx, yang, LLLYS_IN_YANG));

    /* keyless list - JSON */
    fd = open(json, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->dt = lllyd_parse_path(st->ctx, json, LLLYD_JSON, LLLYD_OPT_DATA | LLLYD_OPT_DATA_NO_YANGLIB, NULL);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_print_mem(&(st->str2), st->dt, LLLYD_JSON, LLLYP_FORMAT);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;
    lllyd_free_withsiblings(st->dt);
    st->dt = NULL;

    /* keyless list - XML */
    fd = open(xml, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->dt = lllyd_parse_path(st->ctx, xml, LLLYD_XML, LLLYD_OPT_DATA | LLLYD_OPT_DATA_NO_YANGLIB, NULL);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_print_mem(&(st->str2), st->dt, LLLYD_XML, LLLYP_FORMAT);

    assert_string_equal(st->str1, st->str2);
}

static void
test_parse_print_lyb(void **state)
{
    struct state *st = (*state);
    char *str;
    struct stat s;
    const struct lllys_module *mod;
    struct llly_set *set;
    int fd;
    const char *data = TESTS_DIR"/data/files/all-data.json";
    const char *rpc = TESTS_DIR"/data/files/all-rpc.json";
    const char *rpcreply = TESTS_DIR"/data/files/all-rpcreply.json";
    const char *act = TESTS_DIR"/data/files/all-act.json";
    const char *actreply = TESTS_DIR"/data/files/all-actreply.json";
    const char *notif = TESTS_DIR"/data/files/all-notif.json";
    const char *innotif = TESTS_DIR"/data/files/all-innotif.json";

    /* data */
    fd = open(data, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->dt = lllyd_parse_path(st->ctx, data, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_not_equal(st->dt, NULL);

    lllyd_print_mem(&str, st->dt, LLLYD_LYB, 0);
    lllyd_free(st->dt);
    st->dt = lllyd_parse_mem(st->ctx, str, LLLYD_LYB, LLLYD_OPT_CONFIG);
    free(str);

    lllyd_print_mem(&(st->str2), st->dt, LLLYD_JSON, LLLYP_FORMAT);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;
    lllyd_free(st->dt);
    st->dt = NULL;

    /* rpc */
    fd = open(rpc, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->rpc_act = lllyd_parse_path(st->ctx, rpc, LLLYD_JSON, LLLYD_OPT_RPC, NULL);
    assert_ptr_not_equal(st->rpc_act, NULL);

    lllyd_print_mem(&str, st->rpc_act, LLLYD_LYB, 0);
    lllyd_free(st->rpc_act);
    st->rpc_act = lllyd_parse_mem(st->ctx, str, LLLYD_LYB, LLLYD_OPT_RPC, NULL);
    free(str);

    lllyd_print_mem(&(st->str2), st->rpc_act, LLLYD_JSON, LLLYP_FORMAT | LLLYP_NETCONF);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;

    /* rpcreply */
    fd = open(rpcreply, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    mod = llly_ctx_get_module(st->ctx, "all", NULL, 1);
    assert_ptr_not_equal(mod, NULL);
    set = lllys_find_path(mod, NULL, "/rpc1");
    assert_ptr_not_equal(set, NULL);
    assert_int_equal(set->set.s[0]->nodetype, LLLYS_RPC);
    llly_set_free(set);

    st->dt = lllyd_parse_path(st->ctx, rpcreply, LLLYD_JSON, LLLYD_OPT_RPCREPLY, st->rpc_act, NULL);
    assert_ptr_not_equal(st->dt, NULL);

    lllyd_print_mem(&str, st->dt, LLLYD_LYB, 0);
    lllyd_free(st->dt);
    st->dt = lllyd_parse_mem(st->ctx, str, LLLYD_LYB, LLLYD_OPT_RPCREPLY, st->rpc_act, NULL);
    free(str);

    lllyd_print_mem(&(st->str2), st->dt, LLLYD_JSON, LLLYP_FORMAT | LLLYP_NETCONF);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;
    lllyd_free(st->dt);
    st->dt = NULL;
    lllyd_free(st->rpc_act);
    st->rpc_act = NULL;

    /* act */
    fd = open(act, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->rpc_act = lllyd_parse_path(st->ctx, act, LLLYD_JSON, LLLYD_OPT_RPC, NULL);
    assert_ptr_not_equal(st->rpc_act, NULL);

    lllyd_print_mem(&str, st->rpc_act, LLLYD_LYB, 0);
    lllyd_free(st->rpc_act);
    st->rpc_act = lllyd_parse_mem(st->ctx, str, LLLYD_LYB, LLLYD_OPT_RPC, NULL);
    free(str);

    lllyd_print_mem(&(st->str2), st->rpc_act, LLLYD_JSON, LLLYP_FORMAT | LLLYP_NETCONF);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;

    /* actreply */
    fd = open(actreply, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    set = lllys_find_path(mod, NULL, "/all:cont1/list1/act1");
    assert_ptr_not_equal(set, NULL);
    assert_int_equal(set->set.s[0]->nodetype, LLLYS_ACTION);
    llly_set_free(set);

    st->dt = lllyd_parse_path(st->ctx, actreply, LLLYD_JSON, LLLYD_OPT_RPCREPLY, st->rpc_act, NULL);
    assert_ptr_not_equal(st->dt, NULL);

    lllyd_print_mem(&str, st->dt, LLLYD_LYB, 0);
    lllyd_free(st->dt);
    st->dt = lllyd_parse_mem(st->ctx, str, LLLYD_LYB, LLLYD_OPT_RPCREPLY, st->rpc_act, NULL);
    free(str);

    lllyd_print_mem(&(st->str2), st->dt, LLLYD_JSON, LLLYP_FORMAT | LLLYP_NETCONF);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;
    lllyd_free(st->dt);
    st->dt = NULL;
    lllyd_free(st->rpc_act);
    st->rpc_act = NULL;

    /* notif */
    fd = open(notif, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->dt = lllyd_parse_path(st->ctx, notif, LLLYD_JSON, LLLYD_OPT_NOTIF, NULL);
    assert_ptr_not_equal(st->dt, NULL);

    lllyd_print_mem(&str, st->dt, LLLYD_LYB, 0);
    lllyd_free(st->dt);
    st->dt = lllyd_parse_mem(st->ctx, str, LLLYD_LYB, LLLYD_OPT_NOTIF, NULL);
    free(str);

    lllyd_print_mem(&(st->str2), st->dt, LLLYD_JSON, LLLYP_FORMAT);

    assert_string_equal(st->str1, st->str2);

    close(fd);
    fd = -1;
    free(st->str1);
    st->str1 = NULL;
    free(st->str2);
    st->str2 = NULL;
    lllyd_free(st->dt);
    st->dt = NULL;

    /* inline notif */
    fd = open(innotif, O_RDONLY);
    fstat(fd, &s);
    st->str1 = malloc(s.st_size + 1);
    assert_ptr_not_equal(st->str1, NULL);
    assert_int_equal(read(fd, st->str1, s.st_size), s.st_size);
    st->str1[s.st_size] = '\0';

    st->dt = lllyd_parse_path(st->ctx, innotif, LLLYD_JSON, LLLYD_OPT_NOTIF, NULL);
    assert_ptr_not_equal(st->dt, NULL);

    lllyd_print_mem(&str, st->dt, LLLYD_LYB, 0);
    lllyd_free(st->dt);
    st->dt = lllyd_parse_mem(st->ctx, str, LLLYD_LYB, LLLYD_OPT_NOTIF, NULL);
    free(str);

    lllyd_print_mem(&(st->str2), st->dt, LLLYD_JSON, LLLYP_FORMAT);

    assert_string_equal(st->str1, st->str2);
}

static void
test_parse_print_oookeys_xml(void **state)
{
    struct state *st = (*state);
    const char *xmlin = "<cont1 xmlns=\"urn:all\">"
                          "<leaf3>-1</leaf3>"
                          "<list1><leaf18>aaa</leaf18></list1>"
                          "<list1><leaf19>123</leaf19><leaf18>bbb</leaf18></list1>"
                        "</cont1>";
    const char *xmlout = "<cont1 xmlns=\"urn:all\">"
                          "<leaf3>-1</leaf3>"
                          "<list1><leaf18>aaa</leaf18></list1>"
                          "<list1><leaf18>bbb</leaf18><leaf19>123</leaf19></list1>"
                        "</cont1>";
    st->dt = NULL;

    /* with strict parsing, it is error since the key is not encoded as the first child */
    st->dt = lllyd_parse_mem(st->ctx, xmlin, LLLYD_XML, LLLYD_OPT_CONFIG | LLLYD_OPT_STRICT);
    assert_ptr_equal(st->dt, NULL);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_INORDER);
    assert_string_equal(llly_errmsg(st->ctx), "Invalid position of the key \"leaf18\" in a list \"list1\".");

    /* without strict, it produces only warning, but the data are correctly loaded */
    st->dt = lllyd_parse_mem(st->ctx, xmlin, LLLYD_XML, LLLYD_OPT_CONFIG);
    assert_ptr_not_equal(st->dt, NULL);
    assert_int_equal(lllyd_print_mem(&st->str1, st->dt, LLLYD_XML, 0), 0);
    assert_string_equal(st->str1, xmlout);
}

static void
test_parse_print_oookeys_json(void **state)
{
    struct state *st = (*state);
    const char *in = "{\"all:cont1\":{\"leaf3\":-1,\"list1\":[{\"leaf18\":\"a\"},{\"leaf19\":123,\"leaf18\":\"b\"}]}}";
    const char *out = "{\"all:cont1\":{\"leaf3\":-1,\"list1\":[{\"leaf18\":\"a\"},{\"leaf18\":\"b\",\"leaf19\":123}]}}";

    st->dt = NULL;

    /* in JSON, ordering does not matter, so it will succeed even with strict */
    st->dt = lllyd_parse_mem(st->ctx, in, LLLYD_JSON, LLLYD_OPT_CONFIG | LLLYD_OPT_STRICT);
    assert_ptr_not_equal(st->dt, NULL);
    assert_int_equal(lllyd_print_mem(&st->str1, st->dt, LLLYD_JSON, 0), 0);
    assert_string_equal(st->str1, out);
}

static void
test_parse_noncharacters_xml(void **state)
{
    struct state *st;
    const char* mod = "module x {namespace urn:x; prefix x; leaf x { type string;}}";
    const char* data = "<x xmlns=\"urn:x\">----------</x>";

    assert_ptr_not_equal(((*state) = st = calloc(1, sizeof *st)), NULL);
    assert_ptr_not_equal((st->ctx = llly_ctx_new(NULL, 0)), NULL);

    /* test detection of invalid characters according to RFC 7950, sec 9.4 */
    assert_ptr_not_equal(lllys_parse_mem(st->ctx, mod, LLLYS_IN_YANG), 0);
    assert_ptr_not_equal((st->str1 = strdup(data)), NULL);

    /* exclude surrogate blocks 0xD800-DFFF - trying 0xd800 */
    st->str1[17] = 0xed;
    st->str1[18] = 0xa0;
    st->str1[19] = 0x80;
    assert_ptr_equal(lllyd_parse_mem(st->ctx, st->str1, LLLYD_XML, LLLYD_OPT_CONFIG), NULL);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_XML_INCHAR);
    assert_string_equal(llly_errmsg(st->ctx), "Invalid UTF-8 value 0x0000d800");

    /* exclude noncharacters %xFDD0-FDEF - trying 0xfdd0 */
    st->str1[17] = 0xef;
    st->str1[18] = 0xb7;
    st->str1[19] = 0x90;
    assert_ptr_equal(lllyd_parse_mem(st->ctx, st->str1, LLLYD_XML, LLLYD_OPT_CONFIG), NULL);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_XML_INCHAR);
    assert_string_equal(llly_errmsg(st->ctx), "Invalid UTF-8 value 0x0000fdd0");

    /* exclude noncharacters %xFFFE-FFFF - trying 0xfffe */
    st->str1[17] = 0xef;
    st->str1[18] = 0xbf;
    st->str1[19] = 0xbe;
    assert_ptr_equal(lllyd_parse_mem(st->ctx, st->str1, LLLYD_XML, LLLYD_OPT_CONFIG), NULL);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_XML_INCHAR);
    assert_string_equal(llly_errmsg(st->ctx), "Invalid UTF-8 value 0x0000fffe");

    /* exclude c0 control characters except tab, carriage return and line feed */
    st->str1[17] = 0x9; /* valid - horizontal tab */
    st->str1[18] = 0xa; /* valid - new line */
    st->str1[19] = 0xd; /* valid - carriage return */
    st->str1[20] = 0x6; /* invalid - ack */
    assert_ptr_equal(lllyd_parse_mem(st->ctx, st->str1, LLLYD_XML, LLLYD_OPT_CONFIG), NULL);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_XML_INCHAR);
    assert_string_equal(llly_errmsg(st->ctx), "Invalid UTF-8 value 0x06");

    /* exclude noncharacters %x?FFFE-?FFFF - trying 0x10ffff */
    st->str1[17] = 0xf4;
    st->str1[18] = 0x8f;
    st->str1[19] = 0xbf;
    st->str1[20] = 0xbf;
    assert_ptr_equal(lllyd_parse_mem(st->ctx, st->str1, LLLYD_XML, LLLYD_OPT_CONFIG), NULL);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_XML_INCHAR);
    assert_string_equal(llly_errmsg(st->ctx), "Invalid UTF-8 value 0x0010ffff");

    /* 0x6 */
    st->str1[17] = '&';
    st->str1[18] = '#';
    st->str1[19] = 'x';
    st->str1[20] = '6';
    st->str1[21] = ';';
    assert_ptr_equal(lllyd_parse_mem(st->ctx, st->str1, LLLYD_XML, LLLYD_OPT_CONFIG), NULL);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_XML_INVAL);
    assert_string_equal(llly_errmsg(st->ctx), "Invalid character reference value.");

    /* 0xdfff */
    st->str1[17] = '&';
    st->str1[18] = '#';
    st->str1[19] = 'x';
    st->str1[20] = 'd';
    st->str1[21] = 'f';
    st->str1[22] = 'f';
    st->str1[23] = 'f';
    st->str1[24] = ';';
    assert_ptr_equal(lllyd_parse_mem(st->ctx, st->str1, LLLYD_XML, LLLYD_OPT_CONFIG), NULL);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_XML_INVAL);
    assert_string_equal(llly_errmsg(st->ctx), "Invalid character reference value.");

    /* 0xfdef */
    st->str1[17] = '&';
    st->str1[18] = '#';
    st->str1[19] = 'x';
    st->str1[20] = 'f';
    st->str1[21] = 'd';
    st->str1[22] = 'e';
    st->str1[23] = 'f';
    st->str1[24] = ';';
    assert_ptr_equal(lllyd_parse_mem(st->ctx, st->str1, LLLYD_XML, LLLYD_OPT_CONFIG), NULL);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_XML_INVAL);
    assert_string_equal(llly_errmsg(st->ctx), "Invalid character reference value.");

    /* 0xffff */
    st->str1[17] = '&';
    st->str1[18] = '#';
    st->str1[19] = 'x';
    st->str1[20] = 'f';
    st->str1[21] = 'f';
    st->str1[22] = 'f';
    st->str1[23] = 'f';
    st->str1[24] = ';';
    assert_ptr_equal(lllyd_parse_mem(st->ctx, st->str1, LLLYD_XML, LLLYD_OPT_CONFIG), NULL);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_XML_INVAL);
    assert_string_equal(llly_errmsg(st->ctx), "Invalid character reference value.");

    /* the same using character reference */
    /* 0x10ffff */
    st->str1[17] = '&';
    st->str1[18] = '#';
    st->str1[19] = 'x';
    st->str1[20] = '1';
    st->str1[21] = '0';
    st->str1[22] = 'f';
    st->str1[23] = 'f';
    st->str1[24] = 'f';
    st->str1[25] = 'f';
    st->str1[26] = ';';
    assert_ptr_equal(lllyd_parse_mem(st->ctx, st->str1, LLLYD_XML, LLLYD_OPT_CONFIG), NULL);
    assert_int_equal(llly_errno, LLLY_EVALID);
    assert_int_equal(llly_vecode(st->ctx), LLLYVE_XML_INVAL);
    assert_string_equal(llly_errmsg(st->ctx), "Invalid character reference value.");

}

int main(void)
{
    const struct CMUnitTest tests[] = {
                    cmocka_unit_test_teardown(test_parse_print_yin, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yang, teardown_f),
                    cmocka_unit_test_setup_teardown(test_parse_print_xml, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_parse_print_json, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_parse_print_keyless, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_parse_print_lyb, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_parse_print_oookeys_xml, setup_f, teardown_f),
                    cmocka_unit_test_setup_teardown(test_parse_print_oookeys_json, setup_f, teardown_f),
                    cmocka_unit_test_teardown(test_parse_noncharacters_xml, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_prefix, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_contact, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_organization, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_description, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_reference, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_yang_version, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_namespace, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_when, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_container, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_leaflist, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_leaf, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_list, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_choice, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_uses, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_anydata, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_rpc, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_action, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_notification, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_augment, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_grouping, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_revision, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_extension, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_import, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_include, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_identity, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_feature, teardown_f),
                    cmocka_unit_test_teardown(test_parse_print_yin_error_deviation, teardown_f),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
