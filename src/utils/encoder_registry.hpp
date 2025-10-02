//
// Created by Giuseppe Francione on 01/10/25.
//

#ifndef MONOLITH_ENCODER_REGISTRY_HPP
#define MONOLITH_ENCODER_REGISTRY_HPP

#include "../encoder/encoder.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using Factory = std::function<std::unique_ptr<IEncoder>()>;
using EncoderRegistry = std::unordered_map<std::string, std::vector<Factory>>;

// build and return the registry of encoders
EncoderRegistry build_encoder_registry(bool preserve_metadata);

#endif //MONOLITH_ENCODER_REGISTRY_HPP