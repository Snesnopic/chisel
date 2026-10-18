//
// Created by Giuseppe Francione on 24/02/26.
//

#include "../../include/zopfli_compressor.hpp"
#include "../../include/thread_pool.hpp"
#include "zopfli.h"
#include <cstdlib>
#include "deflate.h"
#include "gzip_container.h"
#include "zlib_container.h"


namespace chisel {


    std::vector<unsigned char> ZopfliCompressor::compress(const std::span<const unsigned char> input,
                                                          const unsigned iterations,
                                                          const ZopfliFormat format) {
        ZopfliOptions opts;
        ZopfliInitOptions(&opts);

        // Set iterations (default 15 if == 0 passed)
        opts.numiterations = (iterations != 0) ? iterations : 15;

        // Default blocksplitting behavior
        opts.blocksplitting = 1;

        // on a pool worker, inputs over zopfli's 1 MB master block share the pool; the output stays the same
        if (ThreadPool* pool = ThreadPool::current(); pool && pool->size() > 1) {
            opts.parallel_for = ThreadPool::parallel_for_c;
            opts.parallel_ctx = pool;
            opts.parallel_width = pool->size();
        }

        unsigned char* out_data = nullptr;
        size_t out_size = 0;
        unsigned char bp = 0;

        switch (format) {
            case ZopfliFormat::ZLIB:
                ZopfliZlibCompress(&opts, input.data(), input.size(), &out_data, &out_size);
                break;
            case ZopfliFormat::DEFLATE:
                ZopfliDeflate(&opts,2,1, input.data(), input.size(), &bp, &out_data, &out_size);
                break;
            case ZopfliFormat::GZIP:
                ZopfliGzipCompress(&opts, input.data(), input.size(), &out_data, &out_size);
                break;
        }

        std::vector<unsigned char> result;
        if (out_data != nullptr) {
            try {
                result.assign(out_data, out_data + out_size);
            } catch (...) {
                free(out_data);
                throw;
            }
            free(out_data);
        }

        return result;
    }

} // namespace chisel