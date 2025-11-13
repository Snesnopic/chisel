//
// Created by Giuseppe Francione on 07/10/25.
//

#ifndef CHISEL_RANDOM_UTILS_H
#define CHISEL_RANDOM_UTILS_H

#include <random>
#include <string>

/**
 * @brief Provides simple, thread-local random number utilities.
 *
 * This namespace contains helper functions for generating random numbers
 * and random string suffixes, typically used for creating unique
 * temporary file or directory names. The underlying generator
 * (std::mt19937_64) is thread-local for safety.
 */
namespace RandomUtils {

    /**
     * @brief Generates a random 64-bit unsigned integer.
     * @return A random unsigned long long.
     */
    unsigned long long next_u64();

    /**
     * @brief Generates a random string suffix.
     *
     * This is typically appended to filenames to ensure uniqueness.
     * @return A string representation of a random 64-bit integer.
     */
    std::string random_suffix();

} // namespace

#endif //CHISEL_RANDOM_UTILS_H