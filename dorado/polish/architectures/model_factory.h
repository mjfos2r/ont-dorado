#pragma once

#include "model_config.h"
#include "model_gru.h"
#include "model_torch_base.h"
#include "model_torch_script.h"

#include <string>

namespace dorado::polisher {

enum class ModelType {
    GRU,
    LATENT_SPACE_GRU,
    LATENT_SPACE_LSTM,
};

ModelType parse_model_type(const std::string& type);

std::shared_ptr<ModelTorchBase> model_factory(const ModelConfig& config);

}  // namespace dorado::polisher