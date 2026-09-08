#pragma once
// Nur fuer Host-Tests unter Windows; die Firmware benutzt ESP-IDF/POSIX.
#ifdef _WIN32
#include <time.h>
#include <stdlib.h>
static inline struct tm *localtime_r(const time_t *t, struct tm *out) {
    return localtime_s(out, t) == 0 ? out : NULL;
}
static inline int setenv(const char *key, const char *value, int overwrite) {
    if (!overwrite && getenv(key)) return 0;
    return _putenv_s(key, value);
}
#endif
