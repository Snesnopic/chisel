//
// Created by Giuseppe Francione on 11/06/26.
//

#include "../../include/lua_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cstring>

/**
 * @file lua_processor.cpp
 * @brief Lossless debug-info stripper for Lua 5.1, 5.2, and 5.3 compiled bytecode.
 *
 * Lua bytecode layout (all integers are little-endian in standard builds):
 *
 *   File header (12 bytes for 5.1, 18 bytes for 5.2, variable for 5.3):
 *     4 bytes  magic  \x1BLua
 *     1 byte   version  (0x51 = 5.1, 0x52 = 5.2, 0x53 = 5.3)
 *     ...rest of header (version-specific, we skip over it)
 *
 *   Followed by exactly one top-level function prototype.
 *
 * Function prototype layout (Lua 5.1):
 *   string  source name
 *   int32   line defined
 *   int32   last line defined
 *   uint8   num upvalues
 *   uint8   num params
 *   uint8   is_vararg
 *   uint8   max stack size
 *   int32   size_code  → size_code * 4 bytes of opcodes
 *   int32   size_const → constants (type-tagged)
 *   int32   size_proto → nested prototypes (recursive)
 *   --- debug section (stripped) ---
 *   int32   size_lineinfo → size_lineinfo * 4 bytes
 *   int32   size_locvars  → per entry: string name + int32 startpc + int32 endpc
 *   int32   size_upvalues → per entry: string name
 *
 * Function prototype layout (Lua 5.2) -- note the source name is NOT first
 * here (unlike 5.1/5.3): it was moved into the debug section for this
 * version only, then moved back out again in 5.3:
 *   int32   line defined
 *   int32   last line defined
 *   uint8   num params
 *   uint8   is_vararg
 *   uint8   max stack size
 *   int32   size_code
 *   int32   size_const → constants (type-tagged); size_proto → nested
 *           prototypes (recursive) is read as part of the same step
 *   int32   size_upval (upvalue descriptors, NOT names)
 *   --- debug section ---
 *   string  source name
 *   int32   size_lineinfo
 *   int32   size_locvars
 *   int32   size_upvalnames
 *
 * Function prototype layout (Lua 5.3): same layout as 5.2, except the source
 * name moves back to being the very first field again (like 5.1), and the
 * upvalue descriptor includes an "instack" byte and an "idx" byte. Also,
 * right after the file header and before the top-level prototype,
 * luaU_undump() reads one extra byte: the main chunk's upvalue count
 * (structural, not debug info -- copied verbatim, not stripped).
 *
 * In every version the "string" type is:
 *   5.1/5.2: size_t length (includes NUL), then `length` bytes (or 0 = nil/empty).
 *            `size_t` here is the COMPILING MACHINE's native width, as declared
 *            in the file header (byte offset 8) -- 8 bytes on any standard
 *            64-bit build, not a portable fixed 4 bytes.
 *   5.3:     uint8 length+1 if < 0xFF (0 = nil), else 0xFF + native-size_t
 *            length+1; then `length` bytes (no NUL). Note the "+1": the
 *            encoded value must be decremented by one to get the real length.
 *
 * Strategy: we parse each prototype recursively, copy everything except the
 * three debug sections (lineinfo, locvars, upvalue names), replacing their
 * size fields with 0.
 */

namespace chisel {

namespace {

// ─── version tags ────────────────────────────────────────────────────────────
constexpr uint8_t kLuaMagic[4] = { 0x1B, 'L', 'u', 'a' };
constexpr uint8_t kVer51 = 0x51;
constexpr uint8_t kVer52 = 0x52;
constexpr uint8_t kVer53 = 0x53;

// ─── reader/writer helpers ────────────────────────────────────────────────────

class ByteReader {
public:
    explicit ByteReader(const std::vector<uint8_t>& buf) : buf_(buf), pos_(0) {}

    uint8_t read_u8() {
        if (pos_ >= buf_.size()) throw std::runtime_error("LuaProcessor: unexpected end of file");
        return buf_[pos_++];
    }

    uint32_t read_u32() {
        if (pos_ + 4 > buf_.size()) throw std::runtime_error("LuaProcessor: unexpected end of file");
        const uint32_t v = static_cast<uint32_t>(buf_[pos_])
                   | (static_cast<uint32_t>(buf_[pos_+1]) << 8)
                   | (static_cast<uint32_t>(buf_[pos_+2]) << 16)
                   | (static_cast<uint32_t>(buf_[pos_+3]) << 24);
        pos_ += 4;
        return v;
    }

    uint64_t read_u64() {
        if (pos_ + 8 > buf_.size()) throw std::runtime_error("LuaProcessor: unexpected end of file");
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= (static_cast<uint64_t>(buf_[pos_+i]) << (8*i));
        pos_ += 8;
        return v;
    }

    void skip(const std::size_t n) {
        if (pos_ + n > buf_.size()) throw std::runtime_error("LuaProcessor: unexpected end of file");
        pos_ += n;
    }

    /**
     * @brief Bounds-checks and returns a pointer to the next n bytes, advancing past them.
     *
     * Any raw memcpy/insert of a size read from the file (opcode/constant/string
     * bytes) must go through this rather than ptr()+skip() separately, so a
     * corrupted or desynced size field fails cleanly here instead of driving an
     * out-of-bounds read in the caller.
     */
    const uint8_t* take(const std::size_t n) {
        if (pos_ + n > buf_.size()) throw std::runtime_error("LuaProcessor: unexpected end of file");
        const uint8_t* p = buf_.data() + pos_;
        pos_ += n;
        return p;
    }

    [[nodiscard]] const uint8_t* ptr() const { return buf_.data() + pos_; }
    [[nodiscard]] std::size_t remaining() const { return buf_.size() - pos_; }
    [[nodiscard]] std::size_t pos() const { return pos_; }

private:
    const std::vector<uint8_t>& buf_;
    std::size_t pos_;
};

class ByteWriter {
public:
    std::vector<uint8_t> buf;

    void write_u8(const uint8_t v) { buf.push_back(v); }

    void write_u32(const uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v >> 16));
        buf.push_back(static_cast<uint8_t>(v >> 24));
    }

    void write_bytes(const uint8_t* p, const std::size_t n) {
        buf.insert(buf.end(), p, p + n);
    }
};

// ─── Lua 5.1 / 5.2 / 5.3 string reading ─────────────────────────────────────
//
// size_t_width is the file header's declared sizeof(size_t) (4 or 8) for the
// machine that compiled the bytecode -- 8 on any standard 64-bit build. Every
// string length prefix in 5.1/5.2 (and the 5.3 long-string 0xFF case) is that
// many bytes, not a portable fixed width.

uint64_t read_sized(ByteReader& r, const int size_t_width) {
    return size_t_width == 8 ? r.read_u64() : static_cast<uint64_t>(r.read_u32());
}

void write_sized(ByteWriter& w, const uint64_t v, const int size_t_width) {
    if (size_t_width == 8) {
        for (int i = 0; i < 8; ++i) w.write_u8(static_cast<uint8_t>(v >> (8*i)));
    } else {
        w.write_u32(static_cast<uint32_t>(v));
    }
}

/**
 * @brief Read and discard a Lua 5.1/5.2 string (used for stripped debug strings).
 */
void skip_lua_string_51(ByteReader& r, const int size_t_width) {
    const uint64_t len = read_sized(r, size_t_width);
    if (len > 0) r.skip(static_cast<std::size_t>(len));
}

/**
 * @brief Read and discard a Lua 5.3 string (used for stripped debug strings).
 */
void skip_lua_string_53(ByteReader& r, const int size_t_width) {
    uint64_t size = r.read_u8();
    if (size == 0xFF) size = read_sized(r, size_t_width);
    if (size == 0) return; // nil string
    r.skip(static_cast<std::size_t>(size - 1)); // encoded value is length+1
}

/**
 * @brief Read a Lua 5.1/5.2 string and copy it to writer (for non-debug strings).
 */
void copy_lua_string_51(ByteReader& r, ByteWriter& w, const int size_t_width) {
    const uint64_t len = read_sized(r, size_t_width);
    write_sized(w, len, size_t_width);
    if (len > 0) {
        const auto n = static_cast<std::size_t>(len);
        w.write_bytes(r.take(n), n);
    }
}

/**
 * @brief Read a Lua 5.3 string and copy it to writer.
 */
void copy_lua_string_53(ByteReader& r, ByteWriter& w, const int size_t_width) {
    uint64_t size = r.read_u8();
    w.write_u8(static_cast<uint8_t>(size));
    if (size == 0xFF) {
        size = read_sized(r, size_t_width);
        write_sized(w, size, size_t_width);
    }
    if (size == 0) return; // nil string
    const auto n = static_cast<std::size_t>(size - 1); // encoded value is length+1
    w.write_bytes(r.take(n), n);
}

// ─── Lua 5.1 prototype parser ─────────────────────────────────────────────────

void parse_proto_51(ByteReader& r, ByteWriter& w, const int size_t_width) {
    // source name → zero it out (write empty string, still size_t_width bytes)
    skip_lua_string_51(r, size_t_width);
    write_sized(w, 0, size_t_width);

    // line defined, last line defined → zero (portable 4-byte "int" fields)
    r.read_u32(); r.read_u32();
    w.write_u32(0); w.write_u32(0);

    // num_upvalues, num_params, is_vararg, max_stack_size
    for (int i = 0; i < 4; ++i) {
        w.write_u8(r.read_u8());
    }

    // instruction list
    const uint32_t size_code = r.read_u32();
    w.write_u32(size_code);
    w.write_bytes(r.take(static_cast<std::size_t>(size_code) * 4), static_cast<std::size_t>(size_code) * 4);

    // constant list
    const uint32_t size_const = r.read_u32();
    w.write_u32(size_const);
    for (uint32_t i = 0; i < size_const; ++i) {
        const uint8_t tag = r.read_u8();
        w.write_u8(tag);
        switch (tag) {
            case 0: break; // LUA_TNIL
            case 1: w.write_u8(r.read_u8()); break; // LUA_TBOOLEAN
            case 3: // LUA_TNUMBER (double)
                w.write_bytes(r.take(8), 8);
                break;
            case 4: // LUA_TSTRING
                copy_lua_string_51(r, w, size_t_width);
                break;
            default:
                throw std::runtime_error("LuaProcessor 5.1: unknown constant type " + std::to_string(tag));
        }
    }

    // nested prototypes
    const uint32_t size_proto = r.read_u32();
    w.write_u32(size_proto);
    for (uint32_t i = 0; i < size_proto; ++i) {
        parse_proto_51(r, w, size_t_width);
    }

    // --- debug section — strip entirely ---

    // lineinfo: int32 count + count*int32
    const uint32_t size_lineinfo = r.read_u32();
    r.skip(static_cast<std::size_t>(size_lineinfo) * 4);
    w.write_u32(0); // stripped

    // locvars: int32 count + per entry: string + int32 + int32
    const uint32_t size_locvars = r.read_u32();
    for (uint32_t i = 0; i < size_locvars; ++i) {
        skip_lua_string_51(r, size_t_width);
        r.skip(8); // startpc + endpc
    }
    w.write_u32(0); // stripped

    // upvalue names: int32 count + per entry: string
    const uint32_t size_upvals = r.read_u32();
    for (uint32_t i = 0; i < size_upvals; ++i) {
        skip_lua_string_51(r, size_t_width);
    }
    w.write_u32(0); // stripped
}

// ─── Lua 5.2 prototype parser ─────────────────────────────────────────────────

void parse_proto_52(ByteReader& r, ByteWriter& w, const int size_t_width) {
    // NOTE: unlike 5.1/5.3, 5.2's LoadFunction does NOT read a source name
    // first -- that field was moved into LoadDebug (it's the first thing in
    // the debug section instead), a format change specific to this version
    // that was reverted again in 5.3. Line/last-line-defined come first here.

    // line defined, last line defined → zero
    r.read_u32(); r.read_u32();
    w.write_u32(0); w.write_u32(0);

    // num_params, is_vararg, max_stack_size
    for (int i = 0; i < 3; ++i) w.write_u8(r.read_u8());

    // instruction list
    const uint32_t size_code = r.read_u32();
    w.write_u32(size_code);
    w.write_bytes(r.take(static_cast<std::size_t>(size_code) * 4), static_cast<std::size_t>(size_code) * 4);

    // constant list
    const uint32_t size_const = r.read_u32();
    w.write_u32(size_const);
    for (uint32_t i = 0; i < size_const; ++i) {
        const uint8_t tag = r.read_u8();
        w.write_u8(tag);
        switch (tag) {
            case 0: break; // LUA_TNIL
            case 1: w.write_u8(r.read_u8()); break; // LUA_TBOOLEAN
            case 3: w.write_bytes(r.take(8), 8); break; // LUA_TNUMBER (double)
            case 4: // LUA_TSTRING
                copy_lua_string_51(r, w, size_t_width);
                break;
            default:
                throw std::runtime_error("LuaProcessor 5.2: unknown constant type " + std::to_string(tag));
        }
    }

    // nested prototypes (read as part of the same step as constants, per LoadConstants)
    const uint32_t size_proto = r.read_u32();
    w.write_u32(size_proto);
    for (uint32_t i = 0; i < size_proto; ++i) {
        parse_proto_52(r, w, size_t_width);
    }

    // upvalue descriptors (instack + idx, NOT names) -- comes after constants/protos
    const uint32_t size_upval = r.read_u32();
    w.write_u32(size_upval);
    for (uint32_t i = 0; i < size_upval; ++i) {
        w.write_u8(r.read_u8()); // instack
        w.write_u8(r.read_u8()); // idx
    }

    // --- debug section — strip (source name lives here in 5.2) ---
    skip_lua_string_51(r, size_t_width); // source name
    write_sized(w, 0, size_t_width);

    // lineinfo
    const uint32_t size_lineinfo = r.read_u32();
    r.skip(static_cast<std::size_t>(size_lineinfo) * 4);
    w.write_u32(0);

    // locvars
    const uint32_t size_locvars = r.read_u32();
    for (uint32_t i = 0; i < size_locvars; ++i) {
        skip_lua_string_51(r, size_t_width);
        r.skip(8);
    }
    w.write_u32(0);

    // upvalue names
    const uint32_t size_upnames = r.read_u32();
    for (uint32_t i = 0; i < size_upnames; ++i) {
        skip_lua_string_51(r, size_t_width);
    }
    w.write_u32(0);
}

// ─── Lua 5.3 prototype parser ─────────────────────────────────────────────────

void parse_proto_53(ByteReader& r, ByteWriter& w, const int size_t_width) {
    // source name → zero (5.3 uses length-prefixed string without NUL)
    skip_lua_string_53(r, size_t_width);
    w.write_u8(0); // nil string

    // line defined, last line defined → zero
    r.read_u32(); r.read_u32();
    w.write_u32(0); w.write_u32(0);

    // num_params, is_vararg, max_stack_size
    for (int i = 0; i < 3; ++i) w.write_u8(r.read_u8());

    // instruction list
    const uint32_t size_code = r.read_u32();
    w.write_u32(size_code);
    w.write_bytes(r.take(static_cast<std::size_t>(size_code) * 4), static_cast<std::size_t>(size_code) * 4);

    // constant list — 5.3 uses different tags
    const uint32_t size_const = r.read_u32();
    w.write_u32(size_const);
    for (uint32_t i = 0; i < size_const; ++i) {
        const uint8_t tag = r.read_u8();
        w.write_u8(tag);
        switch (tag) {
            case 0:  break; // LUA_TNIL
            case 1:  w.write_u8(r.read_u8()); break; // LUA_TBOOLEAN
            case 3:  w.write_bytes(r.take(8), 8); break; // LUA_TNUMFLT (double)
            case 19: w.write_bytes(r.take(8), 8); break; // LUA_TNUMINT (int64)
            case 4:  // LUA_TSHRSTR
            case 20: // LUA_TLNGSTR
                copy_lua_string_53(r, w, size_t_width);
                break;
            default:
                throw std::runtime_error("LuaProcessor 5.3: unknown constant type " + std::to_string(tag));
        }
    }

    // upvalue descriptors (instack + idx)
    const uint32_t size_upval = r.read_u32();
    w.write_u32(size_upval);
    for (uint32_t i = 0; i < size_upval; ++i) {
        w.write_u8(r.read_u8()); // instack
        w.write_u8(r.read_u8()); // idx
    }

    // nested prototypes
    const uint32_t size_proto = r.read_u32();
    w.write_u32(size_proto);
    for (uint32_t i = 0; i < size_proto; ++i) {
        parse_proto_53(r, w, size_t_width);
    }

    // --- debug section — strip ---
    const uint32_t size_lineinfo = r.read_u32();
    r.skip(static_cast<std::size_t>(size_lineinfo) * 4);
    w.write_u32(0);

    const uint32_t size_locvars = r.read_u32();
    for (uint32_t i = 0; i < size_locvars; ++i) {
        skip_lua_string_53(r, size_t_width);
        r.skip(8); // startpc + endpc
    }
    w.write_u32(0);

    const uint32_t size_upnames = r.read_u32();
    for (uint32_t i = 0; i < size_upnames; ++i) {
        skip_lua_string_53(r, size_t_width);
    }
    w.write_u32(0);
}

// ─── detect if data is Lua bytecode ──────────────────────────────────────────

bool is_lua_bytecode(const std::vector<uint8_t>& data) {
    return data.size() >= 5
        && data[0] == kLuaMagic[0]
        && data[1] == kLuaMagic[1]
        && data[2] == kLuaMagic[2]
        && data[3] == kLuaMagic[3]
        && (data[4] == kVer51 || data[4] == kVer52 || data[4] == kVer53);
}

/**
 * @brief Returns the byte offset past the file header (to the first prototype).
 *
 * Header sizes:
 *   5.1 — 12 bytes: magic(4) + version(1) + format(1) + endian(1) + int_size(1) +
 *                   size_t_size(1) + instr_size(1) + number_size(1) + integral_flag(1)
 *   5.2 — 18 bytes: magic(4) + version(1) + format(1) + endian(1) + int_size(1) +
 *                   size_t_size(1) + instr_size(1) + number_size(1) + integral_flag(1) +
 *                   LUAC_TAIL(6)
 *   5.3 — 33 bytes: magic(4) + version(1) + format(1) + LUAC_DATA(6) + int_size(1) +
 *                   size_t_size(1) + instr_size(1) + integer_size(1) + number_size(1) +
 *                   sample_int(8) + sample_float(8)
 */
size_t header_size(const uint8_t version) {
    switch (version) {
        case kVer51: return 12;
        case kVer52: return 18;
        case kVer53: return 33;
        default:     return 0;
    }
}

/**
 * @brief Returns the declared sizeof(size_t) (4 or 8) of the machine that
 * compiled this bytecode -- string length prefixes use this width, not a
 * fixed one. Byte offset 8 for 5.1/5.2, offset 13 for 5.3 (verified against
 * the reference luaU_header()/checkHeader() implementations).
 */
int read_size_t_width(const std::vector<uint8_t>& data, const uint8_t version) {
    switch (version) {
        case kVer51:
        case kVer52:
            return data.size() > 8 ? data[8] : 4;
        case kVer53:
            return data.size() > 13 ? data[13] : 8;
        default:
            return 4;
    }
}

/**
 * @brief Strip debug info from a loaded Lua bytecode buffer.
 * @return Stripped bytecode, or empty vector if not a recognised Lua file.
 */
std::vector<uint8_t> strip_debug(const std::vector<uint8_t>& in) {
    if (!is_lua_bytecode(in)) return {};

    const uint8_t version = in[4];
    const std::size_t hdr = header_size(version);
    if (hdr == 0 || in.size() < hdr) return {};

    const int size_t_width = read_size_t_width(in, version);
    if (size_t_width != 4 && size_t_width != 8) return {}; // unsupported platform width

    ByteReader r(in);
    ByteWriter w;

    // Copy the file header verbatim
    w.write_bytes(in.data(), hdr);
    r.skip(hdr);

    // 5.3 only: luaU_undump() reads one extra byte right after the header --
    // the top-level closure's upvalue count (LoadByte, before LoadFunction) --
    // that 5.1/5.2 don't have. It's structural, not debug info: copy it verbatim.
    if (version == kVer53) {
        w.write_u8(r.read_u8());
    }

    // Parse & strip the top-level prototype
    switch (version) {
        case kVer51: parse_proto_51(r, w, size_t_width); break;
        case kVer52: parse_proto_52(r, w, size_t_width); break;
        case kVer53: parse_proto_53(r, w, size_t_width); break;
        default:     return {};
    }

    return std::move(w.buf);
}

} // namespace

// ─── IProcessor interface ─────────────────────────────────────────────────────

void LuaProcessor::recompress(const std::filesystem::path& input_path,
                               const std::filesystem::path& output_path,
                               const ProcessingOptions& /*options*/) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input_path.string(), get_name());

    const auto in_data = read_file(input_path);

    if (!is_lua_bytecode(in_data)) {
        // Not compiled bytecode — could be Lua source; leave untouched.
        Logger::log(LogLevel::Debug, "Not a compiled Lua bytecode file, skipping", get_name());
        if (!write_file(output_path, in_data))
            throw std::runtime_error("LuaProcessor: failed to write output");
        return;
    }

    const auto out_data = strip_debug(in_data);
    if (out_data.empty()) {
        Logger::log(LogLevel::Warning, "Lua debug strip returned empty result, keeping original", get_name());
        if (!write_file(output_path, in_data))
            throw std::runtime_error("LuaProcessor: failed to write output");
        return;
    }

    if (!write_file(output_path, out_data))
        throw std::runtime_error("LuaProcessor: failed to write output to " + output_path.string());

    Logger::log(LogLevel::Debug,
        "Lua debug stripped: " + std::to_string(in_data.size()) +
        " → " + std::to_string(out_data.size()) + " bytes",
        get_name());
}

bool LuaProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    // After stripping, both files must produce the same bytecode.
    // We strip both and compare the results, ignoring any leftover debug data.
    try {
        const auto da = read_file(a);
        const auto db = read_file(b);

        if (!is_lua_bytecode(da) || !is_lua_bytecode(db)) {
            // Fall back to byte comparison for non-bytecode files.
            return da == db;
        }

        const auto sa = strip_debug(da);
        const auto sb = strip_debug(db);
        return !sa.empty() && sa == sb;
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, std::string("raw_equal failed: ") + e.what(), get_name());
        return false;
    }
}

} // namespace chisel
