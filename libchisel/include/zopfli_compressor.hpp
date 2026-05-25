//
// Created by Giuseppe Francione on 24/02/26.
//

/**
 * @file zopfli_compressor.hpp
 * @brief Compression utility for ZOPFLI_COMPRESSOR.
 */

#ifndef CHISEL_ZOPFLI_COMPRESSOR_HPP
#define CHISEL_ZOPFLI_COMPRESSOR_HPP

#include <vector>
#include <span>
#include <cstdint>

namespace chisel {

    /**
     * @brief Specifies the output format for Zopfli compression.
     */
    enum class ZopfliFormat {
        /**
         * @brief Zlib format (RFC 1950).
         * Includes a zlib header, DEFLATE stream, and Adler32 checksum.
         * Used by PNG (IDAT chunks) and PDF (/FlateDecode streams).
         */
        ZLIB,

        /**
         * @brief Raw DEFLATE format (RFC 1951).
         * Contains only the compressed data stream with no headers or footers.
         * Used by ZIP archives (including OOXML like .docx, .xlsx) and inside Gzip.
         */
        DEFLATE,

        /**
         * @brief Gzip format (RFC 1952).
         * Includes a gzip header, DEFLATE stream, and CRC32 checksum footer.
         * Used by standalone .gz files.
         */
        GZIP
    };

    /**
     * @brief Static compression helper using the Zopfli algorithm.
     *
     * @details This class encapsulates calls to the Zopfli library, providing
     * a unified interface for compressing raw byte buffers into Zlib, Deflate,
     * or Gzip formats. It is stateless and thread-safe.
     */
    class ZopfliCompressor {
    public:
        /**
         * @brief Compresses a raw buffer using the Zopfli algorithm.
         *
         * @param input The raw input data to be compressed.
         * @param iterations The number of Zopfli iterations. Higher values result
         * in better compression but significantly slower performance.
         * Standard values range from 15 (default) to 50+.
         * @param format The desired output container format (ZLIB, DEFLATE, or GZIP).
         * @return A vector containing the compressed data.
         */
        static std::vector<unsigned char> compress(
            std::span<const unsigned char> input,
            unsigned iterations,
            ZopfliFormat format = ZopfliFormat::ZLIB
        );
    };

} // namespace chisel

#endif // CHISEL_ZOPFLI_COMPRESSOR_HPP