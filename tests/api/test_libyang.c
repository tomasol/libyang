/*
 * @file test_libyang.c
 * @author: Mislav Novakovic <mislav.novakovic@sartura.hr>
 * @brief unit tests for functions from libyang.h header
 *
 * Copyright (C) 2016 Deutsche Telekom AG.
 *
 * Author: Mislav Novakovic <mislav.novakovic@sartura.hr>
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>

#include "tests/config.h"
#include "libyang.h"

/* include private header to be able to check internal values */
#include "../../src/context.h"

struct llly_ctx *ctx = NULL;
struct lllyd_node *root = NULL;
const struct lllys_module *module = NULL;

static int
setup_f(void **state)
{
    (void) state; /* unused */
    char *config_file = TESTS_DIR"/api/files/a.xml";
    char *yin_file = TESTS_DIR"/api/files/a.yin";
    char *yang_file = TESTS_DIR"/api/files/b.yang";
    char *yang_dev_file = TESTS_DIR"/api/files/b-dev.yang";
    char *yang_folder = TESTS_DIR"/api/files";

    ctx = llly_ctx_new(yang_folder, 0);
    if (!ctx) {
        return -1;
    }

    if (!lllys_parse_path(ctx, yin_file, LLLYS_IN_YIN)) {
        return -1;
    }

    if (!(module = lllys_parse_path(ctx, yang_file, LLLYS_IN_YANG))) {
        return -1;
    }

    if (!lllys_parse_path(ctx, yang_dev_file, LLLYS_IN_YANG)) {
        return -1;
    }

    root = lllyd_parse_path(ctx, config_file, LLLYD_XML, LLLYD_OPT_CONFIG | LLLYD_OPT_STRICT);
    if (!root) {
        return -1;
    }

    return 0;
}

static int
teardown_f(void **state)
{
    (void) state; /* unused */
    if (root) {
        lllyd_free_withsiblings(root);
    }

    if (ctx) {
        llly_ctx_destroy(ctx, NULL);
    }

    root = NULL;
    ctx = NULL;

    return 0;
}

static void
test_ly_ctx_new(void **state)
{
    char *yang_folder1 = TESTS_DIR"/data/files";
    char *yang_folder2 = TESTS_DIR"/data:"TESTS_DIR"/data/files";
    const char * const *list = NULL;
    (void) state; /* unused */

    ctx = llly_ctx_new(yang_folder1, 0);
    assert_ptr_not_equal(NULL, ctx);
    list = llly_ctx_get_searchdirs(ctx);
    assert_ptr_not_equal(NULL, list);
    assert_ptr_not_equal(NULL, list[0]);
    assert_ptr_equal(NULL, list[1]);
    llly_ctx_destroy(ctx, NULL);

    ctx = llly_ctx_new(yang_folder2, 0);
    assert_ptr_not_equal(NULL, ctx);
    list = llly_ctx_get_searchdirs(ctx);
    assert_ptr_not_equal(NULL, list);
    assert_ptr_not_equal(NULL, list[0]);
    assert_ptr_not_equal(NULL, list[1]);
    assert_ptr_equal(NULL, list[2]);
    llly_ctx_destroy(ctx, NULL);
}

static void
test_ly_ctx_new_invalid(void **state)
{
    char *yang_folder = "INVALID_PATH";
    (void) state; /* unused */
    ctx = llly_ctx_new(yang_folder, 0);
    if (ctx) {
        fail();
    }
}

static void
test_ly_ctx_get_searchdirs(void **state)
{
    const char * const *result;
    char yang_folder[PATH_MAX];
    (void) state; /* unused */

    assert_ptr_not_equal(realpath(TESTS_DIR"/data/files", yang_folder), NULL);

    ctx = llly_ctx_new(yang_folder, 0);
    if (!ctx) {
        fail();
    }

    result = llly_ctx_get_searchdirs(ctx);
    if (!result) {
        fail();
    }
    assert_string_equal(yang_folder, result[0]);
    assert_ptr_equal(NULL, result[1]);

    llly_ctx_destroy(ctx, NULL);
}

static void
test_ly_ctx_set_searchdir(void **state)
{
    const char * const *result;
    char yang_folder[PATH_MAX];
    char new_yang_folder[PATH_MAX];
    (void) state; /* unused */

    assert_ptr_not_equal(realpath(TESTS_DIR"/data/files", yang_folder), NULL);
    assert_ptr_not_equal(realpath(TESTS_DIR"/schema/yin", new_yang_folder), NULL);

    ctx = llly_ctx_new(yang_folder, 0);
    if (!ctx) {
        fail();
    }

    llly_ctx_set_searchdir(ctx, new_yang_folder);
    result = llly_ctx_get_searchdirs(ctx);
    if (!result) {
        fail();
    }

    assert_string_equal(yang_folder, result[0]);
    assert_string_equal(new_yang_folder, result[1]);
    assert_ptr_equal(NULL, result[2]);

    llly_ctx_unset_searchdirs(ctx, 0);
    assert_string_equal(new_yang_folder, result[0]);
    assert_ptr_equal(NULL, result[1]);

    llly_ctx_destroy(ctx, NULL);
}

static void
test_ly_ctx_set_searchdir_invalid(void **state)
{
    const char * const *result;
    char yang_folder[PATH_MAX];
    char *new_yang_folder = "INVALID_PATH";
    (void) state; /* unused */

    assert_ptr_not_equal(realpath(TESTS_DIR"/data/files", yang_folder), NULL);

    ctx = llly_ctx_new(yang_folder, 0);
    if (!ctx) {
        fail();
    }

    /* adding duplicity - the path is not duplicated */
    llly_ctx_set_searchdir(NULL, yang_folder);
    result = llly_ctx_get_searchdirs(ctx);
    if (!result) {
        fail();
    }
    assert_string_equal(yang_folder, result[0]);
    assert_ptr_equal(NULL, result[1]);

    /* adding invalid path, previous is kept */
    llly_ctx_set_searchdir(ctx, new_yang_folder);
    result = llly_ctx_get_searchdirs(ctx);
    if (!result) {
        fail();
    }
    assert_string_equal(yang_folder, result[0]);
    assert_ptr_equal(NULL, result[1]);

    llly_ctx_unset_searchdirs(ctx, -1);
    result = llly_ctx_get_searchdirs(ctx);
    if (result) {
        fail();
    }

    llly_ctx_destroy(ctx, NULL);
}

static void
test_ly_ctx_info(void **state)
{
    struct lllyd_node *node;
    (void) state; /* unused */

    node = llly_ctx_info(NULL);
    if (node) {
        fail();
    }

    node = llly_ctx_info(ctx);
    if (!node) {
        fail();
    }

    assert_int_equal(LLLYD_VAL_OK, node->validity);

    lllyd_free_withsiblings(node);
}

static void
test_ly_ctx_new_ylmem(void **state)
{
    struct lllyd_node *node;
    char *mem;
    struct llly_ctx *new_ctx;
    (void) state; /* unused */

    node = llly_ctx_info(ctx);
    if (!node) {
        fail();
    }

    if (lllyd_print_mem(&mem, node, LLLYD_XML, LLLYP_WITHSIBLINGS)) {
        fail();
    }

    new_ctx = llly_ctx_new_ylmem(TESTS_DIR"/api/files", mem, LLLYD_XML, 0);
    if (!new_ctx) {
        fail();
    }

    lllyd_free_withsiblings(node);
    free(mem);
    llly_ctx_destroy(new_ctx, NULL);
}

static void
test_ly_ctx_module_clb(void **state)
{
    (void) state;
    void *clb, *data;

    assert_ptr_equal(clb = llly_ctx_get_module_imp_clb(ctx, &data), NULL);
    assert_ptr_equal(data, NULL);

    clb = (intptr_t *)64;
    data = (intptr_t *)128;
    llly_ctx_set_module_imp_clb(ctx, clb, data);

    assert_ptr_equal(llly_ctx_get_module_imp_clb(ctx, &data), clb);
    assert_ptr_equal(data, (intptr_t *)128);
    llly_ctx_set_module_imp_clb(ctx, NULL, NULL);

    assert_ptr_equal(clb = llly_ctx_get_module_data_clb(ctx, &data), NULL);
    assert_ptr_equal(data, NULL);

    clb = (intptr_t *)64;
    data = (intptr_t *)128;
    llly_ctx_set_module_data_clb(ctx, clb, data);

    assert_ptr_equal(llly_ctx_get_module_data_clb(ctx, &data), clb);
    assert_ptr_equal(data, (intptr_t *)128);
    llly_ctx_set_module_data_clb(ctx, NULL, NULL);
}

static void
test_ly_ctx_get_module(void **state)
{
    (void) state; /* unused */
    const struct lllys_module *module;
    const char *name1 = "a";
    const char *name2 = "b";
    const char *revision = "2016-03-01";

    module = llly_ctx_get_module(NULL, name1, NULL, 0);
    if (module) {
        fail();
    }

    module = llly_ctx_get_module(ctx, NULL, NULL, 0);
    if (module) {
        fail();
    }

    module = llly_ctx_get_module(ctx, "invalid", NULL, 0);
    if (module) {
        fail();
    }

    module = llly_ctx_get_module(ctx, name1, NULL, 0);
    if (!module) {
        fail();
    }

    assert_string_equal("a", module->name);

    module = llly_ctx_get_module(ctx, name1, "invalid", 0);
    if (module) {
        fail();
    }

    module = llly_ctx_get_module(ctx, name1, revision, 0);
    if (!module) {
        fail();
    }

    assert_string_equal(revision, module->rev->date);

    module = llly_ctx_get_module(ctx, name2, NULL, 0);
    if (!module) {
        fail();
    }

    assert_string_equal("b", module->name);

    module = llly_ctx_get_module(ctx, name2, "invalid", 0);
    if (module) {
        fail();
    }

    module = llly_ctx_get_module(ctx, name2, revision, 0);
    if (!module) {
        fail();
    }

    assert_string_equal(revision, module->rev->date);
}

static void
test_ly_ctx_get_module_older(void **state)
{
    (void) state; /* unused */
    const struct lllys_module *module = NULL;
    const struct lllys_module *module_older = NULL;
    const char *name = "a";
    const char *revision = "2016-03-01";
    const char *revision_older = "2015-01-01";

    module_older = llly_ctx_get_module_older(NULL, module);
    if (module_older) {
        fail();
    }

    module_older = llly_ctx_get_module_older(ctx, NULL);
    if (module_older) {
        fail();
    }

    module = llly_ctx_load_module(ctx, "c", NULL);
    if (!module) {
        fail();
    }

    module = llly_ctx_load_module(ctx, name, revision);
    if (!module) {
        fail();
    }

    module_older = llly_ctx_get_module_older(ctx, module);
    if (!module_older) {
        fail();
    }

    assert_string_equal(revision_older, module_older->rev->date);
}

static void
test_ly_ctx_load_module(void **state)
{
    (void) state; /* unused */
    const struct lllys_module *module;
    const char *name = "a";
    const char *revision = "2015-01-01";

    module = llly_ctx_load_module(NULL, name, revision);
    if (module) {
        fail();
    }

    module = llly_ctx_load_module(ctx, NULL, revision);
    if (module) {
        fail();
    }

    module = llly_ctx_load_module(ctx, "INVALID_NAME", revision);
    if (module) {
        fail();
    }

    module = llly_ctx_load_module(ctx, "c", NULL);
    if (!module) {
        fail();
    }

    assert_string_equal("c", module->name);

    module = llly_ctx_get_module(ctx, "a", revision, 0);
    if (!module) {
        fail();
    }

    assert_string_equal("a", module->name);

    module = llly_ctx_get_module(ctx, "b", revision, 0);
    if (!module) {
        fail();
    }

    assert_string_equal("b", module->name);
}

static void
test_ly_ctx_clean(void **state)
{
    (void) state; /* unused */
    const struct lllys_module *mod;
    struct llly_ctx *ctx;
    uint32_t dict_used;
    uint16_t setid;
    int modules_count;

    ctx = llly_ctx_new(TESTS_DIR"/api/files/", 0);
    /* remember starting values */
    setid = ctx->models.module_set_id;
    modules_count = ctx->models.used;
    dict_used = ctx->dict.hash_tab->used;

    /* add a module */
    mod = llly_ctx_load_module(ctx, "x", NULL);
    assert_ptr_not_equal(mod, NULL);
    assert_int_equal(modules_count + 1, ctx->models.used);
    assert_int_not_equal(dict_used, ctx->dict.hash_tab->used);

    /* clean the context */
    llly_ctx_clean(ctx, NULL);
    assert_int_equal(setid + 2, ctx->models.module_set_id);
    assert_int_equal(modules_count, ctx->models.used);
    assert_int_equal(dict_used, ctx->dict.hash_tab->used);

    /* add a module again ... */
    mod = llly_ctx_load_module(ctx, "x", NULL);
    assert_ptr_not_equal(mod, NULL);
    assert_int_equal(modules_count + 1, ctx->models.used);
    assert_int_not_equal(dict_used, ctx->dict.hash_tab->used);

    /* .. and add some string into dictionary */
    assert_ptr_not_equal(lllydict_insert(ctx, "qwertyuiop", 0), NULL);
    ++dict_used;

    /* clean the context */
    llly_ctx_clean(ctx, NULL);
    assert_int_equal(setid + 4, ctx->models.module_set_id);
    assert_int_equal(modules_count, ctx->models.used);
    assert_int_equal(dict_used, ctx->dict.hash_tab->used);

    /* cleanup */
    lllydict_remove(ctx, "qwertyuiop");
    llly_ctx_destroy(ctx, NULL);
}

static void
test_ly_ctx_clean2(void **state)
{
    (void) state; /* unused */
    const char *yang_dep = "module x {"
                    "  namespace uri:x;"
                    "  prefix x;"
                    "  import ietf-yang-library { prefix yl; }"
                    "  leaf x { config false; type leafref { path /yl:modules-state/yl:module/yl:name; } } }";
    struct llly_ctx *ctx;
    const struct lllys_module *mod;
    struct lllys_node_leaf *leaf;

    ctx = llly_ctx_new(NULL, 0);
    assert_ptr_not_equal(ctx, NULL);

    /* load module depending by leafref on internal ietf-yang-library */
    assert_ptr_not_equal(lllys_parse_mem(ctx, yang_dep, LLLYS_IN_YANG), NULL);

    /* get the target leaf in ietf-yang-library */
    mod = ctx->models.list[ctx->internal_module_count - 1];
    /* magic: leaf = /yl:modules-state/yl:module/yl:name */
    leaf = (struct lllys_node_leaf *)mod->data->prev->prev->prev->child->next->child->prev->child->child;
    assert_true(leaf->backlinks && leaf->backlinks->number == 1);

    /* clean the context ... */
    llly_ctx_clean(ctx, NULL);

    /* ... and check that the leafref backlinks are removed */
    assert_true(!leaf->backlinks || !leaf->backlinks->number);

    /* cleanup */
    llly_ctx_destroy(ctx, NULL);
}

static void
test_ly_ctx_remove_module(void **state)
{
    (void) state; /* unused */
    const struct lllys_module *mod;
    uint32_t dict_used;
    uint16_t setid;
    int modules_count;

    ctx = llly_ctx_new(TESTS_DIR"/api/files/", 0);
    /* remember starting values */
    setid = ctx->models.module_set_id;
    modules_count = ctx->models.used;
    dict_used = ctx->dict.hash_tab->used;

    mod = llly_ctx_load_module(ctx, "x", NULL);
    llly_ctx_remove_module(mod, NULL);

    /* add a module */
    mod = llly_ctx_load_module(ctx, "y", NULL);
    assert_ptr_not_equal(mod, NULL);
    assert_true(setid < ctx->models.module_set_id);
    setid = ctx->models.module_set_id;
    assert_int_equal(modules_count + 2, ctx->models.used);
    assert_int_not_equal(dict_used, ctx->dict.hash_tab->used);

    /* remove the imported module (x), that should cause removing also the loaded module (y) */
    mod = llly_ctx_get_module(ctx, "x", NULL, 0);
    assert_ptr_not_equal(mod, NULL);
    llly_ctx_remove_module(mod, NULL);
    assert_true(setid < ctx->models.module_set_id);
    setid = ctx->models.module_set_id;
    assert_int_equal(modules_count, ctx->models.used);
    assert_int_equal(dict_used, ctx->dict.hash_tab->used);

    /* add a module again ... */
    mod = llly_ctx_load_module(ctx, "y", NULL);
    assert_ptr_not_equal(mod, NULL);
    assert_true(setid < ctx->models.module_set_id);
    setid = ctx->models.module_set_id;
    assert_int_equal(modules_count + 2, ctx->models.used);
    assert_int_not_equal(dict_used, ctx->dict.hash_tab->used);
    /* ... now remove the loaded module, the imported module is supposed to be removed because it is not
     * used in any other module */
    llly_ctx_remove_module(mod, NULL);
    assert_true(setid < ctx->models.module_set_id);
    setid = ctx->models.module_set_id;
    assert_int_equal(modules_count, ctx->models.used);
    assert_int_equal(dict_used, ctx->dict.hash_tab->used);

    /* add a module again ... */
    mod = llly_ctx_load_module(ctx, "y", NULL);
    assert_ptr_not_equal(mod, NULL);
    assert_true(setid < ctx->models.module_set_id);
    setid = ctx->models.module_set_id;
    assert_int_equal(modules_count + 2, ctx->models.used);
    assert_int_not_equal(dict_used, ctx->dict.hash_tab->used);
    /* and mark even the imported module 'x' as implemented ... */
    assert_int_equal(lllys_set_implemented(mod->imp[0].module), EXIT_SUCCESS);
    /* ... now remove the loaded module, the imported module is supposed to be kept because it is implemented */
    llly_ctx_remove_module(mod, NULL);
    assert_true(setid < ctx->models.module_set_id);
    setid = ctx->models.module_set_id;
    assert_int_equal(modules_count + 1, ctx->models.used);
    assert_int_not_equal(dict_used, ctx->dict.hash_tab->used);
    mod = llly_ctx_get_module(ctx, "y", NULL, 0);
    assert_ptr_equal(mod, NULL);
    mod = llly_ctx_get_module(ctx, "x", NULL, 0);
    assert_ptr_not_equal(mod, NULL);
    llly_ctx_clean(ctx, NULL);

    /* add a module again ... */
    mod = llly_ctx_load_module(ctx, "y", NULL);
    assert_true(setid < ctx->models.module_set_id);
    setid = ctx->models.module_set_id;
    assert_int_equal(modules_count + 2, ctx->models.used);
    assert_int_not_equal(dict_used, ctx->dict.hash_tab->used);
    /* and add another one also importing module 'x' ... */
    assert_ptr_not_equal(llly_ctx_load_module(ctx, "z", NULL), NULL);
    assert_true(setid < ctx->models.module_set_id);
    setid = ctx->models.module_set_id;
    assert_int_equal(modules_count + 3, ctx->models.used);
    /* ... now remove the first loaded module, the imported module is supposed to be kept because it is used
     * by the second loaded module */
    llly_ctx_remove_module(mod, NULL);
    assert_true(setid < ctx->models.module_set_id);
    setid = ctx->models.module_set_id;
    assert_int_equal(modules_count + 2, ctx->models.used);
    assert_int_not_equal(dict_used, ctx->dict.hash_tab->used);
    mod = llly_ctx_get_module(ctx, "y", NULL, 0);
    assert_ptr_equal(mod, NULL);
    mod = llly_ctx_get_module(ctx, "x", NULL, 0);
    assert_ptr_not_equal(mod, NULL);
    mod = llly_ctx_get_module(ctx, "z", NULL, 0);
    assert_ptr_not_equal(mod, NULL);
}

static void
test_ly_ctx_remove_module2(void **state)
{
    (void) state; /* unused */
    const char *yang_main = "module x {"
                    "  namespace uri:x;"
                    "  prefix x;"
                    "  feature x;"
                    "  identity basex;"
                    "  leaf x { type string; } }";
    const char *yang_dep = "module y {"
                    "  namespace uri:y;"
                    "  prefix y;"
                    "  import x { prefix x; }"
                    "  feature y { if-feature x:x; }"
                    "  identity y { base x:basex; }"
                    "  leaf y { type leafref { path /x:x; } } }";
    const struct lllys_module *mod;
    struct lllys_node_leaf *leaf;

    ctx = llly_ctx_new(NULL, 0);
    assert_ptr_not_equal(ctx, NULL);

    /* load both modules, y depends on x and x will contain several backlinks to y */
    assert_ptr_not_equal((mod = lllys_parse_mem(ctx, yang_main, LLLYS_IN_YANG)), NULL);
    assert_ptr_not_equal(lllys_parse_mem(ctx, yang_dep, LLLYS_IN_YANG), NULL);

    /* check that there are the expected backlinks */
    leaf = (struct lllys_node_leaf *)mod->data;
    assert_true(mod->features[0].depfeatures && mod->features[0].depfeatures->number);
    assert_true(mod->ident[0].der && mod->ident[0].der->number);
    assert_true(leaf->backlinks && leaf->backlinks->number);

    /* remove y ... */
    mod = llly_ctx_get_module(ctx, "y", NULL, 0);
    assert_ptr_not_equal(mod, NULL);
    assert_int_equal(llly_ctx_remove_module(mod, NULL), 0);

    /* ... make sure that x is still present ... */
    mod = llly_ctx_get_module(ctx, "x", NULL, 0);
    assert_ptr_not_equal(mod, NULL);
    leaf = (struct lllys_node_leaf *)mod->data;

    /* ... and check that the backlinks in it were removed */
    assert_true(!mod->features[0].depfeatures || !mod->features[0].depfeatures->number);
    assert_true(!mod->ident[0].der || !mod->ident[0].der->number);
    assert_true(!leaf->backlinks || !leaf->backlinks->number);
}

static void
test_lys_set_enabled(void **state)
{
    (void) state; /* unused */
    const struct lllys_module *mod;

    ctx = llly_ctx_new(NULL, 0);
    assert_ptr_not_equal(ctx, NULL);

    /* test failures - invalid input */
    assert_int_not_equal(lllys_set_enabled(NULL), 0);

    /* test success - enabled module */
    mod = llly_ctx_get_module(ctx, "ietf-yang-library", NULL, 0);
    assert_ptr_not_equal(mod, NULL);
    assert_int_equal(lllys_set_enabled(mod), 0);
}

/* include also some test for lllys_set_enabled() */
static void
test_lys_set_disabled(void **state)
{
    (void) state; /* unused */
    uint32_t idx;
    const struct lllys_module *mod, *modx, *mody;
    const char *yang_x = "module x {"
                    "  namespace uri:x;"
                    "  prefix x;"
                    "  container x { presence yes; }}";
    const char *yang_y = "module y {"
                    "  namespace uri:y;"
                    "  prefix y;"
                    "  import x { prefix x;}"
                    "  augment /x:x {"
                    "    leaf y { type string;}}}";

    ctx = llly_ctx_new(NULL, 0);
    assert_ptr_not_equal(ctx, NULL);

    /* test failures - invalid input */
    assert_int_not_equal(lllys_set_disabled(NULL), 0);

    /* test failures - internal module */
    mod = llly_ctx_get_module(ctx, "ietf-yang-library", NULL, 0);
    assert_ptr_not_equal(mod, NULL);
    assert_int_not_equal(lllys_set_disabled(mod), 0);

    /* test success - disabling y extending x */
    modx = lllys_parse_mem(ctx, yang_x, LLLYS_IN_YANG);
    assert_ptr_not_equal(modx, NULL);
    mody = lllys_parse_mem(ctx, yang_y, LLLYS_IN_YANG);
    assert_ptr_not_equal(mody, NULL);

    /* all the modules are enabled ... */
    assert_int_equal(mody->disabled, 0);
    assert_int_equal(modx->disabled, 0);
    /* ... and the y's augment is applied */
    assert_ptr_not_equal(modx->data->child, NULL);

    /* by disabling y ... */
    assert_int_equal(lllys_set_disabled(mody), 0);
    /* ... y is disabled while x stays enabled (it is implemented) ...*/
    assert_int_equal(mody->disabled, 1);
    assert_int_equal(modx->disabled, 0);
    /* ... and y's augment disappeared from x */
    assert_ptr_equal(modx->data->child, NULL);

    /* by enabling it, everything goes back */
    assert_int_equal(lllys_set_enabled(mody), 0);
    assert_int_equal(mody->disabled, 0);
    assert_int_equal(modx->disabled, 0);
    assert_ptr_not_equal(modx->data->child, NULL);

    /* by disabling x ... */
    assert_int_equal(lllys_set_disabled(modx), 0);
    /* ... both x and y are disabled (y depends on x) ...*/
    assert_int_equal(mody->disabled, 1);
    assert_int_equal(modx->disabled, 1);
    /* ... and y's augment disappeared from x */
    assert_ptr_equal(modx->data->child, NULL);

    /* iterate through all disabled modules */
    idx = 0;
    mod = llly_ctx_get_disabled_module_iter(ctx, &idx);
    assert_ptr_not_equal(mod, NULL);
    assert_int_equal(mod->disabled, 1);
    assert_string_equal(mod->name, "x");

    /* by enabling it, everything goes back */
    assert_int_equal(lllys_set_enabled(modx), 0);
    assert_int_equal(mody->disabled, 0);
    assert_int_equal(modx->disabled, 0);
    assert_ptr_not_equal(modx->data->child, NULL);
}


static void
test_ly_ctx_get_module_by_ns(void **state)
{
    (void) state; /* unused */
    const struct lllys_module *module;
    const char *ns = "urn:a";
    const char *revision = NULL;

    module = llly_ctx_get_module_by_ns(NULL, ns, revision, 0);
    if (module) {
        fail();
    }

    module = llly_ctx_get_module_by_ns(ctx, NULL, revision, 0);
    if (module) {
        fail();
    }

    module = llly_ctx_get_module_by_ns(ctx, ns, revision, 0);
    if (!module) {
        fail();
    }

    assert_string_equal("a", module->name);

    module = llly_ctx_get_module_by_ns(ctx, "urn:b", revision, 0);
    if (!module) {
        fail();
    }

    assert_string_equal("b", module->name);
}

static void
test_ly_ctx_get_submodule(void **state)
{
    (void) state; /* unused */
    const struct lllys_submodule *submodule;
    const char *mod_name = "a";
    const char *sub_name = "asub";
    const char *revision = NULL;

    submodule = llly_ctx_get_submodule(NULL, mod_name, revision, sub_name, NULL);
    if (submodule) {
        fail();
    }

    submodule = llly_ctx_get_submodule(ctx, NULL, revision, sub_name, "2010-02-08");
    if (submodule) {
        fail();
    }

    submodule = llly_ctx_get_submodule(ctx, mod_name, revision, NULL, NULL);
    if (submodule) {
        fail();
    }

    submodule = llly_ctx_get_submodule(ctx, mod_name, revision, sub_name, NULL);
    if (!submodule) {
        fail();
    }

    assert_string_equal("asub", submodule->name);

    submodule = llly_ctx_get_submodule(ctx, "b", revision, "bsub", NULL);
    if (!submodule) {
        fail();
    }

    assert_string_equal("bsub", submodule->name);
}

static void
test_ly_ctx_get_submodule2(void **state)
{
    (void) state; /* unused */
    const struct lllys_submodule *submodule;
    const char *sub_name1 = "asub";
    const char *sub_name2 = "bsub";

    submodule = llly_ctx_get_submodule2(NULL, sub_name1);
    if (submodule) {
        fail();
    }

    submodule = llly_ctx_get_submodule2(root->schema->module, NULL);
    if (submodule) {
        fail();
    }

    submodule = llly_ctx_get_submodule2(root->schema->module, sub_name1);
    if (!submodule) {
        fail();
    }

    assert_string_equal("asub", submodule->name);

    submodule = llly_ctx_get_submodule2(module, sub_name2);
    if (!submodule) {
        fail();
    }

    assert_string_equal("bsub", submodule->name);
}

static void
test_lys_find_path(void **state)
{
    (void) state; /* unused */
    struct llly_set *set;
    const char *nodeid1 = "/x/bubba";
    const char *nodeid2 = "/b:x/b:bubba";
    const char *nodeid3 = "/x/choic/con/con/lef";

    set = lllys_find_path(NULL, root->schema, nodeid1);
    if (!set || (set->number != 1)) {
        fail();
    }
    llly_set_free(set);

    set = lllys_find_path(NULL, root->schema, NULL);
    if (set) {
        fail();
    }

    set = lllys_find_path(root->schema->module, root->schema, nodeid1);
    if (!set || (set->number != 1)) {
        fail();
    }

    assert_string_equal("bubba", set->set.s[0]->name);
    llly_set_free(set);

    set = lllys_find_path(root->schema->module, root->schema, nodeid2);
    if (!set || (set->number != 1)) {
        fail();
    }

    assert_string_equal("bubba", set->set.s[0]->name);
    llly_set_free(set);

    set = lllys_find_path(root->schema->module, root->schema, nodeid3);
    if (!set || (set->number != 1)) {
        fail();
    }

    assert_string_equal("lef", set->set.s[0]->name);
    llly_set_free(set);
}

static void
test_ly_set_new(void **state)
{
    (void) state; /* unused */
    struct llly_set *set;

    set = llly_set_new();
    if (!set) {
        fail();
    }

    free(set);
}

static void
test_ly_set_add(void **state)
{
    (void) state; /* unused */
    struct llly_set *set;
    int rc;

    set = llly_set_new();
    if (!set) {
        fail();
    }

    rc = llly_set_add(NULL, root->child->schema, 0);
    if(rc != -1) {
        fail();
    }

    rc = llly_set_add(set, root->child->schema, 0);
    if(rc == -1) {
        fail();
    }

    llly_set_free(set);
}

static void
test_ly_set_rm(void **state)
{
    (void) state; /* unused */
    struct llly_set *set;
    int rc;

    set = llly_set_new();
    if (!set) {
        fail();
    }

    rc = llly_set_rm(NULL, root->child->schema);
    if(!rc) {
        fail();
    }

    rc = llly_set_rm(set, NULL);
    if(!rc) {
        fail();
    }

    rc = llly_set_add(set, root->child->schema, 0);
    if(rc) {
        fail();
    }

    rc = llly_set_rm(set, root->child->schema);
    if(rc) {
        fail();
    }

    llly_set_free(set);
}

static void
test_ly_set_rm_index(void **state)
{
    (void) state; /* unused */
    struct llly_set *set;
    int rc;

    set = llly_set_new();
    if (!set) {
        fail();
    }

    rc = llly_set_rm_index(NULL, 0);
    if(!rc) {
        fail();
    }

    rc = llly_set_add(set, root->child->schema, 0);
    if(rc) {
        fail();
    }

    rc = llly_set_rm_index(set, 0);
    if(rc) {
        fail();
    }

    llly_set_free(set);
}

static void
test_ly_set_free(void **state)
{
    (void) state; /* unused */
    struct llly_set *set;

    set = llly_set_new();
    if (!set) {
        fail();
    }

    llly_set_free(set);

    if (!set) {
        fail();
    }
}

static void
test_ly_verb(void **state)
{
    (void) state; /* unused */

    llly_verb(LLLY_LLERR);
}

void clb_custom(LLLY_LOG_LEVEL level, const char *msg, const char *path )
{
    (void) level; /* unused */
    (void) msg; /* unused */
    (void) path; /* unused */
}

static void
test_ly_get_log_clb(void **state)
{
    (void) state; /* unused */
    void *clb = NULL;

    clb = llly_get_log_clb();
    assert_ptr_equal(clb, NULL);
}

static void
test_ly_set_log_clb(void **state)
{
    (void) state; /* unused */
    void *clb = NULL;
    void *clb_new = NULL;

    clb = llly_get_log_clb();

    llly_set_log_clb(clb_custom,0);

    clb_new = llly_get_log_clb();

    assert_ptr_not_equal(clb, clb_new);
}

static void
test_ly_log_options(void **state)
{
    (void)state;
    const struct llly_err_item *i;
    const struct lllys_module *mod;
    char *path;

    /* reset logging with path */
    llly_set_log_clb(NULL, 1);

    assert_int_equal(llly_log_options(LLLY_LOLOG | LLLY_LOSTORE_LAST), LLLY_LOLOG | LLLY_LOSTORE_LAST);

    i = llly_err_first(ctx);
    assert_null(i);

    mod = llly_ctx_load_module(ctx, "INVALID_NAME", NULL);
    assert_null(mod);
    assert_int_equal(llly_errno, LLLY_ESYS);

    i = llly_err_first(ctx);
    assert_non_null(i);
    i = i->prev;
    assert_int_equal(i->no, LLLY_ESYS);
    assert_string_equal(i->msg, "Data model \"INVALID_NAME\" not found.");
    assert_null(i->next);

    mod = llly_ctx_load_module(ctx, "INVALID_NAME2", NULL);
    assert_null(mod);
    assert_int_equal(llly_errno, LLLY_ESYS);

    i = llly_err_first(ctx);
    assert_non_null(i);
    i = i->prev;
    assert_int_equal(i->no, LLLY_ESYS);
    assert_string_equal(i->msg, "Data model \"INVALID_NAME2\" not found.");
    assert_null(i->next);

    llly_log_options(LLLY_LOSTORE);

    path = llly_path_data2schema(ctx, "/a:f/g/h");
    assert_null(path);
    assert_int_equal(llly_errno, LLLY_EVALID);

    i = llly_err_first(ctx);
    assert_non_null(i);
    i = i->prev;
    assert_int_equal(i->no, LLLY_EVALID);
    assert_int_equal(i->vecode, LLLYVE_PATH_INNODE);
    assert_string_equal(i->msg, "Schema node not found.");
    assert_string_equal(i->path, "f");
    assert_null(i->next);

    path = llly_path_data2schema(ctx, "/fgh:f/g/h");
    assert_null(path);
    assert_int_equal(llly_errno, LLLY_EVALID);

    i = llly_err_first(ctx);
    assert_non_null(i);
    i = i->prev;
    assert_int_equal(i->no, LLLY_EVALID);
    assert_int_equal(i->vecode, LLLYVE_PATH_INMOD);
    assert_string_equal(i->msg, "Module not found or not implemented.");
    assert_string_equal(i->path, "fgh");
    assert_null(i->next);

    assert_non_null(i->prev->next);
    assert_non_null(i->prev->prev->next);

    llly_log_options(LLLY_LOLOG | LLLY_LOSTORE_LAST);

    llly_err_clean(ctx, NULL);
    assert_int_equal(llly_errno, LLLY_SUCCESS);
    i = llly_err_first(ctx);
    assert_null(i);
}

static void
test_ly_path_data2schema(void **state)
{
    (void) state; /* unused */
    char *schema_path;

    schema_path = llly_path_data2schema(ctx, "/a:x/con/lef");
    assert_string_equal(schema_path, "/a:x/choic/con/con/lef");
    free(schema_path);

    schema_path = llly_path_data2schema(ctx, "/a:*");
    assert_string_equal(schema_path, "/a:*");
    free(schema_path);

    schema_path = llly_path_data2schema(ctx, "/a:*//*");
    assert_string_equal(schema_path, "/a:*//*");
    free(schema_path);

    schema_path = llly_path_data2schema(ctx, "/a:x//.");
    assert_string_equal(schema_path, "/a:x//.");
    free(schema_path);

    schema_path = llly_path_data2schema(ctx, "/a:x[bar-leaf='aa']//.");
    assert_string_equal(schema_path, "/a:x[bar-leaf='aa']//.");
    free(schema_path);

    schema_path = llly_path_data2schema(ctx, "/a:x/bar-gggg");
    assert_string_equal(schema_path, "/a:x/bar-gggg");
    free(schema_path);

    schema_path = llly_path_data2schema(ctx, "/a:x/bar-gggg | /a:x");
    assert_string_equal(schema_path, "/a:x/bar-gggg | /a:x");
    free(schema_path);

    schema_path = llly_path_data2schema(ctx, "/a:x/bar-gggg and ( /a:x/bar-gggg or /a:x)");
    assert_string_equal(schema_path, "/a:x/bar-gggg and ( /a:x/bar-gggg or /a:x)");
    free(schema_path);
}

static void
test_ly_get_loaded_plugins(void **state)
{
    (void) state;
    int i;
    const char * const *plugins;

    for (i = 0, plugins = llly_get_loaded_plugins(); plugins && plugins[i]; ++i) {
        fail();
    }

    llly_load_plugins();

    plugins = llly_get_loaded_plugins();
    assert_non_null(plugins);

    for (i = 0; plugins[i]; ++i) {
        if (!strcmp(plugins[i], "metadata")) {
            break;
        }
    }
    assert_non_null(plugins[i]);
    for (i = 0; plugins[i]; ++i) {
        if (!strcmp(plugins[i], "yangdata")) {
            break;
        }
    }
    assert_non_null(plugins[i]);
    for (i = 0; plugins[i]; ++i) {
        if (!strcmp(plugins[i], "nacm")) {
            break;
        }
    }
    assert_non_null(plugins[i]);
    for (i = 0; plugins[i]; ++i) {
        if (!strcmp(plugins[i], "user_yang_types")) {
            break;
        }
    }
    assert_non_null(plugins[i]);
    for (i = 0; plugins[i]; ++i) {
        if (!strcmp(plugins[i], "user_inet_types")) {
            break;
        }
    }
    assert_non_null(plugins[i]);

    llly_clean_plugins();

    for (i = 0, plugins = llly_get_loaded_plugins(); plugins && plugins[i]; ++i) {
        fail();
    }
}

static void
test_ly_ctx_internal_modules_count(void **state)
{
    (void) state;
    unsigned int internal_modules_count;
    struct llly_ctx *new_ctx;
    const char *yang_folder = TESTS_DIR"/api/files";

    new_ctx = llly_ctx_new(yang_folder, 0);

    internal_modules_count = llly_ctx_internal_modules_count(new_ctx);
    if (internal_modules_count == 0) {
        fail();
    }
    llly_ctx_clean(new_ctx, NULL);
    llly_ctx_destroy(new_ctx, NULL);

    new_ctx = llly_ctx_new("INVALID PATH", 0);

    internal_modules_count = llly_ctx_internal_modules_count(new_ctx);
    if (internal_modules_count != 0) {
        fail();
    }
    llly_ctx_clean(new_ctx, NULL);
    llly_ctx_destroy(new_ctx, NULL);
}

void
test_ly_ctx_set_allimplemented(void **state)
{
    (void) state; /* unused */
    const struct lllys_module *module = NULL;

    /* standard setup */
    module = llly_ctx_load_module(ctx, "y", NULL);
    /* implemented flag should be 0 */
    if (module->imp->module->implemented == 1) {
        fail();
    }

    llly_ctx_remove_module(module, NULL);

    /* setup with set_allimplement */
    llly_ctx_set_allimplemented(ctx);
    module = llly_ctx_load_module(ctx, "y", NULL);

    /* implemented flag should be 1 */
    if (module->imp->module->implemented != 1) {
        fail();
    }

    llly_ctx_remove_module(module, NULL);
}

void
test_ly_ctx_get_module_set_id(void **state)
{
    (void) state;
    uint16_t set_id = ctx->models.module_set_id;

    if (set_id != llly_ctx_get_module_set_id(ctx)) {
        fail();
    }
}

void
test_ly_ctx_get_module_iter(void **state)
{
    (void) state;
    const struct lllys_module *first_module = NULL;
    const struct lllys_module *second_module = NULL;
    const struct lllys_module *iteration = NULL;
    uint32_t iter_num = 0;
    uint8_t first_found = 0;
    uint8_t second_found = 0;
    struct llly_ctx *ctx;

    ctx = llly_ctx_new(TESTS_DIR"/api/files/", 0);
    first_module = llly_ctx_load_module(ctx, "x", NULL);
    second_module = llly_ctx_load_module(ctx, "y", NULL);

    /* enabled modules  */
    do {
        iteration = llly_ctx_get_module_iter(ctx, &iter_num);
        if (iteration == first_module) {
            first_found = 1;
        }
        if (iteration == second_module) {
            second_found = 1;
        }

    } while (iteration != NULL);

    if (!second_found) {
        fail();
    }

    if (!first_found) {
        fail();
    }

    /* disabled modules */
    iter_num = 0;
    lllys_set_disabled(first_module);
    lllys_set_disabled(second_module);

    first_found = 0;
    second_found = 0;

    do {
        iteration = llly_ctx_get_disabled_module_iter(ctx, &iter_num);
        if (iteration == first_module) {
            first_found = 1;
        }
        if (iteration == second_module) {
            second_found = 1;
        }

    } while (iteration != NULL);

    if (!second_found) {
        fail();
    }

    if (!first_found) {
        fail();
    }

    llly_ctx_clean(ctx, NULL);
    llly_ctx_destroy(ctx, NULL);
}

void
test_ly_ctx_set_trusted(void **state)
{
    (void) state;
    int flags = ctx->models.flags;
    /* raising flag for trusted  */
    llly_ctx_set_trusted(ctx);
    /* Checking for changes in context  */
    if (ctx->models.flags == flags) {
        fail();
    }

    /* lowering the flag for trusted  */
    llly_ctx_unset_trusted(ctx);
    /* Checking whether the the context has returned to previous state  */
    if (ctx->models.flags != flags) {
        fail();
    }
}

void
test_ly_ctx_get_node(void **state)
{
    (void) state;
    const struct lllys_node *node = NULL;

    module = llly_ctx_load_module(ctx, "y", NULL);

    /* Test with a valid path */
    node = llly_ctx_get_node(ctx, NULL, "/b:x/b:bubba", 0);
    if (!node) {
        fail();
    }

    /* Test with an invalid path */
    node = llly_ctx_get_node(ctx, NULL, "INVALID PATH", 0);
    if (node) {
        fail();
    }
}

void
test_ly_ctx_find_path(void **state)
{
    (void) state;
    struct llly_set *set = NULL;

    set = llly_ctx_find_path(ctx, "/b:*");

    /* Test with a valid path  */
    if (!set) {
        fail();
    }

    llly_set_free(set);

    set = llly_ctx_find_path(ctx, "INVALID PATH");

    /* Test with an invalid path  */
    if (set) {
        fail();
    }

    llly_set_free(set);
}

void
test_ly_ctx_destroy(void **state)
{
    (void) state; /* unused */
    struct llly_ctx *new_ctx = NULL;

    if (new_ctx) {
        fail();
    }

    new_ctx = llly_ctx_new(TESTS_DIR "/api/files", 0);
    /* Making sure that the context has internal modules  */
    if (!new_ctx->internal_module_count) {
        fail();
    }

    llly_ctx_clean(new_ctx, NULL);
    llly_ctx_destroy(new_ctx, NULL);

    /* Checking if the funcion has cleared the internal structure  */
}

void
test_ly_path_xml2json(void **state)
{
    (void) state;
    struct lllyxml_elem *xml = NULL;
    char *xml_path;
    char *mem;
    struct lllyd_node *node;

    node = llly_ctx_info(ctx);
    if (!node) {
        fail();
    }

    if (lllyd_print_mem(&mem, node, LLLYD_XML, LLLYP_WITHSIBLINGS)) {
        fail();
    }

    xml = lllyxml_parse_mem(ctx, mem, LLLYXML_PARSE_NOMIXEDCONTENT);
    if (!mem) {
        fail();
    }

    /* Check for the xml element */
    if (!xml) {
        fail();
    }

    xml_path = llly_path_xml2json(ctx, "/c", xml);

    /* Check for xml path */
    if (!xml_path) {
        fail();
    }

    free(xml_path);
    xml_path = llly_path_xml2json(ctx, "INVALID PATH", xml);

    /* Check for xml invalid path */
    if (xml_path) {
        fail();
    }

    /* Freeing the elements */
    lllyxml_free_withsiblings(ctx, xml);
    lllyd_free_withsiblings(node);
    free(mem);
}

void
test_ly_set_dup(void **state)
{
    (void) state;
    struct llly_set *first_set = NULL;
    struct llly_set *second_set = NULL;

    /* Creating the first set */
    first_set = llly_set_new();

    if (!first_set) {
        fail();
    }

    /* Duplicating the first set onto the second  */
    second_set = llly_set_dup(first_set);
    if (!second_set) {
        fail();
    }

    llly_set_free(first_set);
    llly_set_free(second_set);
}

void
test_ly_set_merge(void **state)
{
    (void) state;
    struct llly_set *first_set = NULL;
    struct llly_set *second_set = NULL;

    first_set = llly_set_new();
    second_set = llly_set_new();
    /* Adding a node to the second set to later see whether the merge was successful*/
    llly_set_add(second_set, root->child->schema, 0);

    /* Check if both sets are set */
    if (!first_set || !second_set) {
        fail();
    }

    /* Check if the merge is successful  */
    if (llly_set_merge(first_set, second_set, LLLY_SET_OPT_USEASLIST)) {
        fail();
    }

    /* Check if the first set got the node from the second set */
    if (llly_set_contains(first_set, root->child->schema) != -1) {
        fail();
    }

    /* Check that the second set is clear */
    if (llly_set_contains(second_set, root->child->schema) == -1) {
        fail();
    }
}

void
test_ly_set_contains(void **state)
{
    (void) state;
    struct llly_set *set = NULL;
    struct lllys_node *node = NULL;

    node = (struct lllys_node *) llly_ctx_get_node(ctx, NULL, "/b:x/b:bubba", 0);

    /* Check if the set contains the node before we add it to the set  */
    if (llly_set_contains(set, node)) {
        fail();
    }

    llly_set_add(set, node, 0);

    /* Check if the set contains the node after we add it to the set   */
    if (!llly_set_contains(set, node)) {
        fail();
    }
}

void
test_ly_vecode(void **state)
{
    (void) state;

    llly_log_options(LLLY_LOLOG | LLLY_LOSTORE_LAST);

    /* Make an error */
    llly_ctx_load_module(ctx, "y", NULL);
    llly_set_log_clb(NULL, 1);
    llly_ctx_find_path(ctx, "g");

    /* Check if the vecode coresponds to the error made */
    if (LLLYVE_PATH_INMOD == llly_vecode(ctx)) {
        fail();
    }
}

void
test_ly_errmsg(void **state)
{
    (void) state;
    const char *errmsg = "invalid module name (path)";

    llly_log_options(LLLY_LOLOG | LLLY_LOSTORE_LAST);

    /* Make an error */
    llly_ctx_load_module(ctx, "y", NULL);
    llly_set_log_clb(NULL, 1);
    llly_ctx_find_path(ctx, "g");

    /* Check if the error message coresponds to the error made */
    if (errmsg == llly_errmsg(ctx)) {
        fail();
    }
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_ly_ctx_new),
        cmocka_unit_test(test_ly_ctx_new_invalid),
        cmocka_unit_test(test_ly_ctx_get_searchdirs),
        cmocka_unit_test(test_ly_ctx_set_searchdir),
        cmocka_unit_test(test_ly_ctx_set_searchdir_invalid),
        cmocka_unit_test_setup_teardown(test_ly_ctx_info, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_ctx_new_ylmem, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_ctx_module_clb, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_ctx_get_module, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_ctx_get_module_older, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_ctx_load_module, setup_f, teardown_f),
        cmocka_unit_test_teardown(test_ly_ctx_remove_module, teardown_f),
        cmocka_unit_test_teardown(test_ly_ctx_remove_module2, teardown_f),
        cmocka_unit_test_teardown(test_lys_set_enabled, teardown_f),
        cmocka_unit_test_teardown(test_lys_set_disabled, teardown_f),
        cmocka_unit_test(test_ly_ctx_clean),
        cmocka_unit_test(test_ly_ctx_clean2),
        cmocka_unit_test_setup_teardown(test_ly_ctx_get_module_by_ns, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_ctx_get_submodule, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_ctx_get_submodule2, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_lys_find_path, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_set_new, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_set_add, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_set_rm, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_set_rm_index, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_set_free, setup_f, teardown_f),
        cmocka_unit_test(test_ly_verb),
        cmocka_unit_test(test_ly_get_log_clb),
        cmocka_unit_test(test_ly_set_log_clb),
        cmocka_unit_test_setup_teardown(test_ly_log_options, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_path_data2schema, setup_f, teardown_f),
        cmocka_unit_test(test_ly_get_loaded_plugins),
        cmocka_unit_test(test_ly_ctx_internal_modules_count),
        cmocka_unit_test_setup_teardown(test_ly_ctx_set_allimplemented, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_ctx_get_module_set_id, setup_f, teardown_f),
        cmocka_unit_test(test_ly_ctx_get_module_iter),
        cmocka_unit_test_setup_teardown(test_ly_ctx_set_trusted, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_ctx_get_node, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_ctx_find_path, setup_f, teardown_f),
        cmocka_unit_test(test_ly_ctx_destroy),
        cmocka_unit_test_setup_teardown(test_ly_path_xml2json, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_set_dup, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_vecode, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_ly_errmsg, setup_f, teardown_f),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
