#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdint.h>
#include <assert.h>
#include <limits.h>

int c_get_value(void) {
    double x = fabs(-3.0) + ceil(2.3) + floor(3.7) + pow(2, 3) - 13;
    return (int)x + 96;  // 100
}

const char* c_get_string(void) {
    static char buf[64];
    char src[] = "test123";
    char* p = strstr(src, "123");
    snprintf(buf, sizeof(buf), "%s%s", src, p ? "abc" : "");
    for(int i = 0; buf[i]; i++) buf[i] = toupper(buf[i]);
    return buf;
}

void c_print_summary(int sum, int val, const char* str) {
     uint64_t big = UINT64_MAX;
    int* arr = (int*)calloc(5, sizeof(int));
    for(int i = 0; i < 5; i++) arr[i] = i * 2;
    assert(arr[0] == 0 && arr[4] == 8);
    
    printf("[C] sum=%d val=%d str=%s big=%llu arr=", 
           sum, val, str, (unsigned long long)big);
    for(int i = 0; i < 5; i++) printf("%d ", arr[i]);
    printf("\n");
    free(arr);
}
