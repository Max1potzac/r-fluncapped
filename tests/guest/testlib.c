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
int close(int);
long read(int, void*, size_t);
long write(int, const void*, size_t);
int fcntl(int, int, ...);
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

// --- sockets ------------------------------------------------------------------------------

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char zero[8];
};
struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    unsigned ai_addrlen;
    char* ai_canonname;
    void* ai_addr;
    struct addrinfo* ai_next;
};
struct pollfd {
    int fd;
    short events, revents;
};
int socket(int, int, int);
int bind(int, const void*, unsigned);
int listen(int, int);
int accept(int, void*, unsigned*);
int connect(int, const void*, unsigned);
int getsockname(int, void*, unsigned*);
int getsockopt(int, int, int, void*, unsigned*);
long send(int, const void*, size_t, int);
long recv(int, void*, size_t, int);
int poll(struct pollfd*, unsigned long, int);
int getaddrinfo(const char*, const char*, const struct addrinfo*, struct addrinfo**);
void freeaddrinfo(struct addrinfo*);
int inet_pton(int, const char*, void*);
const char* inet_ntop(int, const void*, char*, unsigned);

// The way libcurl connects: a non-blocking connect, poll for writable, check SO_ERROR.
int test_sockets(void) {
    struct addrinfo hints = {0}, *res = 0;
    hints.ai_family = 2;  // AF_INET
    hints.ai_socktype = 1;
    hints.ai_flags = 4;  // AI_NUMERICHOST
    CHECK(1, getaddrinfo("127.0.0.1", "80", &hints, &res) == 0 && res);
    CHECK(2, res->ai_family == 2 && res->ai_addrlen == 16 && ((struct sockaddr_in*)res->ai_addr)->sin_port == 0x5000);
    freeaddrinfo(res);

    struct sockaddr_in a = {0};
    a.sin_family = 2;
    CHECK(3, inet_pton(2, "127.0.0.1", &a.sin_addr) == 1 && a.sin_addr == 0x0100007f);
    char text[16];
    CHECK(4, inet_ntop(2, &a.sin_addr, text, sizeof text) && strcmp(text, "127.0.0.1") == 0);

    int server = socket(2, 1, 0);
    CHECK(5, server >= 0);
    CHECK(6, bind(server, &a, sizeof a) == 0 && listen(server, 1) == 0);
    unsigned len = sizeof a;
    CHECK(7, getsockname(server, &a, &len) == 0 && len == 16 && a.sin_port != 0);

    int client = socket(2, 1 | 04000, 0);  // SOCK_NONBLOCK
    CHECK(8, client >= 0 && (fcntl(client, 3) & 04000));
    int r = connect(client, &a, sizeof a);
    CHECK(9, r == 0 || *__errno() == 115);  // EINPROGRESS
    struct pollfd p = {client, 4, 0};       // POLLOUT
    CHECK(10, poll(&p, 1, 5000) == 1 && (p.revents & 4));
    int err = -1;
    len = sizeof err;
    CHECK(11, getsockopt(client, 1, 4, &err, &len) == 0 && err == 0);  // SOL_SOCKET, SO_ERROR

    int conn = accept(server, 0, 0);
    CHECK(12, conn >= 0);
    CHECK(13, send(client, "ping", 4, 0x4000) == 4);  // MSG_NOSIGNAL
    char buf[8] = {0};
    struct pollfd q = {conn, 1, 0};  // POLLIN
    CHECK(14, poll(&q, 1, 5000) == 1 && read(conn, buf, sizeof buf) == 4 && memcmp(buf, "ping", 4) == 0);
    CHECK(15, write(conn, "pong", 4) == 4);
    struct pollfd w = {client, 1, 0};
    CHECK(16, poll(&w, 1, 5000) == 1 && recv(client, buf, sizeof buf, 0) == 4 && memcmp(buf, "pong", 4) == 0);
    CHECK(17, recv(client, buf, sizeof buf, 0x40) == -1 && *__errno() == 11);  // MSG_DONTWAIT: EAGAIN

    close(conn);
    CHECK(18, poll(&w, 1, 5000) == 1 && recv(client, buf, sizeof buf, 0) == 0);  // peer closed
    close(client);
    close(server);
    CHECK(19, recv(server, buf, 1, 0) == -1 && *__errno() == 9);  // EBADF
    return 0;
}

// rfu --offline: what the game sees without a connection.
int test_offline(void) {
    struct addrinfo* res = 0;
    CHECK(1, socket(2, 1, 0) == -1 && *__errno() == 97);  // EAFNOSUPPORT
    CHECK(2, getaddrinfo("reflourished.ducnguyen.org", "443", 0, &res) == 8 && !res);  // EAI_NONAME
    return 0;
}

// core/hooks.h: the test hooks hook_target, watching it and then replacing it.
int hook_bias = 1;
__attribute__((noinline)) int hook_target(int a) { return a * 3 + hook_bias; }
int call_hook_target(int a) { return hook_target(a) + 1000; }
