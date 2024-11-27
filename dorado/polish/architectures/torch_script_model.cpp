#include "torch_script_model.h"

#include <spdlog/spdlog.h>
#include <torch/script.h>

#include <stdexcept>

namespace dorado::polisher {

TorchScriptModel::TorchScriptModel(const std::filesystem::path& model_path) {
    try {
        spdlog::debug("Loading model from file: {}", model_path.string());
        m_module = torch::jit::load(model_path.string());
    } catch (const c10::Error& e) {
        throw std::runtime_error("Error loading model from " + model_path.string() +
                                 " with error: " + e.what());
    }
}

torch::Tensor TorchScriptModel::forward(torch::Tensor x) {
    return m_module.forward({std::move(x)}).toTensor();
}

void TorchScriptModel::to_half() {
    this->to(torch::kHalf);
    m_module.to(torch::kHalf);
}

void TorchScriptModel::set_eval() {
    this->eval();
    m_module.eval();
}

void TorchScriptModel::to_device(torch::Device device, bool non_blocking) {
    this->to(device, non_blocking);
    m_module.to(device, non_blocking);
}

}  // namespace dorado::polisher
