#include "ModBaseRunner.h"

#include "ModBaseCaller.h"
#include "ModBaseModelConfig.h"
#include "ModbaseScaler.h"
#include "utils/sequence_utils.h"
#include "utils/tensor_utils.h"

#if DORADO_GPU_BUILD && !defined(__APPLE__)
#include <c10/cuda/CUDAGuard.h>
#endif

#include <torch/torch.h>

namespace dorado::modbase {

ModBaseRunner::ModBaseRunner(std::shared_ptr<ModBaseCaller> caller) : m_caller(std::move(caller)) {
    auto opts = at::TensorOptions()
                        .device(torch::kCPU)
                        .pinned_memory(m_caller->m_options.device().is_cuda())
                        .dtype(m_caller->m_options.dtype());

    auto seq_input_options = at::TensorOptions()
                                     .device(torch::kCPU)
                                     .pinned_memory(m_caller->m_options.device().is_cuda())
                                     .dtype(torch::kInt8);

    for (auto& caller_data : m_caller->m_caller_data) {
        auto sig_len = static_cast<int64_t>(caller_data->params.context_before +
                                            caller_data->params.context_after);
        auto kmer_len = caller_data->params.bases_after + caller_data->params.bases_before + 1;
        m_input_sigs.push_back(torch::empty({caller_data->batch_size, 1, sig_len}, opts));
        m_input_seqs.push_back(torch::empty(
                {caller_data->batch_size, sig_len, utils::BaseInfo::NUM_BASES * kmer_len},
                seq_input_options));
#if DORADO_GPU_BUILD && !defined(__APPLE__)
        if (m_caller->m_options.device().is_cuda()) {
            m_streams.push_back(
                    c10::cuda::getStreamFromPool(false, m_caller->m_options.device().index()));
        } else {
            m_streams.emplace_back();
        }
#endif
    }
}

void ModBaseRunner::accept_chunk(int model_id,
                                 int chunk_idx,
                                 const at::Tensor& signal,
                                 const std::vector<int8_t>& kmers) {
    // As usual, avoid torch indexing because it is glacially slow.
    // GPU base calling uses float16 signals and input tensors.
    // CPU base calling uses float16 signals, float32 input tensors.
    // Both versions take int8 sequence encodings.

    auto& input_sigs = m_input_sigs[model_id];
    auto& input_seqs = m_input_seqs[model_id];
    assert(signal.size(0) == input_sigs.size(2));

    const auto sig_len = signal.size(0);
    dorado::utils::copy_tensor_elems(input_sigs, chunk_idx * sig_len, signal, 0, sig_len);

    const auto kmer_elem_count = input_seqs.size(1) * input_seqs.size(2);
    if (input_seqs.dtype() != torch::kInt8) {
        throw std::runtime_error("Unsupported input dtype");
    }
    using SeqInputType = int8_t;
    SeqInputType* const input_seqs_ptr = input_seqs.data_ptr<SeqInputType>();
    std::memcpy(&input_seqs_ptr[chunk_idx * kmer_elem_count], kmers.data(),
                kmer_elem_count * sizeof(SeqInputType));
}

at::Tensor ModBaseRunner::call_chunks(int model_id, int num_chunks) {
#if DORADO_GPU_BUILD && !defined(__APPLE__)
    c10::cuda::OptionalCUDAStreamGuard guard(m_streams[model_id]);
#endif
    return m_caller->call_chunks(model_id, m_input_sigs[model_id], m_input_seqs[model_id],
                                 num_chunks);
}

at::Tensor ModBaseRunner::scale_signal(size_t caller_id,
                                       at::Tensor signal,
                                       const std::vector<int>& seq_ints,
                                       const std::vector<uint64_t>& seq_to_sig_map) const {
    auto& scaler = m_caller->m_caller_data[caller_id]->scaler;
    if (scaler) {
        return scaler->scale_signal(signal, seq_ints, seq_to_sig_map);
    }
    return signal;
}

std::vector<size_t> ModBaseRunner::get_motif_hits(size_t caller_id, const std::string& seq) const {
    return m_caller->m_caller_data[caller_id]->get_motif_hits(seq);
}

const ModBaseModelConfig& ModBaseRunner::caller_params(size_t caller_id) const {
    return m_caller->m_caller_data[caller_id]->params;
}

size_t ModBaseRunner::num_callers() const { return m_caller->m_caller_data.size(); }
void ModBaseRunner::terminate() { m_caller->terminate(); }
void ModBaseRunner::restart() { m_caller->restart(); }

std::string ModBaseRunner::get_name() const {
    std::ostringstream name_stream;
    name_stream << "ModBaseRunner_" << this;
    return name_stream.str();
}

stats::NamedStats ModBaseRunner::sample_stats() const {
    // We don't have direct access to the caller object when the pipeline is set up,
    // so pass through stats here.
    // Each runner will retrieve stats from the caller.
    // Only the last retrieved version will appear, but they should be very similar.
    stats::NamedStats stats = stats::from_obj(*m_caller);
    stats["batches_called"] = double(m_num_batches_called);
    return stats;
}

}  // namespace dorado::modbase
