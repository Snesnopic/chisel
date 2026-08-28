//
// Created by Giuseppe Francione on 26/08/26.
//

// the one translation unit providing stb_image / stb_image_write's actual
// implementation for the whole library -- every other file only includes the
// headers for their public (STBIDEF) declarations. gif_animations_equal()
// lives here too, since it needs stb's internal streaming GIF decoder
// (stbi__gif_load_next and friends), which has file-local (static) linkage
// and is therefore only callable from this same translation unit.

#include "gif_animation_compare.hpp"

// undef right after use: in a Unity build this file is concatenated with
// others into one translation unit, and these macros otherwise stay defined
// for the rest of it -- reactivating the implementation section (which,
// unlike the declarations, isn't guarded against re-inclusion) the next time
// any other file in the same batch plainly #includes the same header
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#undef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#undef STB_IMAGE_WRITE_IMPLEMENTATION

#include <cstring>
#include <vector>

namespace chisel {

namespace {

// one GIF, yielded a moment (a distinct canvas plus the summed delay of
// every consecutive frame that rendered it) at a time. Decoding one canvas
// at a time instead of stb_image's usual all-frames-in-one-buffer approach
// is what keeps memory bounded on very large animations.
class GifMoments {
public:
    explicit GifMoments(const std::span<const unsigned char> bytes) {
        if (bytes.empty()) return;
        stbi__start_mem(&s_, bytes.data(), static_cast<int>(bytes.size()));
        ok_ = stbi__gif_test(&s_) != 0;
        std::memset(&g_, 0, sizeof(g_));
    }
    ~GifMoments() {
        STBI_FREE(g_.out);
        STBI_FREE(g_.history);
        STBI_FREE(g_.background);
    }
    GifMoments(const GifMoments&) = delete;
    GifMoments& operator=(const GifMoments&) = delete;

    [[nodiscard]] bool ok() const { return ok_ && !failed_; }
    [[nodiscard]] int width() const { return g_.w; }
    [[nodiscard]] int height() const { return g_.h; }
    [[nodiscard]] bool decode_failed() const { return failed_; }

    // the next distinct canvas, carrying the summed delay of every frame
    // that rendered it. Returns false once the animation is exhausted.
    bool next(std::vector<unsigned char>& canvas, long long& delay) {
        if (!held_ && !pull()) return false;
        canvas.swap(cur_);
        delay = cur_delay_;
        held_ = false;
        while (pull()) {
            if (cur_.size() != canvas.size() ||
                std::memcmp(cur_.data(), canvas.data(), canvas.size()) != 0) {
                return true; // differs: kept in cur_ for the next call
            }
            delay += cur_delay_;
            held_ = false;
        }
        return true;
    }

private:
    // reads one raw frame into cur_ unless one is already held
    bool pull() {
        if (held_) return true;
        if (done_ || !ok_) return false;
        int comp = 0;
        stbi_uc* two_back = raw_frames_ >= 2 ? back2_.data() : nullptr;
        stbi_uc* u = stbi__gif_load_next(&s_, &g_, &comp, 4, two_back);
        if (u == reinterpret_cast<stbi_uc*>(&s_) || u == nullptr) {
            done_ = true;
            failed_ = (u == nullptr);
            return false;
        }
        const std::size_t stride = static_cast<std::size_t>(g_.w) * g_.h * 4;
        back2_.swap(back1_);
        back1_.assign(u, u + stride);
        cur_.assign(u, u + stride);
        cur_delay_ = g_.delay;
        ++raw_frames_;
        held_ = true;
        return true;
    }

    std::vector<unsigned char> back1_, back2_, cur_;
    stbi__context s_{};
    stbi__gif g_{};
    long long cur_delay_ = 0;
    int raw_frames_ = 0;
    bool ok_ = false, done_ = false, failed_ = false, held_ = false;
};

} // namespace

GifCompareResult gif_animations_equal(const std::span<const unsigned char> a, const std::span<const unsigned char> b) {
    GifMoments ma(a), mb(b);
    if (!ma.ok() || !mb.ok()) {
        return {.equal=false, .reason="not a valid gif"};
    }

    std::vector<unsigned char> ca, cb;
    long long da = 0, db = 0;
    while (true) {
        const bool ha = ma.next(ca, da);
        const bool hb = mb.next(cb, db);
        if (!ha || !hb) {
            if (ha != hb) return {.equal=false, .reason="moment count mismatch"};
            break;
        }
        if (da != db) return {.equal=false, .reason="delay mismatch"};
        if (ca.size() != cb.size() || std::memcmp(ca.data(), cb.data(), ca.size()) != 0) {
            return {.equal=false, .reason="pixel mismatch"};
        }
    }

    // a stream that fails to decode isn't legitimate end-of-animation --
    // never call it equal just because it failed the same way on both sides
    if (ma.decode_failed() || mb.decode_failed()) {
        return {.equal=false, .reason="decode error"};
    }
    if (ma.width() != mb.width() || ma.height() != mb.height()) {
        return {.equal=false, .reason="dimension mismatch"};
    }

    return {.equal=true, .reason=""};
}

} // namespace chisel
