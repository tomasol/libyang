/**
 * @file test_xpath.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief Cmocka tests for XPath expression evaluation.
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
    struct lllyd_node *dt;
};

static const char *if_data =
"{"
  "\"ietf-interfaces:interfaces\": {"
    "\"interface\": ["
      "{"
        "\"name\": \"iface1\","
        "\"description\": \"iface1 dsc\","
        "\"type\": \"iana-if-type:ethernetCsmacd\","
        "\"@type\": {"
          "\"yang:type_attr\":\"1\""
        "},"
        "\"enabled\": true,"
        "\"link-up-down-trap-enable\": \"disabled\","
        "\"ietf-ip:ipv4\": {"
          "\"@\": {"
            "\"yang:ip_attr\":\"14\""
          "},"
          "\"enabled\": true,"
          "\"forwarding\": true,"
          "\"mtu\": 68,"
          "\"address\": ["
            "{"
              "\"ip\": \"10.0.0.1\","
              "\"netmask\": \"255.0.0.0\""
            "},"
            "{"
              "\"ip\": \"172.0.0.1\","
              "\"prefix-length\": 16"
            "}"
          "],"
          "\"neighbor\": ["
            "{"
              "\"ip\": \"10.0.0.2\","
              "\"link-layer-address\": \"01:34:56:78:9a:bc:de:f0\""
            "}"
          "]"
        "},"
        "\"ietf-ip:ipv6\": {"
          "\"@\": {"
            "\"yang:ip_attr\":\"16\""
          "},"
          "\"enabled\": true,"
          "\"forwarding\": false,"
          "\"mtu\": 1280,"
          "\"address\": ["
            "{"
              "\"ip\": \"2001:abcd:ef01:2345:6789:0:1:1\","
              "\"prefix-length\": 64"
            "}"
          "],"
          "\"neighbor\": ["
            "{"
              "\"ip\": \"2001:abcd:ef01:2345:6789:0:1:2\","
              "\"link-layer-address\": \"01:34:56:78:9a:bc:de:f0\""
            "}"
          "],"
          "\"dup-addr-detect-transmits\": 52,"
          "\"autoconf\": {"
            "\"create-global-addresses\": true,"
            "\"create-temporary-addresses\": false,"
            "\"temporary-valid-lifetime\": 600,"
            "\"temporary-preferred-lifetime\": 300"
          "}"
        "}"
      "},"
      "{"
        "\"name\": \"iface2\","
        "\"description\": \"iface2 dsc\","
        "\"type\": \"iana-if-type:softwareLoopback\","
        "\"@type\": {"
          "\"yang:type_attr\":\"2\""
        "},"
        "\"enabled\": false,"
        "\"link-up-down-trap-enable\": \"disabled\","
        "\"ietf-ip:ipv4\": {"
          "\"@\": {"
            "\"yang:ip_attr\":\"24\""
          "},"
          "\"address\": ["
            "{"
              "\"ip\": \"10.0.0.5\","
              "\"netmask\": \"255.0.0.0\""
            "},"
            "{"
              "\"ip\": \"172.0.0.5\","
              "\"prefix-length\": 16"
            "}"
          "],"
          "\"neighbor\": ["
            "{"
              "\"ip\": \"10.0.0.1\","
              "\"link-layer-address\": \"01:34:56:78:9a:bc:de:fa\""
            "}"
          "]"
        "},"
        "\"ietf-ip:ipv6\": {"
          "\"@\": {"
            "\"yang:ip_attr\":\"26\""
          "},"
          "\"address\": ["
            "{"
              "\"ip\": \"2001:abcd:ef01:2345:6789:0:1:5\","
              "\"prefix-length\": 64"
            "}"
          "],"
          "\"neighbor\": ["
            "{"
              "\"ip\": \"2001:abcd:ef01:2345:6789:0:1:1\","
              "\"link-layer-address\": \"01:34:56:78:9a:bc:de:fa\""
            "}"
          "],"
          "\"dup-addr-detect-transmits\": 100,"
          "\"autoconf\": {"
            "\"create-global-addresses\": true,"
            "\"create-temporary-addresses\": false,"
            "\"temporary-valid-lifetime\": 600,"
            "\"temporary-preferred-lifetime\": 300"
          "}"
        "}"
      "}"
    "]"
  "}"
"}"
;

static const char *num_data =
"{"
  "\"numbers:nums\": {"
    "\"num1\": 9223372036854775807,"
    "\"num2\": 18446744073709551615,"
    "\"num3\": -2147483648,"
    "\"num4\": 4294967295,"
    "\"num5\": 9.87654321e+4,"
    "\"num6\": 987654321098765E-10,"
    "\"num7\": -922337203685477580.8,"
    "\"num8\": 922337203685477580.7,"
    "\"num9\": -9.223372036854775808,"
    "\"num10\": 9.223372036854775807,"
    "\"num11\": -92233720368.54775808e-10,"
    "\"num12\": 92233720.36854775807e10"
  "}"
"}"
;

static const char *text_schema =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
"<module name=\"ietf-anydata\""
        "xmlns=\"urn:ietf:params:xml:ns:yang:yin:1\""
        "xmlns:anydata=\"urn:ietf:params:xml:ns:yang:ietf-anydata\""
	"xmlns:if=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\""
	"xmlns:yang=\"urn:ietf:params:xml:ns:yang:ietf-yang-types\">"
  "<namespace uri=\"urn:ietf:params:xml:ns:yang:ietf-anydata\"/>"
  "<prefix value=\"anydata\"/>"
  "<import module=\"ietf-interfaces\">"
    "<prefix value=\"if\"/>"
  "</import>"
  "<import module=\"ietf-yang-types\">"
    "<prefix value=\"yang\"/>"
  "</import>"
  "<organization>"
    "<text>IETF NETMOD (NETCONF Data Modeling Language) Working Group</text>"
  "</organization>"

  "<container name=\"anydata-con\">"
    "<leaf name=\"leaf1\">"
      "<type name=\"boolean\"/>"
    "</leaf>"
    "<anydata name=\"anyvalue\">"
      "<description>"
        "<text> this is an example type anydata</text>"
      "</description>"
    "</anydata>"
  "</container>"
"</module>"
;

/* Non-number after decimal point */
static const char *error_num_data_001 =
"{"
  "\"numbers:nums\": {"
    "\"num1\": -0.abcd"
  "}"
"}"
;

/* Null after decimal point */
static const char *error_num_data_002 =
"{"
  "\"numbers:nums\": {"
    "\"num1\": 9.\0"
  "}"
"}"
;

/* not all numbers after the decimal point*/
static const char *error_num_data_003 =
"{"
  "\"numbers:nums\": {"
    "\"num1\": 9.02abcd"
  "}"
"}"
;

/* Non-number before the decimal point */
static const char *error_num_data_004 =
"{"
  "\"numbers:nums\": {"
    "\"num1\": .123456"
  "}"
"}"
;

/* the error_num_data is -.123456e+4 */
static const char *error_num_data_005 =
"{"
  "\"numbers:nums\": {"
    "\"num1\": -.123456e+4"
  "}"
"}"
;

/* the string length is greater than 1024 - 3 */
static const char *string_data_001 =
"{"
  "\"kkkkeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	"yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy\" : \"value1\""
"}"
;

/* The string contains the escape character '\\\"' */
static const char *string_data_002 =
"{"
  "\"key\\\"key\" : \"value2\""
"}"
;

/* The string contains the escape character '\\\\' */
static const char *string_data_003 =
"{"
  "\"key\\\\key\" : \"value3\""
"}"
;

/* The string contains the escape character '\\/' */
static const char *string_data_004 =
"{"
  "\"key\\/key\" : \"value4\""
"}"
;

/* The string contains the backspace character '\\b' */
static const char *string_data_005 =
"{"
  "\"key\\bkey\" : \"value5\""
"}"
;

/* The string contains the form feed character '\\f' */
static const char *string_data_006 =
"{"
  "\"key\\fkey\" : \"value6\""
"}"
;

/* The string contains the line feed character '\\n' */
static const char *string_data_007 =
"{"
  "\"key\\nkey\" : \"value7\""
"}"
;

/* The string contains the carriage return character '\\r' */
static const char *string_data_008 =
"{"
  "\"key\\rkey\" : \"value8\""
"}"
;

/* The string contains the tab character '\\t' */
static const char *string_data_009 =
"{"
  "\"key\\tkey\" : \"value9\""
"}"
;

/* The string contains the Basic Multilingual Plane character '\\u' , the format \u[a-z][A-Z]* */
static const char *string_data_010 =
"{"
  "\"key\\ukey\" : \"value10\""
"}"
;

/* The string contains the Basic Multilingual Plane character '\\u' , the format \u[0-9]* */
static const char *string_data_011 =
"{"
  "\"key\\u123\" : \"value11\""
"}"
;

/* The string contains the characters('\r') which ascii code is less than 0x20  */
static const char *string_data_012 =
"{"
  "\"key\rkey\" : \"value12\""
"}"
;

/* the anydata value contains invalid escape sequence '\\g' */
static const char *string_data_013 =
"{"
  "\"ietf-anydata:anydata-con\" : {"
        "\"anyvalue\" : \"value13\\g\""
  "}"
"}"
;

/* the tailing character of anydata value is not '\"' */
static const char *string_data_014 =
"{"
  "\"ietf-anydata:anydata-con\" : {"
        "\"anyvalue\" : \"value14}"
  "}"
"}"
;

/* the heading character of anydata value is just '{' */
static const char *string_data_015 =
"{"
  "\"ietf-anydata:anydata-con\" : {"
        "\"anyvalue\" : {\0"
  "}"
"}"
;

/* the heading character of anydata value are not '{' or '\"' */
static const char *string_data_016 =
"{"
  "\"ietf-anydata:anydata-con\" : {"
        "\"anyvalue\" : value16\""
  "}"
"}"
;

/* the anydata value is normal */
static const char *string_data_017 =
"{"
  "\"ietf-anydata:anydata-con\" : {"
        "\"anyvalue\" : \"value17\""
  "}"
"}"
;

/* the boolean value are not "true" or "false" */
static const char *string_data_018 =
"{"
  "\"ietf-anydata:anydata-con\" : {"
        "\"leaf1\" : falue18"
  "}"
"}"
;

/* the heading character of anydata value is not '\"'  */
static const char *string_data_019 =
"{"
  "ietf-anydata:anydata-con\" : {"
        "\"leaf1\" : value19"
  "}"
"}"
;

/* the attributes is't the root node */
static const char *string_data_020 =
"{"
  "\"ietf-anydata:anydata-con\" : {"
        "\"@\" : \"value20\""
  "}"
"}"
;

/* the attributes is null */
static const char *string_data_021 =
"{"
  "\"ietf-anydata:anydata-con\" : {"
        "\"@\" : null"
  "}"
"}"
;

/* the heading character of attributes is '{' */
static const char *string_data_022 =
"{"
  "\"ietf-anydata:anydata-con\" : {"
        "\"@\" : {value22"
  "}"
"}"
;

/* the tailing character of attributes is not '\"' */
static const char *string_data_023 =
"{"
  "\"ietf-anydata:anydata-con\" : {"
        "\"@\" : {\"value23"
  "}"
"}"
;

/* the attributes is the root node */
static const char *string_data_024 =
"{"
  "\"@\" : {"
        "\"leaf1\" : \"value24\""
  "}"
"}"
;

static int
setup_f(struct state **state, const char *search_dir, const char **modules, int module_count)
{
    const struct lllys_module *mod;
    int i;

    (*state) = calloc(1, sizeof **state);
    if (!(*state)) {
        fprintf(stderr, "Memory allocation error.\n");
        return -1;
    }

    /* libyang context */
    (*state)->ctx = llly_ctx_new(search_dir, 0);
    if (!(*state)->ctx) {
        fprintf(stderr, "Failed to create context.\n");
        goto error;
    }

    /* schemas */
    for (i = 0; i < module_count; ++i) {
        mod = llly_ctx_load_module((*state)->ctx, modules[i], NULL);
        if (!mod) {
            fprintf(stderr, "Failed to load data module \"%s\".\n", modules[i]);
            goto error;
        }
        lllys_features_enable(mod, "*");
    }

    return 0;

error:
    llly_ctx_destroy((*state)->ctx, NULL);
    free(*state);
    (*state) = NULL;

    return -1;
}

static int
teardown_f(void **state)
{
    struct state *st = (*state);

    lllyd_free_withsiblings(st->dt);
    llly_ctx_destroy(st->ctx, NULL);
    free(st);
    (*state) = NULL;

    return 0;
}

static void
test_parse_if(void **state)
{
    struct state *st;
    const char *modules[] = {"ietf-interfaces", "ietf-ip", "iana-if-type"};
    int module_count = 3;

    if (setup_f(&st, TESTS_DIR "/schema/yin/ietf", modules, module_count)) {
        fail();
    }

    (*state) = st;

    st->dt = lllyd_parse_mem(st->ctx, if_data, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_not_equal(st->dt, NULL);
}

static void
test_parse_numbers(void **state)
{
    struct lllyd_node_leaf_list *leaf;
    struct state *st;
    const char *modules[] = {"numbers"};
    int module_count = 1;

    if (setup_f(&st, TESTS_DIR "/data/files", modules, module_count)) {
        fail();
    }

    (*state) = st;

    st->dt = lllyd_parse_mem(st->ctx, num_data, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_not_equal(st->dt, NULL);

    /* num1 */
    leaf = (struct lllyd_node_leaf_list *)st->dt->child;

    /* num2 */
    leaf = (struct lllyd_node_leaf_list *)leaf->next;

    /* num3 */
    leaf = (struct lllyd_node_leaf_list *)leaf->next;

    /* num4 */
    leaf = (struct lllyd_node_leaf_list *)leaf->next;

    /* num5 */
    leaf = (struct lllyd_node_leaf_list *)leaf->next;

    /* num6 */
    leaf = (struct lllyd_node_leaf_list *)leaf->next;

    /* num7 */
    leaf = (struct lllyd_node_leaf_list *)leaf->next;

    /* num8 */
    leaf = (struct lllyd_node_leaf_list *)leaf->next;

    /* num9 */
    leaf = (struct lllyd_node_leaf_list *)leaf->next;

    /* num10 */
    leaf = (struct lllyd_node_leaf_list *)leaf->next;

    /* num11 */
    leaf = (struct lllyd_node_leaf_list *)leaf->next;

    /* num12 */
    leaf = (struct lllyd_node_leaf_list *)leaf->next;

}

static void
test_parse_error_numbers(void **state)
{
    struct state *st;
    const char *modules[] = {"numbers"};
    int module_count = 1;

    if (setup_f(&st, TESTS_DIR "/data/files", modules, module_count)) {
        fail();
    }

    (*state) = st;

    st->dt = lllyd_parse_mem(st->ctx, error_num_data_001, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, error_num_data_002, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, error_num_data_003, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, error_num_data_004, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, error_num_data_005, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);
}

static void
test_parse_string(void **state)
{
    struct state *st;
    const char *modules[] = {"ietf-interfaces"};
    int module_count = 1;
    const struct lllys_module *mod;

    if (setup_f(&st, TESTS_DIR "/schema/yin/ietf", modules, module_count)) {
        fail();
    }

    (*state) = st;

    mod = lllys_parse_mem(st->ctx, text_schema, LLLYS_IN_YIN);
    assert_ptr_not_equal(mod, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_001, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_002, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_003, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_004, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_005, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_006, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_007, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_008, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_009, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_010, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_011, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_012, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_013, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_014, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_015, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_016, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_017, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_free_withsiblings(st->dt);

    st->dt = lllyd_parse_mem(st->ctx, string_data_018, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_019, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_020, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_021, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_not_equal(st->dt, NULL);
    lllyd_free_withsiblings(st->dt);

    st->dt = lllyd_parse_mem(st->ctx, string_data_022, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_023, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);

    st->dt = lllyd_parse_mem(st->ctx, string_data_024, LLLYD_JSON, LLLYD_OPT_CONFIG);
    assert_ptr_equal(st->dt, NULL);
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
                    cmocka_unit_test_teardown(test_parse_if, teardown_f),
                    cmocka_unit_test_teardown(test_parse_numbers, teardown_f),
                    cmocka_unit_test_teardown(test_parse_error_numbers, teardown_f),
                    cmocka_unit_test_teardown(test_parse_string, teardown_f),
                    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}

