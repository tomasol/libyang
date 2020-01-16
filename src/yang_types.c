/**
 * @file yang_types.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Static YANG built-in-types
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <stdlib.h>

#include "tree_internal.h"

struct lllys_tpdf llly_type_binary = {
    .name = "binary",
    .module = NULL,
    .dsc = "Any binary data",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_BINARY}
};

struct lllys_tpdf llly_type_bits = {
    .name = "bits",
    .module = NULL,
    .dsc = "A set of bits or flags",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_BITS}
};

struct lllys_tpdf llly_type_bool = {
    .name = "boolean",
    .module = NULL,
    .dsc = "true or false",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_BOOL}
};

struct lllys_tpdf llly_type_dec64 = {
    .name = "decimal64",
    .module = NULL,
    .dsc = "64-bit signed decimal number",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_DEC64}
};

struct lllys_tpdf llly_type_empty = {
    .name = "empty",
    .module = NULL,
    .dsc = "A leaf that does not have any value",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_EMPTY}
};

struct lllys_tpdf llly_type_enum = {
    .name = "enumeration",
    .module = NULL,
    .dsc = "Enumerated strings",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_ENUM}
};

struct lllys_tpdf llly_type_ident = {
    .name = "identityref",
    .module = NULL,
    .dsc = "A reference to an abstract identity",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_IDENT}
};

struct lllys_tpdf llly_type_inst = {
    .name = "instance-identifier",
    .module = NULL,
    .dsc = "References a data tree node",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_INST}
};

struct lllys_tpdf llly_type_int8 = {
    .name = "int8",
    .module = NULL,
    .dsc = "8-bit signed integer",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_INT8}
};

struct lllys_tpdf llly_type_int16 = {
    .name = "int16",
    .module = NULL,
    .dsc = "16-bit signed integer",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_INT16}
};

struct lllys_tpdf llly_type_int32 = {
    .name = "int32",
    .module = NULL,
    .dsc = "32-bit signed integer",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_INT32}
};

struct lllys_tpdf llly_type_int64 = {
    .name = "int64",
    .module = NULL,
    .dsc = "64-bit signed integer",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_INT64}
};

struct lllys_tpdf llly_type_leafref = {
    .name = "leafref",
    .module = NULL,
    .dsc = "A reference to a leaf instance",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_LEAFREF}
};

struct lllys_tpdf llly_type_string = {
    .name = "string",
    .module = NULL,
    .dsc = "Human-readable string",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_STRING}
};

struct lllys_tpdf llly_type_uint8 = {
    .name = "uint8",
    .module = NULL,
    .dsc = "8-bit unsigned integer",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_UINT8}
};

struct lllys_tpdf llly_type_uint16 = {
    .name = "uint16",
    .module = NULL,
    .dsc = "16-bit unsigned integer",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_UINT16}
};

struct lllys_tpdf llly_type_uint32 = {
    .name = "uint32",
    .module = NULL,
    .dsc = "32-bit unsigned integer",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_UINT32}
};

struct lllys_tpdf llly_type_uint64 = {
    .name = "uint64",
    .module = NULL,
    .dsc = "64-bit unsigned integer",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_UINT64}
};

struct lllys_tpdf llly_type_union = {
    .name = "union",
    .module = NULL,
    .dsc = "Choice of member types",
    .ref = "RFC 6020, section 4.2.4",
    .flags = 0,
    .type = {.base = LLLY_TYPE_UNION}
};

struct lllys_tpdf *llly_types[LLLY_DATA_TYPE_COUNT] = {
    [LLLY_TYPE_DER] = NULL,
    [LLLY_TYPE_BINARY] = &llly_type_binary,
    [LLLY_TYPE_BITS] = &llly_type_bits,
    [LLLY_TYPE_BOOL] = &llly_type_bool,
    [LLLY_TYPE_DEC64] = &llly_type_dec64,
    [LLLY_TYPE_EMPTY] = &llly_type_empty,
    [LLLY_TYPE_ENUM] = &llly_type_enum,
    [LLLY_TYPE_IDENT] = &llly_type_ident,
    [LLLY_TYPE_INST] = &llly_type_inst,
    [LLLY_TYPE_INT8] = &llly_type_int8,
    [LLLY_TYPE_INT16] = &llly_type_int16,
    [LLLY_TYPE_INT32] = &llly_type_int32,
    [LLLY_TYPE_INT64] = &llly_type_int64,
    [LLLY_TYPE_LEAFREF] = &llly_type_leafref,
    [LLLY_TYPE_STRING] = &llly_type_string,
    [LLLY_TYPE_UINT8] = &llly_type_uint8,
    [LLLY_TYPE_UINT16] = &llly_type_uint16,
    [LLLY_TYPE_UINT32] = &llly_type_uint32,
    [LLLY_TYPE_UINT64] = &llly_type_uint64,
    [LLLY_TYPE_UNION] = &llly_type_union
};

