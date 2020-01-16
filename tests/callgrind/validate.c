#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <valgrind/callgrind.h>

#include "tests/config.h"
#include "libyang.h"

int
main(int argc, char **argv)
{
    int i;
    char *path;
    struct llly_ctx *ctx;
    struct lllyd_node *data;

    if (argc < 3) {
        return 1;
    }

    ctx = llly_ctx_new(NULL, 0);
    if (!ctx) {
        return 1;
    }

    for (i = 1; i < argc - 1; ++i) {
        asprintf(&path, "%s/callgrind/files/%s", TESTS_DIR, argv[i]);
        if (!lllys_parse_path(ctx, path, LLLYS_YANG)) {
            free(path);
            llly_ctx_destroy(ctx, NULL);
            return 1;
        }
        free(path);
    }

    asprintf(&path, "%s/callgrind/files/%s", TESTS_DIR, argv[argc - 1]);

    CALLGRIND_START_INSTRUMENTATION;
    data = lllyd_parse_path(ctx, path, LLLYD_XML, LLLYD_OPT_STRICT | LLLYD_OPT_DATA_NO_YANGLIB);
    CALLGRIND_STOP_INSTRUMENTATION;

    free(path);
    if (!data) {
        llly_ctx_destroy(ctx, NULL);
        return 1;
    }

    lllyd_free_withsiblings(data);
    llly_ctx_destroy(ctx, NULL);
    return 0;
}
