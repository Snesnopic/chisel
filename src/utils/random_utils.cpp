//
// Created by Giuseppe Francione on 07/10/25.
//

#include "random_utils.hpp"

namespace {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    thread_local std::uniform_int_distribution<unsigned long long> dist;
}

unsigned long long RandomUtils::next_u64() {
    return dist(rng);
}

std::string RandomUtils::random_suffix() {
    return std::to_string(next_u64());
}