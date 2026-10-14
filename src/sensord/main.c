/* sensord <dht|light>: skeleton only. Real sampling arrives with docs/DESIGN.md section 1. */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "dht") != 0 && strcmp(argv[1], "light") != 0)) {
        fprintf(stderr, "usage: %s <dht|light>\n", argc > 0 ? argv[0] : "sensord");
        return 2;
    }
    fprintf(stderr, "sensord %s: not implemented yet\n", argv[1]);
    return 0;
}
