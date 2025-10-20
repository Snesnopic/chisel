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

    class ProcessorRegistry {
    public:
        ProcessorRegistry();

        // restituisce il processor che gestisce un certo MIME
        [[nodiscard]] std::optional<IProcessor*> find_by_mime(const std::string& mime) const;

        // restituisce il processor che gestisce una certa estensione
        [[nodiscard]] std::optional<IProcessor*> find_by_extension(const std::string& ext) const;

        // restituisce tutti i processor registrati
        [[nodiscard]] const std::vector<std::unique_ptr<IProcessor>>& all() const { return processors_; }

    private:
        std::vector<std::unique_ptr<IProcessor>> processors_;
    };

} // namespace chisel

#endif // CHISEL_PROCESSOR_REGISTRY_HPP