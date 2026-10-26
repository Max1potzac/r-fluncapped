// A small Android arm64 library that exercises the runtime: the test runner loads it like
// the game's libraries and calls each test_* function, which returns 0 on success or the
// number of the check that failed.
//
// Built freestanding (no NDK needed): the libc declarations below are the bionic ABI.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef long jmp_buf[32];
typedef struct { int32_t private_[10]; } pthread_mutex_t;
typedef int pthread_once_t;
typedef long pthread_t;
typedef struct FILE FILE;

void* malloc(size_t);
void* realloc(void*, size_t);
void free(void*);
void* memcpy(void*, const void*, size_t);
void* memset(void*, int, size_t);
size_t strlen(const char*);
int strcmp(const char*, const char*);
int memcmp(const void*, const void*, size_t);
char* strstr(const char*, const char*);
int snprintf(char*, size_t, const char*, ...);
int vsnprintf(char*, size_t, const char*, va_list);
int sscanf(const char*, const char*, ...);
double strtod(const char*, char**);
long double strtold(const char*, char**);
long strtol(const char*, char**, int);
void qsort(void*, size_t, size_t, int (*)(const void*, const void*));
__attribute__((returns_twice)) int setjmp(jmp_buf);
__attribute__((noreturn)) void longjmp(jmp_buf, int);
int pthread_once(pthread_once_t*, void (*)(void));
int pthread_mutex_lock(pthread_mutex_t*);
int pthread_mutex_unlock(pthread_mutex_t*);
int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
int pthread_join(pthread_t, void**);
int* __errno(void);
FILE* fopen(const char*, const char*);
char* fgets(char*, int, FILE*);
int fclose(FILE*);
int open(const char*, int, ...);
int __cxa_atexit(void (*)(void*), void*, void*);
double sin(double);
float sqrtf(float);
extern void* __dso_handle;

#define CHECK(n, cond) \
    do {               \
        if (!(cond)) return n; \
    } while (0)

// --- static initialisation ---------------------------------------------------------------

static int constructed;
static int destroyed;

static void on_exit_handler(void* arg) { destroyed = *(int*)arg; }

__attribute__((constructor)) static void construct(void) {
    constructed = 42;
    static int token = 7;
    __cxa_atexit(on_exit_handler, &token, &__dso_handle);
}

int test_constructor(void) { return constructed == 42 ? 0 : 1; }
int destroyed_value(void) { return destroyed; }

// --- strings and formatting --------------------------------------------------------------

static int format(char* buf, size_t n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int test_printf(void) {
    char buf[128];
    CHECK(1, snprintf(buf, sizeof buf, "%d %s %5.2f %lld %x", -3, "ok", 3.14159, 1234567890123LL, 255) == 28);
    CHECK(2, strcmp(buf, "-3 ok  3.14 1234567890123 ff") == 0);
    // More arguments than registers: the rest go on the stack, doubles in v registers.
    format(buf, sizeof buf, "%d %d %d %d %d %d %d %d %.1f %.1f %s", 1, 2, 3, 4, 5, 6, 7, 8, 0.5, 1.5, "end");
    CHECK(3, strcmp(buf, "1 2 3 4 5 6 7 8 0.5 1.5 end") == 0);
    CHECK(4, snprintf(buf, 4, "%s", "truncated") == 9 && strcmp(buf, "tru") == 0);
    long double ld = 2.5L;
    snprintf(buf, sizeof buf, "%.2Lf", ld);
    CHECK(5, strcmp(buf, "2.50") == 0);
    return 0;
}

int test_scanf(void) {
    int a = 0;
    unsigned x = 0;
    char word[16];
    double d = 0;
    CHECK(1, sscanf("12 ff hello 2.5", "%d %x %15s %lf", &a, &x, word, &d) == 4);
    CHECK(2, a == 12 && x == 255 && strcmp(word, "hello") == 0 && d == 2.5);
    return 0;
}

int test_strto(void) {
    char* end;
    CHECK(1, strtod("1.25xyz", &end) == 1.25 && strcmp(end, "xyz") == 0);
    CHECK(2, strtol("-0x10", 0, 0) == -16);
    // Returned as IEEE binary128 in q0. Compared bitwise: comparing long doubles would need
    // compiler-rt's soft-float helpers, which this freestanding build doesn't link.
    long double ld = strtold("0.75", 0), expect = 0.75L;
    CHECK(3, memcmp(&ld, &expect, sizeof ld) == 0);
    CHECK(4, sin(0.0) == 0.0 && sqrtf(16.0f) == 4.0f);
    return 0;
}

int test_heap(void) {
    char* p = malloc(10);
    memcpy(p, "123456789", 10);
    p = realloc(p, 100000);
    CHECK(1, strcmp(p, "123456789") == 0);
    free(p);
    return 0;
}

// --- callbacks into the guest ------------------------------------------------------------

static int compare(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }

int test_qsort(void) {
    int v[] = {5, 3, 9, 1, 7, 2, 8, 6, 4, 0};
    qsort(v, 10, sizeof(int), compare);
    for (int i = 0; i < 10; ++i) CHECK(1 + i, v[i] == i);
    return 0;
}

static jmp_buf jump;
static __attribute__((noinline)) void jump_back(int v) { longjmp(jump, v); }

int test_setjmp(void) {
    volatile int count = 0;
    int r = setjmp(jump);
    if (r == 0) {
        count = 1;
        jump_back(5);
    }
    CHECK(1, r == 5 && count == 1);
    return 0;
}

static pthread_once_t once;
static int once_calls;
static void once_init(void) { ++once_calls; }

static pthread_mutex_t lock;
static int counter;

static void* worker(void* arg) {
    for (int i = 0; i < 1000; ++i) {
        pthread_mutex_lock(&lock);
        ++counter;
        pthread_mutex_unlock(&lock);
    }
    return (void*)((long)arg * 2);
}

int test_threads(void) {
    pthread_once(&once, once_init);
    pthread_once(&once, once_init);
    CHECK(1, once_calls == 1);
    pthread_t t[4];
    for (long i = 0; i < 4; ++i) CHECK(2, pthread_create(&t[i], 0, worker, (void*)i) == 0);
    for (long i = 0; i < 4; ++i) {
        void* result;
        CHECK(3, pthread_join(t[i], &result) == 0);
        CHECK(4, (long)result == i * 2);
    }
    CHECK(5, counter == 4000);
    return 0;
}

// --- files -------------------------------------------------------------------------------

int test_files(void) {
    // RFLib finds libPVZ2 this way.
    FILE* f = fopen("/proc/self/maps", "r");
    CHECK(1, f != 0);
    char line[256];
    int found = 0;
    while (fgets(line, sizeof line, f))
        if (strstr(line, "libtest.so")) found = 1;
    fclose(f);
    CHECK(2, found);
    CHECK(3, open("/no/such/file", 0) == -1 && *__errno() == 2);
    return 0;
}

// --- stack protector -----------------------------------------------------------------------

static __attribute__((noinline)) int fill(char* buf, int n) {
    memset(buf, 'x', (size_t)n);
    return buf[n - 1];
}

int test_stack_guard(void) {
    // Compiled with -fstack-protector-all: reads the cookie from [tpidr_el0 + 0x28].
    char buf[64];
    return fill(buf, sizeof buf) == 'x' ? 0 : 1;
}
