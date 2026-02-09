#include <stdio.h>
#include <stdlib.h>

char *decodeInstr(int, char*);

int
main()
{
char strbuf[128];

    decodeInstr(0401234, strbuf);   // add
    printf("%s\n", strbuf);
    decodeInstr(0411234, strbuf);   // add i
    printf("%s\n", strbuf);
    decodeInstr(0765000, strbuf);   // cli|cma
    printf("%s\n", strbuf);
    decodeInstr(0141234, strbuf);   // illegal
    printf("%s\n", strbuf);
    decodeInstr(0640100, strbuf);   // sza
    printf("%s\n", strbuf);
    decodeInstr(0673111, strbuf); // rcr
    printf("%s\n", strbuf);
    decodeInstr(0720007, strbuf);   // dpy-i
    printf("%s\n", strbuf);
    exit(0);
}
