#ifndef UTILS_H
#define UTILS_H

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#define ASSERT_OK(result)                                                                                              \
    if ((result) < 0) {                                                                                                \
        LOG_ERR("Error at %s:%d:%d", __FILE__, __LINE__, result);                                                      \
        return (result);                                                                                               \
    }

// result must stay parenthesized: without it "ASSERT_TRUE(a == b)" expands to "if (!a == b)",
// which silently never fires unless b is 0. Evaluated once so a failing call is not repeated.
#define ASSERT_TRUE(result)                                                                                            \
    do {                                                                                                               \
        if (!(result)) {                                                                                               \
            LOG_ERR("Assertion failed at %s:%d", __FILE__, __LINE__);                                                  \
            return -1;                                                                                                 \
        }                                                                                                              \
    } while (0)

#endif