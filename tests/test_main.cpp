// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Test runner. The TEST_CASE blocks in the other files register themselves;
// the exit code is the number of failing cases (0 = all green).

#include "test_framework.h"

#include "DuplexVersion.h"

int main()
{
    std::printf("AJA-Duplex-ASIO %s unit tests\n\n", AJADUPLEX_VERSION_STRING);
    return ::ajaduplex::testing::Registry::instance().runAll();
}
