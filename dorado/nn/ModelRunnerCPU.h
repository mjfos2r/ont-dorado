#pragma once

#include <torch/torch.h>
#include "ModelRunner.h"
#include "../decode/CPUDecoder.h"

class ModelRunnerCPU: public ModelRunner {
    public:
        explicit ModelRunnerCPU(const std::string &model, const std::string &device, int chunk_size, int batch_size, DecoderOptions d_options);
        void accept_chunk(int chunk_idx, at::Tensor slice);
        std::vector<DecodedChunk> call_chunks(int num_chunks);
    private:
        std::unique_ptr<CPUDecoder> m_decoder;
};
