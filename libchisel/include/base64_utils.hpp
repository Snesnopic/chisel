#ifndef CHISEL_BASE64_UTILS_HPP
#define CHISEL_BASE64_UTILS_HPP

#include <string>
#include <vector>
#include <string_view>
#include <cstdint>

namespace chisel {

/**
 * @brief Base64 encode and decode utilities.
 */
class Base64Utils {
public:
    /**
     * @brief decodes a base64 encoded string into raw binary data.
     * @param encoded_string string view containing base64 data.
     * @return vector of decoded bytes.
     */
    static std::vector<uint8_t> decode(std::string_view encoded_string);

    /**
     * @brief encodes raw binary data into a base64 string.
     * @param bytes_to_encode vector of bytes to encode.
     * @return base64 encoded string.
     */
    static std::string encode(const std::vector<uint8_t>& bytes_to_encode);
};

} // namespace chisel

#endif // CHISEL_BASE64_UTILS_HPP
