#include "groups.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "Usage: %s GROUPS.json\n", argv[0]); return 2; }
    struct groups *groups = NULL;
    char error[256];
    if (groups_load(argv[1], &groups, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error); return 1;
    }
    printf("%zu groups, %zu members\n", groups->count, groups->member_count);
    free(groups);
    return 0;
}
