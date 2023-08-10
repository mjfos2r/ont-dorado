#pragma once

#include "Decoder.h"

#include <torch/torch.h>

namespace dorado {

class GPUDecoder : Decoder {
public:
    GPUDecoder() { std::cerr << "create new decoder" << std::endl; }
    ~GPUDecoder() { std::cerr << "destry decoder" << std::endl; }
    std::vector<DecodedChunk> beam_search(const torch::Tensor& scores,
                                          int num_chunks,
                                          const DecoderOptions& options) final;
    constexpr static torch::ScalarType dtype = torch::kF16;

    // We split beam_search into two parts, the first one running on the GPU and the second
    // one on the CPU. While the second part is running we can submit more commands to the GPU
    // on another thread.
    torch::Tensor gpu_part(torch::Tensor scores, int num_chunks, DecoderOptions options);
    std::vector<DecodedChunk> cpu_part(torch::Tensor moves_sequence_qstring_cpu);
};

}  // namespace dorado
