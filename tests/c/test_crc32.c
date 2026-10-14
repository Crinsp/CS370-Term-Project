#include <stdio.h>
#include <string.h>

#include "crc32.h"

static int failures;

#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                               \
        }                                                             \
    } while (0)

int main(void)
{
    const char *s = "123456789";

    CHECK(crc32_of(s, strlen(s)) == 0xCBF43926u); /* standard check value */
    CHECK(crc32_of("", 0) == 0u);

    /* incremental update equals one-shot */
    uint32_t c = crc32_update(0, s, 4);
    c = crc32_update(c, s + 4, 5);
    CHECK(c == 0xCBF43926u);

    if (failures == 0)
        puts("test_crc32: ok");
    return failures ? 1 : 0;
}
