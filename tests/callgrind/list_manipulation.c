#include <stdlib.h>
#include <valgrind/callgrind.h>

#include "tests/config.h"
#include "libyang.h"

#define SCHEMA TESTS_DIR "/callgrind/files/lists.yang"
#define DATA1 TESTS_DIR "/callgrind/files/lists.xml"
#define DATA2 TESTS_DIR "/callgrind/files/lists2.xml"

int
main(void)
{
    int ret = 0;
    struct llly_ctx *ctx = NULL;
    struct lllyd_node *data1 = NULL, *data2 = NULL, *node;
    struct lllyd_difflist *diff = NULL;

    ctx = llly_ctx_new(NULL, 0);
    if (!ctx) {
        ret = 1;
        goto finish;
    }

    if (!lllys_parse_path(ctx, SCHEMA, LLLYS_YANG)) {
        ret = 1;
        goto finish;
    }

    data1 = lllyd_parse_path(ctx, DATA1, LLLYD_XML, LLLYD_OPT_STRICT | LLLYD_OPT_DATA_NO_YANGLIB);
    if (!data1) {
        ret = 1;
        goto finish;
    }

    data2 = lllyd_parse_path(ctx, DATA2, LLLYD_XML, LLLYD_OPT_STRICT | LLLYD_OPT_DATA_NO_YANGLIB);
    if (!data2) {
        ret = 1;
        goto finish;
    }

    CALLGRIND_START_INSTRUMENTATION;
    diff = lllyd_diff(data1, data2, 0);
    if (!diff) {
        ret = 1;
        goto finish;
    }

    if (lllyd_merge(data1, data2, LLLYD_OPT_DESTRUCT)) {
        ret = 1;
        goto finish;
    }
    data2 = NULL;

    node = data1->child->prev->prev->prev->prev->prev->prev->prev;
    lllyd_unlink(node);

    if (lllyd_insert(data1, node)) {
        ret = 1;
        goto finish;
    }

    if (lllyd_validate(&data1, LLLYD_OPT_DATA | LLLYD_OPT_DATA_NO_YANGLIB, NULL)) {
        ret = 1;
        goto finish;
    }
    CALLGRIND_STOP_INSTRUMENTATION;

finish:
    lllyd_free_diff(diff);
    lllyd_free_withsiblings(data1);
    lllyd_free_withsiblings(data2);
    llly_ctx_destroy(ctx, NULL);
    return ret;
}
