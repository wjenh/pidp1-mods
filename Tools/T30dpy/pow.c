#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define APPLYGAMMA(alpha, gamma) (int)(powf((float)(alpha) / 255.0f, gamma) * 255.0f)

int
main(int argc, char **argv)
{
int i;
int val, val2;
float j;
float alpha, gamma;

    alpha = atof(argv[1]);
    gamma = (argc > 2)?atof(argv[2]):1.0f;

    printf("Cutoff 5 corrected is %d\n", APPLYGAMMA(5, gamma));

    for( i = 1; i <= 256; ++i )
    {
        val = (int)(powf((float)(i), -alpha) * 255.0f);
        val2 = APPLYGAMMA(val, gamma);
        printf("%d %d\n", val, val2);
    }
}
