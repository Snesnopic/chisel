//
// Created by Giuseppe Francione on 19/10/25.
//

#ifndef CHISEL_PROCESSOR_REGISTRY_HPP
#define CHISEL_PROCESSOR_REGISTRY_HPP

#include "../processors/processor.hpp"
#include <memory>
#include <vector>
#include <string>
#include <optional>

namespace chisel {

/**
 * @brief Registry of all available processors in chisel.
 *
 * The ProcessorRegistry owns and manages the lifetime of all concrete
 * IProcessor implementations. It provides lookup facilities to find
 * processors that can handle a given MIME type or file extension.
 *
 * The registry is typically instantiated once per execution and passed
 * to a ProcessorExecutor. It ensures that all processors are available
 * for discovery without requiring the caller to know about specific
 * formats.
 */
class ProcessorRegistry {
public:
    /**
     * @brief Construct and register all built-in processors.
     *
     * The constructor instantiates all known IProcessor implementations
     * (e.g. PNG, JPEG, FLAC) and stores them internally.
     */
    ProcessorRegistry();

    /**
     * @brief Find all processors that support a given MIME type.
     * @param mime MIME type string (e.g. "image/png").
     * @return A vector of pointers to processors that can handle this MIME.
     */
    [[nodiscard]] std::vector<IProcessor*> find_by_mime(const std::string& mime) const;

    /**
     * @brief Find all processors that support a given file extension.
     * @param ext File extension (including the dot, e.g. ".png").
     * @return A vector of pointers to processors that can handle this extension.
     */
    [[nodiscard]] std::vector<IProcessor*> find_by_extension(const std::string& ext) const;

    /**
     * @brief Access all registered processors.
     * @return Constant reference to the internal vector of unique_ptr<IProcessor>.
     */
    [[nodiscard]] const std::vector<std::unique_ptr<IProcessor>>& all() const { return processors_; }

private:
    std::vector<std::unique_ptr<IProcessor>> processors_; ///< Owned processor instances
};

} // namespace chisel

#endif // CHISEL_PROCESSOR_REGISTRY_HPP