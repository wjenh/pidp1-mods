/*
 * Read a macro1 xldr.src list file, convert to include file.
 *
*/
#include <stdlib.h>
#include <stdio.h>

int
main(int argc, char **argv)
{
int lno, addr, val;
FILE *fP;
char str[256];
char rest[256];

    if( argc != 2 )
    {
        fprintf(stderr, "Usage: cvtxldr inputfile\n");
        exit(1);
    }

    if( !(fP = fopen(argv[1], "r")) )
    {
        fprintf(stderr, "Can't open file '%s'\n", argv[1]);
        exit(1);
    }

    printf("#ifndef XLDR_H\n");
    printf("#define XLDR_H\n");
    printf("#include <stdint.h>\n");
    printf("\n");
    printf("#define LDR_START_ADDR 07751\n");
    printf("\n");
    printf("uint32_t xloader[] = {\n");

    while( fgets(str, sizeof(str), fP) )
    {
        if( sscanf(str,"%d %o %o%*6c%[^\n]", &lno, &addr, &val, rest) != 4 )
        {
            continue;
        }

        printf("0%06o, // %s\n", val, rest);
    }

    printf("};\n");
    printf("#endif\n");
    exit(0);
}
