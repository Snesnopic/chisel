//
// Created by Giuseppe Francione on 07/10/25.
//

#ifndef CHISEL_RANDOM_UTILS_H
#define CHISEL_RANDOM_UTILS_H

#include <random>
#include <string>

namespace RandomUtils {
    unsigned long long next_u64();

    std::string random_suffix();
}

#endif //CHISEL_RANDOM_UTILS_H