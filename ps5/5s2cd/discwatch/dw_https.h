#pragma once
#include <stdio.h>

/* GET https://host/path into out (extra_header: one "Name: value" line or NULL); 0 when a response came (its code in *status), -1 when none did (logged). */
int dw_https_get(const char* host, const char* path, const char* extra_header, FILE* out, int* status);
