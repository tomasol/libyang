/**
 * \file test_printer.c
 * \author Michal Vasko <mvasko@cesnet.cz>
 * \brief libyang tests - printers
 *
 * Copyright (c) 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <stdarg.h>
#include <setjmp.h>
#include <stdlib.h>
#include <cmocka.h>

#include "libyang.h"
#include "tests/config.h"

#define SCHEMA_FOLDER_YANG TESTS_DIR"/schema/yang/files"

struct llly_ctx *ctx;

static int
setup_ctx(void **state)
{
    (void)state;
    ctx = llly_ctx_new(SCHEMA_FOLDER_YANG, 0);
    if (!ctx) {
        return -1;
    }

    return 0;
}

static int
teardown_ctx(void **state)
{
    (void)state;
    llly_ctx_destroy(ctx, NULL);
    return 0;
}

static void
test_tree(void **state)
{
   (void)state;
   char *str;
   const struct lllys_module *mod1, *mod2, *mod2_sub;
   const char *model_name = "tree2_sub";

   mod1 = llly_ctx_load_module(ctx, "tree1", NULL);
   assert_ptr_not_equal(mod1, NULL);
   mod2=llly_ctx_load_module(ctx, "tree2", NULL);
   assert_ptr_not_equal(mod2, NULL);
   const char temp1[] = "module: tree1\n"
   "  +--rw cont\n"
   "  |  +--rw leaf4?         uint8\n"
   "  |  +--rw leaf3?         string\n"
   "  |  +--rw tree2:list1* [key1]\n"
   "  |     +--rw tree2:key1     -> /tree1:cont/list1/leaf2\n"
   "  |     +--rw tree2:key2?    -> /tree2:leaf2\n"
   "  |     +--rw (tree2:ch1)? <ca>\n"
   "  |     |  +--:(tree2:ca)\n"
   "  |     +--rw tree2:leaf2?   string\n"
   "  +--rw any?    anyxml\n"
   "\n"
   "  rpcs:\n"
   "    +---x rpc1\n"
   "    |  +---- input\n"
   "    |  |  +---w in?   string\n"
   "    |  +---- output\n"
   "    |     +--ro out?   int8\n"
   "    +---x rpc2\n"
   "\n"
   "  notifications:\n"
   "    +---n notif1\n"
   "    +---n notif2\n";
   lllys_print_mem(&str, mod1, LLLYS_OUT_TREE, NULL, 0, 0);
   assert_string_equal(str, temp1);
   free(str);

   const char temp2[] = "module: tree1\n"
   "  +--rw cont\n"
   "  |  +--rw leaf4?         uint8\n"
   "  |  +---u group2\n"
   "  |  +--rw tree2:list1* [key1]\n"
   "  |     +--rw tree2:key1        -> /tree1:cont/list1/leaf2\n"
   "  |     +--rw tree2:key2?       -> /tree2:leaf2\n"
   "  |     +---u tree2:t1:group1\n"
   "  +--rw any?    anyxml\n"
   "\n"
   "  rpcs:\n"
   "    +---x rpc1\n"
   "    |  +---- input\n"
   "    |  |  +---w in?   string\n"
   "    |  +---- output\n"
   "    |     +--ro out?   int8\n"
   "    +---x rpc2\n"
   "\n"
   "  notifications:\n"
   "    +---n notif1\n"
   "    +---n notif2\n"
   "\n"
   "  grouping group1:\n"
   "    +---- (ch1)? <ca>\n"
   "    |  +--:(ca)\n"
   "    +---- leaf2?   string\n"
   "  grouping group2:\n"
   "    +---- leaf3?   string\n";
   lllys_print_mem(&str, mod1, LLLYS_OUT_TREE, NULL, 0, LLLYS_OUTOPT_TREE_GROUPING | LLLYS_OUTOPT_TREE_USES);
   assert_string_equal(str, temp2);
   free(str); 
   
   const char temp3[] = "module: tree1\n"
   "  +--rw cont\n"
   "  |  +--rw leaf4?         uint8\n"
   "  |  +--rw leaf3?         string\n"
   "  |  +--rw tree2:list1* [key1]\n"
   "  |     +--rw tree2:key1     leafref\n"
   "  |     +--rw tree2:key2?    leafref\n"
   "  |     +--rw (tree2:ch1)? <ca>\n"
   "  |     |  +--:(tree2:ca)\n"
   "  |     +--rw tree2:leaf2?   string\n"
   "  +--rw any?    anyxml\n"
   "\n"
   "  rpcs:\n"
   "    +---x rpc1\n"
   "    |  +---- input\n"
   "    |  |  +---w in?   string\n"
   "    |  +---- output\n"
   "    |     +--ro out?   int8\n"
   "    +---x rpc2\n"
   "\n"
   "  notifications:\n"
   "    +---n notif1\n"
   "    +---n notif2\n";
   lllys_print_mem(&str, mod1, LLLYS_OUT_TREE, NULL, 0, LLLYS_OUTOPT_TREE_NO_LEAFREF);
   assert_string_equal(str, temp3);
   free(str);
   
   const char temp4[] = "module: tree2\n"
   "  +--rw (ch2)? <ca>\n"
   "  |  +--:(ca)\n"
   "  |  |  +--rw presence!\n"
   "  |  +--:(leaf2)\n"
   "  |  |  +--rw leaf2?   string\n"
   "  |  +--:(cb)\n"
   "  |     +--rw presence1!\n"
   "  +--rw leaf1?   string <test tree>\n"
   "  +--rw ll*      tree1:type1\n"
   "\n"
   "  augment /tree1:cont:\n"
   "    +--rw list1* [key1]\n"
   "       +--rw key1     leafref\n"
   "       +--rw key2?    leafref\n"
   "       +--rw (ch1)? <ca>\n"
   "       |  +--:(ca)\n"
   "       +--rw leaf2?   string\n";   
   lllys_print_mem(&str, mod2, LLLYS_OUT_TREE, NULL, 0, LLLYS_OUTOPT_TREE_NO_LEAFREF);
   assert_string_equal(str, temp4);
   free(str); 
  
   mod2_sub = (const struct lllys_module *)llly_ctx_get_submodule(ctx, NULL, NULL, model_name, 0);
   if (!mod2_sub) {
       fprintf(stderr, "No submodule \"%s\" found.\n", model_name);
   }
   assert_ptr_not_equal(mod2_sub, NULL);
   
   const char temp5[] = "submodule: tree2_sub (belongs-to tree2)\n"
   "  +--rw (ch2)? <ca>\n"
   "  |  +--:(ca)\n"
   "  |  |  +--rw presence!\n"
   "  |  +--:(leaf2)\n"
   "  |  |  +--rw leaf2?   string\n";
   lllys_print_mem(&str, mod2_sub, LLLYS_OUT_TREE, NULL, 0, LLLYS_OUTOPT_TREE_NO_LEAFREF);
   assert_string_equal(str, temp5);
   free(str);
}

static void
test_tree_rfc(void **state)
{
    (void)state;
    char *str;
    const struct lllys_module *moda, *modb, *mod2, *mod2_sub;
    const char *model_name = "tree2_sub";

    moda = llly_ctx_load_module(ctx, "tree-a", NULL);
    assert_ptr_not_equal(moda, NULL);
    modb = llly_ctx_load_module(ctx, "tree-b", NULL);
    assert_ptr_not_equal(modb, NULL);
    mod2 = llly_ctx_load_module(ctx, "tree2", NULL);
    assert_ptr_not_equal(mod2, NULL);

    const char temp1[] = "module: tree-a\n"
    "  +--rw cont\n"
    "     +--rw leaf3?      uint8\n"
    "     +--rw tb:list1* [key1]\n"
    "        +--rw tb:key1     -> /ta:cont/list1/leaf1\n"
    "        +--rw tb:leaf1?   string\n"
    "\n"
    "  rpcs:\n"
    "    +---x rpc1\n"
    "    +---x rpc2\n"
    "\n"
    "  notifications:\n"
    "    +---n notif1\n"
    "    +---n notif2\n";
    lllys_print_mem(&str, moda, LLLYS_OUT_TREE, NULL, 0, LLLYS_OUTOPT_TREE_RFC);
    assert_string_equal(str, temp1);
    free(str);

    const char temp2[] = "module: tree-a\n"
    "  +--rw cont\n"
    "     +--rw leaf3?      uint8\n"
    "     +--rw tb:list1* [key1]\n"
    "        +--rw tb:key1        -> /ta:cont/list1/leaf1\n"
    "        +---u tb:ta:group1\n"
    "\n"
    "  rpcs:\n"
    "    +---x rpc1\n"
    "    +---x rpc2\n"
    "\n"
    "  notifications:\n"
    "    +---n notif1\n"
    "    +---n notif2\n"
    "\n"
    "  grouping group1:\n"
    "    +---- leaf1?   string\n"
    "  grouping group2:\n"
    "    +---- leaf2?   string\n";
    lllys_print_mem(&str, moda, LLLYS_OUT_TREE, NULL, 0, LLLYS_OUTOPT_TREE_RFC | LLLYS_OUTOPT_TREE_GROUPING | LLLYS_OUTOPT_TREE_USES);
    assert_string_equal(str, temp2);
    free(str);

    const char temp3[] = "module: tree-a\n"
    "  +--rw cont\n"
    "     +--rw leaf3?      uint8\n"
    "     +--rw tb:list1* [key1]\n"
    "        +--rw tb:key1     leafref\n"
    "        +--rw tb:leaf1?   string\n"
    "\n"
    "  rpcs:\n"
    "    +---x rpc1\n"
    "    +---x rpc2\n"
    "\n"
    "  notifications:\n"
    "    +---n notif1\n"
    "    +---n notif2\n";
    lllys_print_mem(&str, moda, LLLYS_OUT_TREE, NULL, 0, LLLYS_OUTOPT_TREE_RFC | LLLYS_OUTOPT_TREE_NO_LEAFREF);
    assert_string_equal(str, temp3);
    free(str);

    const char temp4[] = "module: tree-b\n"
    "\n"
    "  augment /ta:cont:\n"
    "    +--rw list1* [key1]\n"
    "       +--rw key1     -> /ta:cont/list1/leaf1\n"
    "       +--rw leaf1?   string\n";
    lllys_print_mem(&str, modb, LLLYS_OUT_TREE, NULL, 0, LLLYS_OUTOPT_TREE_RFC);
    assert_string_equal(str, temp4);
    free(str);

    const char temp5[] = "module: tree2\n"
    "  +--rw (ch2)?\n"
    "  |  +--:(ca)\n"
    "  |  |  +--rw presence!\n"
    "  |  +--:(leaf2)\n"
    "  |  |  +--rw leaf2?   string\n"
    "  |  +--:(cb)\n"
    "  |     +--rw presence1!\n"
    "  +--rw leaf1?   string\n"
    "  +--rw ll*      t1:type1\n"
    "\n"
    "  augment /t1:cont:\n"
    "    +--rw list1* [key1]\n"
    "       +--rw key1     -> /t1:cont/list1/leaf2\n"
    "       +--rw key2?    -> /t2:leaf2\n"
    "       +--rw (ch1)?\n"
    "       |  +--:(ca)\n"
    "       +--rw leaf2?   string\n";
    lllys_print_mem(&str, mod2, LLLYS_OUT_TREE, NULL, 0, LLLYS_OUTOPT_TREE_RFC);
    assert_string_equal(str, temp5);
    free(str);

    mod2_sub = (const struct lllys_module *)llly_ctx_get_submodule(ctx, NULL, NULL, model_name, 0);
    if (!mod2_sub) {
        fprintf(stderr, "No submodule \"%s\" found.\n", model_name);
    }
    assert_ptr_not_equal(mod2_sub, NULL);
    const char temp6[] = "submodule: tree2_sub\n"
    "  +--rw (ch2)?\n"
    "  |  +--:(ca)\n"
    "  |  |  +--rw presence!\n"
    "  |  +--:(leaf2)\n"
    "  |  |  +--rw leaf2?   string\n";
    lllys_print_mem(&str, mod2_sub, LLLYS_OUT_TREE, NULL, 0, LLLYS_OUTOPT_TREE_RFC);
    assert_string_equal(str, temp6);
    free(str);
}

static void
test_tree_rfc_subtree(void **state)
{
    (void)state;
    char *str;
    const struct lllys_module *moda, *modb;

    moda = llly_ctx_load_module(ctx, "tree-a", NULL);
    assert_ptr_not_equal(moda, NULL);
    modb = llly_ctx_load_module(ctx, "tree-b", NULL);
    assert_ptr_not_equal(modb, NULL);

    const char temp1[] = "module: tree-a\n"
    "  +--rw cont\n"
    "     +--rw tb:list1* [key1]\n"
    "        +--rw tb:key1     -> /ta:cont/list1/leaf1\n"
    "        +--rw tb:leaf1?   string\n";
    lllys_print_mem(&str, moda, LLLYS_OUT_TREE, "/tree-a:cont/tree-b:list1", 0, LLLYS_OUTOPT_TREE_RFC);
    assert_string_equal(str, temp1);
    free(str);

    const char temp2[] = "module: tree-a\n"
    "\n"
    "  rpcs:\n"
    "    +---x rpc1\n";
    lllys_print_mem(&str, moda, LLLYS_OUT_TREE, "/tree-a:rpc1", 0, LLLYS_OUTOPT_TREE_RFC);
    assert_string_equal(str, temp2);
    free(str);

    const char temp3[] = "module: tree-a\n"
    "\n"
    "  notifications:\n"
    "    +---n notif1\n";
    lllys_print_mem(&str, moda, LLLYS_OUT_TREE, "/tree-a:notif1", 0, LLLYS_OUTOPT_TREE_RFC);
    assert_string_equal(str, temp3);
    free(str);
    
    const char temp4[] = "module: tree-a\n"
    "  +--rw cont\n"
    "     +--rw leaf3?   uint8\n";
    lllys_print_mem(&str, moda, LLLYS_OUT_TREE, "/tree-a:cont/leaf3", 0, LLLYS_OUTOPT_TREE_RFC);
    assert_string_equal(str, temp4);
    free(str);

    /* target node not found */
    lllys_print_mem(&str, moda, LLLYS_OUT_TREE, "/tree-a:unknown", 0, LLLYS_OUTOPT_TREE_RFC);
    free(str);
}

static void
test_tree_rfc_line_length(void **state)
{
    (void)state;
    char *str;
    const struct lllys_module *modc, *modd;

    modc = llly_ctx_load_module(ctx, "tree-c", NULL);
    assert_ptr_not_equal(modc, NULL);
    modd = llly_ctx_load_module(ctx, "tree-d", NULL);
    assert_ptr_not_equal(modd, NULL);
    assert_int_equal(lllys_features_enable(modd, "feat1"), 0);

    const char temp1[] = "module: tree-c\n"
    "  +--rw cont!\n"
    "     +--rw cont2\n"
    "     |  +--rw list1* [key1]\n"
    "     |     +--rw key1\n"
    "     |     |       string\n"
    "     |     +--rw cont3\n"
    "     |        +--rw td:leaf3?\n"
    "     |                uint8\n"
    "     +--rw td:any?\n"
    "     |       anydata\n"
    "     +--rw td:leaf1?\n"
    "     |       string\n"
    "     |       {td:feat1}?\n"
    "     +--rw td:leaf2?\n"
    "     |       -> /tc:cont/td:leaf1\n"
    "     |       {td:feat1}?\n"
    "     +--rw td:llist1*\n"
    "     |       string\n"
    "     +--rw td:list1* [key1]\n"
    "             {td:feat1}?\n"
    "        +--rw td:key1\n"
    "        |       uint8\n"
    "        +--rw td:list2*\n"
    "                [key2]\n"
    "           +--rw td:key2\n"
    "                   uint16\n";
    lllys_print_mem(&str, modc, LLLYS_OUT_TREE, NULL, 27, LLLYS_OUTOPT_TREE_RFC);
    assert_string_equal(str, temp1);
    free(str);

    const char temp2[] = "module: tree-c\n"
    "  +--rw cont!\n"
    "     +--rw cont2\n"
    "     |  +--rw list1* [key1]\n"
    "     |     +--rw key1\n"
    "     |     |       string\n"
    "     |     +--rw cont3\n"
    "     |        +--rw td:leaf3?\n"
    "     |                uint8\n"
    "     +--rw td:any?      anydata\n"
    "     +--rw td:leaf1?    string\n"
    "     |       {td:feat1}?\n"
    "     +--rw td:leaf2?    leafref\n"
    "     |       {td:feat1}?\n"
    "     +--rw td:llist1*   string\n"
    "     +--rw td:list1* [key1]\n"
    "             {td:feat1}?\n"
    "        +--rw td:key1     uint8\n"
    "        +--rw td:list2* [key2]\n"
    "           +--rw td:key2\n"
    "                   uint16\n";
    lllys_print_mem(&str, modc, LLLYS_OUT_TREE, NULL, 31, LLLYS_OUTOPT_TREE_RFC | LLLYS_OUTOPT_TREE_NO_LEAFREF);
    assert_string_equal(str, temp2);
    free(str);

    const char temp3[] = "module: tree-d\n"
    "\n"
    "  augment /tc:cont:\n"
    "    +--rw any?\n"
    "    |       anydata\n"
    "    +--rw leaf1?\n"
    "    |       string\n"
    "    |       {feat1}?\n"
    "    +--rw leaf2?\n"
    "    |       -> /tc:cont/td:leaf1\n"
    "    |       {feat1}?\n"
    "    +--rw llist1*\n"
    "    |       string\n"
    "    +--rw list1* [key1]\n"
    "            {feat1}?\n"
    "       +--rw key1\n"
    "       |       uint8\n"
    "       +--rw list2*\n"
    "               [key2]\n"
    "          +--rw key2\n"
    "                  uint16\n"
    "  augment /tc:cont\n"
    "            /tc:cont2\n"
    "            /tc:list1\n"
    "            /tc:cont3:\n"
    "    +--rw leaf3?   uint8\n";
    lllys_print_mem(&str, modd, LLLYS_OUT_TREE, NULL, 24, LLLYS_OUTOPT_TREE_RFC);
    assert_string_equal(str, temp3);
    free(str);

    const char temp4[] = "module: tree-d\n"
    "\n"
    "  augment /tc:cont:\n"
    "    +--rw any?      anydata\n"
    "    +--rw leaf1?    string\n"
    "    |       {feat1}?\n"
    "    +--rw leaf2?\n"
    "    |       -> /tc:cont/td:leaf1\n"
    "    |       {feat1}?\n"
    "    +--rw llist1*   string\n"
    "    +--rw list1* [key1]\n"
    "            {feat1}?\n"
    "       +--rw key1     uint8\n"
    "       +--rw list2* [key2]\n"
    "          +--rw key2    uint16\n"
    "  augment /tc:cont/tc:cont2\n"
    "            /tc:list1/tc:cont3:\n"
    "    +--rw leaf3?   uint8\n";
    lllys_print_mem(&str, modd, LLLYS_OUT_TREE, NULL, 31, LLLYS_OUTOPT_TREE_RFC);
    assert_string_equal(str, temp4);
    free(str);
}

static void
test_parse_yin_with_unique(void **state)
{
    (void)state;
    char *schema = NULL;
    const struct lllys_module *modyang = NULL, *modyang2 = NULL;
    struct llly_ctx *ctx2 = NULL;
    int ret = 0;

    modyang = llly_ctx_load_module(ctx, "parse-yin-yang-with-unique", NULL);
    assert_non_null(modyang);

    ret = lllys_print_mem(&schema, modyang, LLLYS_OUT_YIN, NULL, 0, 0);
    assert_int_equal(ret, 0);
    assert_non_null(schema);

    ctx2 = llly_ctx_new(NULL, 0);
    modyang2 = lllys_parse_mem(ctx2, schema, LLLYS_IN_YIN);
    assert_non_null(modyang2);
    llly_ctx_destroy(ctx2, NULL);

    free(schema);
}

static void
test_parse_yang_with_unique(void **state)
{
    (void)state;
    char *schema = NULL;
    const struct lllys_module *modyang = NULL, *modyang2 = NULL;
    struct llly_ctx *ctx2 = NULL;
    int ret = 0;

    modyang = llly_ctx_load_module(ctx, "parse-yin-yang-with-unique", NULL);
    assert_non_null(modyang);

    ret = lllys_print_mem(&schema, modyang, LLLYS_OUT_YANG, NULL, 0, 0);
    assert_int_equal(ret, 0);
    assert_non_null(schema);

    ctx2 = llly_ctx_new(NULL, 0);
    modyang2 = lllys_parse_mem(ctx2, schema, LLLYS_IN_YANG);
    assert_non_null(modyang2);
    llly_ctx_destroy(ctx2, NULL);

    free(schema);
}

static void
test_parse_yin_with_submodule_types(void **state)
{
    (void)state;
    char *schema = NULL;
    const struct lllys_module *modyang = NULL, *modyang2 = NULL;
    struct llly_ctx *ctx2 = NULL;
    int ret = 0;

    modyang = llly_ctx_load_module(ctx, "e", NULL);
    assert_non_null(modyang);

    ret = lllys_print_mem(&schema, modyang, LLLYS_OUT_YIN, NULL, 0, 0);
    assert_int_equal(ret, 0);
    assert_non_null(schema);

    ctx2 = llly_ctx_new(NULL, 0);
    llly_ctx_set_searchdir(ctx2, SCHEMA_FOLDER_YANG);
    modyang = llly_ctx_load_module(ctx2, "d", NULL);
    assert_non_null(modyang);
    llly_ctx_unset_searchdirs(ctx2, -1);

    modyang2 = lllys_parse_mem(ctx2, schema, LLLYS_IN_YIN);
    assert_non_null(modyang2);
    llly_ctx_destroy(ctx2, NULL);

    free(schema);
}

static void
test_parse_yang_with_submodule_types(void **state)
{
    (void) state;
    char *schema = NULL;
    const struct lllys_module *modyang = NULL, *modyang2 = NULL;
    struct llly_ctx *ctx2 = NULL;
    int ret = 0;

    modyang = llly_ctx_load_module(ctx, "e", NULL);
    assert_non_null(modyang);

    ret = lllys_print_mem(&schema, modyang, LLLYS_OUT_YANG, NULL, 0, 0);
    assert_int_equal(ret, 0);
    assert_non_null(schema);

    ctx2 = llly_ctx_new(NULL, 0);
    llly_ctx_set_searchdir(ctx2, SCHEMA_FOLDER_YANG);
    modyang = llly_ctx_load_module(ctx2, "d", NULL);
    assert_non_null(modyang);
    llly_ctx_unset_searchdirs(ctx2, -1);

    modyang2 = lllys_parse_mem(ctx2, schema, LLLYS_IN_YANG);
    assert_non_null(modyang2);
    llly_ctx_destroy(ctx2, NULL);

    free(schema);
}

static void
test_parse_yin_with_submodule_grouping_idref_default(void **state)
{
    (void)state;
    char *schema = NULL, *schema2 = NULL;
    const struct lllys_module *modyang = NULL, *modyang2 = NULL;
    const struct lllys_module *subyang = NULL;
    struct llly_ctx *ctx2 = NULL;
    int ret = 0;

    modyang = llly_ctx_load_module(ctx, "grp_idref_def-mod", NULL);
    assert_non_null(modyang);

    ret = lllys_print_mem(&schema, modyang, LLLYS_OUT_YIN, NULL, 0, 0);
    assert_int_equal(ret, 0);
    assert_non_null(schema);

    subyang = (const struct lllys_module *)llly_ctx_get_submodule2(modyang, "grp_idref_def-sub");
    assert_non_null(subyang);

    ret = lllys_print_mem(&schema2, subyang, LLLYS_OUT_YIN, NULL, 0, 0);
    assert_int_equal(ret, 0);
    assert_non_null(schema2);

    ctx2 = llly_ctx_new(NULL, 0);
    llly_ctx_set_searchdir(ctx2, SCHEMA_FOLDER_YANG);

    modyang2 = lllys_parse_mem(ctx2, schema, LLLYS_IN_YIN);
    assert_non_null(modyang2);

    llly_ctx_unset_searchdirs(ctx2, -1);
    llly_ctx_destroy(ctx2, NULL);

    free(schema);
    free(schema2);
}

static void
test_parse_yang_with_submodule_grouping_idref_default(void **state)
{
    (void)state;
    char *schema = NULL, *schema2 = NULL;
    const struct lllys_module *modyang = NULL, *modyang2 = NULL;
    const struct lllys_module *subyang = NULL;
    struct llly_ctx *ctx2 = NULL;
    int ret = 0;

    modyang = llly_ctx_load_module(ctx, "grp_idref_def-mod", NULL);
    assert_non_null(modyang);

    ret = lllys_print_mem(&schema, modyang, LLLYS_OUT_YANG, NULL, 0, 0);
    assert_int_equal(ret, 0);
    assert_non_null(schema);

    subyang = (const struct lllys_module *)llly_ctx_get_submodule2(modyang, "grp_idref_def-sub");
    assert_non_null(subyang);

    ret = lllys_print_mem(&schema2, subyang, LLLYS_OUT_YANG, NULL, 0, 0);
    assert_int_equal(ret, 0);
    assert_non_null(schema2);

    ctx2 = llly_ctx_new(NULL, 0);
    llly_ctx_set_searchdir(ctx2, SCHEMA_FOLDER_YANG);

    modyang2 = lllys_parse_mem(ctx2, schema, LLLYS_IN_YANG);
    assert_non_null(modyang2);

    llly_ctx_unset_searchdirs(ctx2, -1);
    llly_ctx_destroy(ctx2, NULL);

    free(schema);
    free(schema2);
}

int
main(void)
{
    const struct CMUnitTest cmut[] = {
	cmocka_unit_test_setup_teardown(test_tree, setup_ctx, teardown_ctx),
        cmocka_unit_test_setup_teardown(test_tree_rfc, setup_ctx, teardown_ctx),
        cmocka_unit_test_setup_teardown(test_tree_rfc_subtree, setup_ctx, teardown_ctx),
        cmocka_unit_test_setup_teardown(test_tree_rfc_line_length, setup_ctx, teardown_ctx),
        cmocka_unit_test_setup_teardown(test_parse_yin_with_unique, setup_ctx, teardown_ctx),
        cmocka_unit_test_setup_teardown(test_parse_yang_with_unique, setup_ctx, teardown_ctx),
        cmocka_unit_test_setup_teardown(test_parse_yin_with_submodule_types, setup_ctx, teardown_ctx),
        cmocka_unit_test_setup_teardown(test_parse_yang_with_submodule_types, setup_ctx, teardown_ctx),
        cmocka_unit_test_setup_teardown(test_parse_yin_with_submodule_grouping_idref_default, setup_ctx, teardown_ctx),
        cmocka_unit_test_setup_teardown(test_parse_yang_with_submodule_grouping_idref_default, setup_ctx, teardown_ctx),
    };

    return cmocka_run_group_tests(cmut, NULL, NULL);
}
