#ifndef __BYTEFS_UTILS_H_
#define __BYTEFS_UTILS_H_

#include <iostream>
#include <cstring>

#include <assert.h>

#define RED   "\x1B[31m"
#define GRN   "\x1B[32m"
#define YEL   "\x1B[33m"
#define BLU   "\x1B[34m"
#define MAG   "\x1B[35m"
#define CYN   "\x1B[36m"
#define WHT   "\x1B[37m"
#define ENDC  "\x1B[0m"

int opencxd_start_threads(void);
int opencxd_stop_threads(void);
void opencxd_stop_threads_gracefully(void);

#define bytefs_err(fmt, ...)                                                    \
    do {                                                                        \
        printf(RED "[BYTEFS Error]: " fmt ENDC "\n", ## __VA_ARGS__);           \
    } while (0)

#define bytefs_warn(fmt, ...)                                                   \
    do {                                                                        \
        printf(YEL "[BYTEFS Warn]:  " fmt ENDC "\n", ## __VA_ARGS__);           \
    } while (0)

#define bytefs_log(fmt, ...)                                                    \
    do {                                                                        \
        printf("[BYTEFS Log]:   " fmt "\n", ## __VA_ARGS__);                    \
    } while (0)

#define bytefs_expect(cond)                                                     \
    do {                                                                        \
        if (!(cond))                                                            \
            printf(RED "[BYTEFS Exception]: %s:%d (%s) %s" ENDC "\n",           \
                __FILE__, __LINE__, __func__, # cond);                          \
    } while (0)


#define bytefs_expect_msg(cond, fmt, ...)                                       \
    do {                                                                        \
        if (!(cond))                                                            \
            printf(RED "[BYTEFS Exception]: %s:%d (%s) " fmt ENDC "\n",         \
                __FILE__, __LINE__, __func__, ## __VA_ARGS__);                  \
    } while (0)

#define bytefs_assert(cond)                                                     \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf(RED "[!!! BYTEFS Assertion]: %s:%d (%s) %s" ENDC "\n",       \
                __FILE__, __LINE__, __func__, # cond);                          \
            assert(false);                                                      \
        }                                                                       \
    } while (0)                          

#define bytefs_assert_msg(cond, fmt, ...)                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf(RED "[!!! BYTEFS Assertion]: %s:%d (%s) " fmt ENDC "\n",     \
                __FILE__, __LINE__, __func__, ## __VA_ARGS__);                  \
            assert(false);                                                      \
        }                                                                       \
    } while (0)

#endif
