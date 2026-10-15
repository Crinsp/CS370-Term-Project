#include <stdlib.h>
#include <string.h>

/* Copies a sensor label. */
char *copy_label(const char *s)
{
    char *p = malloc(strlen(s) + 1);
    strcpy(p, s);
    return p;
}
