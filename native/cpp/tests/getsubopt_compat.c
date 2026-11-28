// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>

int archivefs_getsubopt(char **option, char *const *keys, char **value);

int main(void)
{
    char dict[] = "dictsize", level[] = "level";
    char *const keys[] = {dict, level, NULL};
    char options[] = "dictsize=1048576,level=9,extreme,x86,armthumb";
    char *next = options, *value = NULL;
    assert(archivefs_getsubopt(&next, keys, &value) == 0);
    assert(value && strcmp(value, "1048576") == 0);
    assert(archivefs_getsubopt(&next, keys, &value) == 1);
    assert(value && strcmp(value, "9") == 0);
    assert(archivefs_getsubopt(&next, keys, &value) == -1);
    assert(value && strcmp(value, "extreme") == 0);
    assert(archivefs_getsubopt(&next, keys, &value) == -1);
    assert(value && strcmp(value, "x86") == 0);
    assert(archivefs_getsubopt(&next, keys, &value) == -1);
    assert(value && strcmp(value, "armthumb") == 0);
    assert(*next == '\0');
    char missing[] = "level";
    next = missing;
    assert(archivefs_getsubopt(&next, keys, &value) == 1);
    assert(value == NULL);
    puts("PASS Windows getsubopt compatibility: values, unmatched flags and missing values");
    return 0;
}
