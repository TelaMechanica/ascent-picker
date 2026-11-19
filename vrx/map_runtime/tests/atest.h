/* atest.h — minimal test framework. One-line PASS/FAIL per check. */
#ifndef ATEST_H
#define ATEST_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int atest_pass = 0, atest_fail = 0;
static const char *atest_suite = "?";

#define SUITE(n) do{ atest_suite=(n); printf("\n=== %s ===\n", (n)); }while(0)

#define CHECK(cond, fmt, ...) do{                                            \
    if (cond) { atest_pass++; printf("  PASS  " fmt "\n", ##__VA_ARGS__); }  \
    else { atest_fail++;                                                     \
           printf("  FAIL  " fmt "   [%s:%d]\n", ##__VA_ARGS__,              \
                  __FILE__, __LINE__); }                                     \
}while(0)

#define CHECK_EQI(got, want, fmt, ...) do{                                   \
    long long g=(long long)(got), w=(long long)(want);                       \
    if (g==w) { atest_pass++; printf("  PASS  " fmt " = %lld\n", ##__VA_ARGS__, g); } \
    else { atest_fail++; printf("  FAIL  " fmt " = %lld, want %lld   [%s:%d]\n", \
           ##__VA_ARGS__, g, w, __FILE__, __LINE__); }                       \
}while(0)

#define CHECK_NEAR(got, want, tol, fmt, ...) do{                             \
    double g=(double)(got), w=(double)(want), d=g-w; if(d<0)d=-d;            \
    if (d<=(tol)) { atest_pass++; printf("  PASS  " fmt " = %.7f\n", ##__VA_ARGS__, g); } \
    else { atest_fail++; printf("  FAIL  " fmt " = %.7f, want %.7f+-%g  [%s:%d]\n", \
           ##__VA_ARGS__, g, w, (double)(tol), __FILE__, __LINE__); }        \
}while(0)

static int atest_report(void)
{
    printf("\n---- %s: %d passed, %d failed ----\n", atest_suite, atest_pass, atest_fail);
    return atest_fail ? 1 : 0;
}
#endif
