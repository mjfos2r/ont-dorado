#pragma once

#include <map>
#include <optional>
#include <string>
#include <tuple>

namespace dorado::basecall {

class CudaChunkBenchmarks final {
private:
    CudaChunkBenchmarks();
    using ChunkTimings = std::map<int, float>;
    using ModelName = std::string;
    using GPUName = std::string;
    using ChunkSize = int;
    std::map<std::tuple<GPUName, ModelName, ChunkSize>, ChunkTimings> m_chunk_benchmarks;

public:
    static const CudaChunkBenchmarks& instance() {
        static CudaChunkBenchmarks chunk_benchmarks;
        return chunk_benchmarks;
    }

    std::optional<const ChunkTimings> get_chunk_timings(GPUName gpu_name,
                                                        ModelName model_name,
                                                        ChunkSize chunk_size) const;
};

}  // namespace dorado::basecall
