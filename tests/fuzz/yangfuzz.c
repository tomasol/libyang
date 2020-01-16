#include <stdio.h>
#include <stdlib.h>

#include "libyang.h"

int main(int argc, char **argv) {
    struct llly_ctx *ctx = NULL;

    if (argc != 2) {
        fprintf(stderr, "invalid usage\n");
        exit(EXIT_FAILURE);
    }

    ctx = llly_ctx_new(NULL, 0);
    if (!ctx) {
        fprintf(stderr, "failed to create context.\n");
        return 1;
    }

    while (__AFL_LOOP(100)) {
        lllys_parse_path(ctx, argv[1], LLLYS_IN_YANG);

        llly_ctx_clean(ctx, NULL);
    }

    llly_ctx_destroy(ctx, NULL);
}
