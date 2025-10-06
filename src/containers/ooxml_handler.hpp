// ooxml_handler.hpp
//
// Generic handler for OOXML containers (DOCX, XLSX, PPTX)
//

#pragma once

#include <string>
#include "../containers/archive_handler.hpp"
#include "../utils/archive_formats.hpp"

class OoxmlHandler final :public IContainer {
public:
    explicit OoxmlHandler(const ContainerFormat fmt) : fmt_(fmt) {}

    ContainerJob prepare(const std::string &path) override;

    bool finalize(const ContainerJob &job, Settings &settings) override;

    static std::vector<unsigned char> recompress_with_zopfli(const std::vector<unsigned char> &input);

private:
    ContainerFormat fmt_;
};