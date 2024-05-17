#include "infer.h"

#include "utils/memory_utils.h"
#ifdef __APPLE__
#include "utils/metal_utils.h"
#endif
#ifdef DORADO_CUDA_BUILD
#include "utils/cuda_utils.h"
#endif
#include "utils/string_utils.h"

#include <torch/types.h>

namespace dorado::correction {

int calculate_batch_size(const std::string& device, float memory_fraction) {
    const float model_mem = 2.5f;
    const float per_sample_mem = 0.9f;
    float usable_memory = 0.f;
    if (device == "cpu") {
        size_t free_ram_GB = utils::available_host_memory_GB() * memory_fraction;
        usable_memory = free_ram_GB * memory_fraction;
    }
#ifdef __APPLE__
    else if (device == "mps") {
        size_t physical_memory = get_apple_physical_memory_bytes() / dorado::utils::BYTES_PER_GB;
        usable_memory = physical_memory * memory_fraction;
    }
#endif
#ifdef DORADO_CUDA_BUILD
    else if (utils::starts_with(device, "cuda")) {
        torch::Device dev = torch::Device(device);
        int64_t available = utils::available_memory(dev) / dorado::utils::BYTES_PER_GB;
        usable_memory = available * memory_fraction;
    }
#endif
    else {
        throw std::runtime_error("Unsupported device: " + device);
    }

    usable_memory -= model_mem;
    if (usable_memory <= 0.f) {
        return 0;
    } else {
        int batch_size = std::round(usable_memory / per_sample_mem);
        // Round to nearest multiple of 4.
        batch_size = static_cast<int>(batch_size / 4) * 4;
        return batch_size;
    }
}

}  // namespace dorado::correction
