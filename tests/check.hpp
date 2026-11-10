// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal test harness shared by the test files.
#pragma once

extern int failures;   // number of failed CHECKs (defined in test_main.cpp)

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::println(std::cerr, "FAIL {}:{}: {}", __FILE__, __LINE__, #cond); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)
