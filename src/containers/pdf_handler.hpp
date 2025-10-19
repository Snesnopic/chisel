//
// Created by Giuseppe Francione on 16/10/25.
//

#ifndef CHISEL_PDF_HANDLER_HPP
#define CHISEL_PDF_HANDLER_HPP

#include "container.hpp"
#include <filesystem>
#include <unordered_map>
#include <vector>

class PdfHandler final : public IContainer {
public:
    explicit PdfHandler(const ContainerFormat fmt) : fmt_(fmt) {}

    ContainerJob prepare(const std::filesystem::path& path) override;
    bool finalize(const ContainerJob &job, Settings& settings) override;
protected:
    ContainerFormat fmt_;
private:

    struct StreamInfo {
        bool decodable = false;
        bool has_decode_parms = false;
        std::filesystem::path file;
    };

    struct PdfState {
        std::vector<StreamInfo> streams;
        std::filesystem::path temp_dir;
    };

    std::unordered_map<std::filesystem::path, PdfState> state_;

    static std::filesystem::path make_temp_dir_for(const std::filesystem::path& input);

    static void cleanup_temp_dir(const std::filesystem::path& dir);
};

#endif // CHISEL_PDF_HANDLER_HPP