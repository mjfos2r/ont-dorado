#include "polish/architectures/counts_feature_encoder.h"

#include "polish/medaka_counts.h"

#include <spdlog/spdlog.h>
#include <utils/timer_high_res.h>

namespace dorado::polisher {

namespace {

CountsResult plp_data_to_tensors(PileupData& data, const size_t n_rows) {
    CountsResult result;

    // Allocate a tensor of the appropriate size directly for `result.counts` on the CPU
    result.counts = torch::empty({static_cast<long>(data.n_cols()), static_cast<long>(n_rows)},
                                 torch::kInt64);

    // Copy the data from `data.matrix()` into `result.counts`
    std::memcpy(result.counts.data_ptr<int64_t>(), data.get_matrix().data(),
                data.n_cols() * n_rows * sizeof(int64_t));

    result.positions_major = std::move(data.get_major());
    result.positions_minor = std::move(data.get_minor());

    return result;
}

/**
 * \brief Function to calculate feature vector normalization groups
 * \param dtypes Vector of data type names (strings).
 * \param num_qstrat Qscore stratifications.
 * \return Lookup of the form: key = (dtype, strand) -> vector of indices
 */
FeatureIndicesType pileup_counts_norm_indices(const std::vector<std::string>& dtypes,
                                              const size_t num_qstrat) {
    // Create a map to store the indices.
    FeatureIndicesType indices;

    const int64_t plp_bases_size = static_cast<int64_t>(std::size(PILEUP_BASES));

    constexpr size_t featlen = std::size(PILEUP_BASES);

    // Iterate over each datatype.
    for (int64_t dti = 0; dti < static_cast<int64_t>(std::size(dtypes)); ++dti) {
        const std::string& dt = dtypes[dti];

        // Iterate over qscore stratification layers.
        for (int64_t qindex = 0; qindex < static_cast<int64_t>(num_qstrat); ++qindex) {
            // Iterate over the base codes (e.g., 'a', 'c', 'g', 't', etc.)
            for (int64_t base_i = 0; base_i < plp_bases_size; ++base_i) {
                const char code = PILEUP_BASES[base_i];
                const bool is_rev = std::islower(code);
                const int64_t index = base_i + dti * num_qstrat * featlen + qindex * featlen;
                indices[std::make_pair(dt, is_rev)].push_back(index);
            }
        }
    }

    return indices;
}

Sample counts_to_features(CountsResult& pileup,
                          const int32_t seq_id,
                          const bool sym_indels,
                          const FeatureIndicesType& feature_indices,
                          const NormaliseType normalise_type) {
    // Avoid slow Torch operations as much as possible. The original Medaka code had this implemented
    // on a very high level with lots of redundancy in computation.
    const int64_t num_rows = static_cast<int64_t>(std::size(pileup.positions_major));
    std::vector<int64_t> minor_inds;
    std::vector<int64_t> major_pos_at_minor_inds;
    std::vector<int64_t> major_ind_at_minor_inds;
    minor_inds.reserve(num_rows);
    major_pos_at_minor_inds.reserve(num_rows);
    major_ind_at_minor_inds.reserve(num_rows);
    int64_t last_non_minor_index = -1;
    for (int64_t i = 0; i < num_rows; ++i) {
        if (pileup.positions_minor[i] > 0) {
            minor_inds.emplace_back(i);
            major_pos_at_minor_inds.emplace_back(pileup.positions_major[i]);
            major_ind_at_minor_inds.emplace_back(last_non_minor_index);
        } else {
            last_non_minor_index = i;
        }
    }

    auto depth = pileup.counts.sum(1);

    auto depth_data = depth.data_ptr<int64_t>();
    for (size_t i = 0; i < std::size(minor_inds); ++i) {
        if (major_ind_at_minor_inds[i] != -1) {
            depth_data[minor_inds[i]] = depth_data[major_ind_at_minor_inds[i]];
        }
    }
    const auto depth_unsequezed = depth.unsqueeze(1).to(FeatureTensorType);

    if (sym_indels) {
        const torch::Tensor minor_inds_tensor = torch::tensor(minor_inds, torch::kInt64);
        const torch::Tensor major_ind_at_minor_inds_tensor =
                torch::tensor(major_ind_at_minor_inds, torch::kInt64);

        for (const auto& [key, inds] : feature_indices) {
            // const std::string& data_type = kv.first.first;
            const bool is_rev = key.second;
            const torch::Tensor inds_tensor = torch::tensor(inds, torch::dtype(torch::kInt64));

            const auto dt_depth =
                    pileup.counts.index({torch::indexing::Slice(), inds_tensor}).sum(1);
            // dt_depth.index_put_({minor_inds}, dt_depth.index({major_ind_at_minor_inds}));

            // Define deletion index.
            const int64_t featlen_index = is_rev ? PILEUP_POS_DEL_REV : PILEUP_POS_DEL_FWD;
            const int64_t dtype_size = PILEUP_BASES_SIZE;

            // Find the deletion index
            // std::vector<int64_t> deletion_indices;
            for (const int64_t x : inds) {
                if ((x % dtype_size) == featlen_index) {
                    // deletion_indices.emplace_back(x);
                    pileup.counts.index_put_({minor_inds_tensor, x},
                                             dt_depth.index({major_ind_at_minor_inds_tensor}) -
                                                     dt_depth.index({minor_inds_tensor}));
                }
            }
            // // Ensure we have at least one valid deletion index
            // if (!deletion_indices.empty()) {
            //     del_ind = deletion_indices[0];  // Take the first valid index
            //     // Update counts for minor indices based on the calculated depths
            //     counts.index_put_({minor_inds, del_ind}, dt_depth.index({major_ind_at_minor_inds}) - dt_depth.index({minor_inds}));
            // } else {
            //     // Handle the case where no deletion index is found (optional)
            //     // e.g., log a warning or set a default behavior
            // }
        }
    }

    torch::Tensor feature_array;
    if (normalise_type == NormaliseType::TOTAL) {
        feature_array =
                pileup.counts / torch::max(depth_unsequezed, torch::ones_like(depth_unsequezed));

    } else if (normalise_type == NormaliseType::FWD_REV) {
        feature_array = torch::empty_like(pileup.counts, FeatureTensorType);
        const torch::Tensor minor_inds_tensor = torch::tensor(minor_inds, torch::kInt64);
        const torch::Tensor major_ind_at_minor_inds_tensor =
                torch::tensor(major_ind_at_minor_inds, torch::kInt64);
        for (const auto& kv : feature_indices) {
            const std::vector<int64_t>& inds = kv.second;
            const torch::Tensor inds_tensor = torch::tensor(inds, torch::dtype(torch::kInt64));

            auto dt_depth = pileup.counts.index({torch::indexing::Slice(), inds_tensor}).sum(1);
            dt_depth.index_put_({minor_inds_tensor},
                                dt_depth.index({major_ind_at_minor_inds_tensor}));
            feature_array.index_put_(
                    {torch::indexing::Slice(), inds_tensor},
                    pileup.counts.index({torch::indexing::Slice(), inds_tensor}) /
                            torch::max(depth_unsequezed, torch::ones_like(depth_unsequezed)));
        }
    } else {
        feature_array = std::move(pileup.counts);
        feature_array = feature_array.to(FeatureTensorType);
    }

    Sample sample{std::move(feature_array), std::move(pileup.positions_major),
                  std::move(pileup.positions_minor), std::move(depth), seq_id};

    return sample;
}

}  // namespace

CountsFeatureEncoder::CountsFeatureEncoder(const int32_t min_mapq) : m_min_mapq{min_mapq} {}

CountsFeatureEncoder::CountsFeatureEncoder(const NormaliseType normalise_type,
                                           const std::vector<std::string>& dtypes,
                                           const std::string_view tag_name,
                                           const int32_t tag_value,
                                           const bool tag_keep_missing,
                                           const std::string_view read_group,
                                           const int32_t min_mapq,
                                           const bool symmetric_indels)
        : m_normalise_type{normalise_type},
          m_dtypes{dtypes},
          m_tag_name{tag_name},
          m_tag_value{tag_value},
          m_tag_keep_missing{tag_keep_missing},
          m_read_group{read_group},
          m_min_mapq{min_mapq},
          m_symmetric_indels{symmetric_indels},
          m_feature_indices{pileup_counts_norm_indices(dtypes, 1)} {}

Sample CountsFeatureEncoder::encode_region(BamFile& bam_file,
                                           const std::string& ref_name,
                                           const int64_t ref_start,
                                           const int64_t ref_end,
                                           const int32_t seq_id) const {
    constexpr size_t num_qstrat = 1;
    constexpr bool weibull_summation = false;

    const int32_t num_dtypes = static_cast<int32_t>(std::size(m_dtypes)) + 1;
    const char* read_group_ptr = std::empty(m_read_group) ? nullptr : m_read_group.c_str();

    // Compute the pileup.
    // NOTE: the `num_qstrat` is passed into the `num_homop` parameter as is done in `pileup_counts` in features.py.
    PileupData pileup = calculate_pileup(
            bam_file, ref_name, ref_start, ref_end, num_dtypes, m_dtypes, num_qstrat, m_tag_name,
            m_tag_value, m_tag_keep_missing, weibull_summation, read_group_ptr, m_min_mapq);

    // Create Torch tensors from the pileup.
    const size_t n_rows = std::size(PILEUP_BASES) * num_dtypes * num_qstrat;
    CountsResult pileup_tensors = plp_data_to_tensors(pileup, n_rows);

    if (!pileup_tensors.counts.numel()) {
        const std::string region =
                ref_name + ':' + std::to_string(ref_start + 1) + '-' + std::to_string(ref_end);
        spdlog::warn("Pileup-feature is zero-length for {} indicating no reads in this region.",
                     region);
        return {};
    }

    return counts_to_features(pileup_tensors, seq_id, m_symmetric_indels, m_feature_indices,
                              m_normalise_type);
}

CountsFeatureDecoder::CountsFeatureDecoder(const LabelSchemeType label_scheme_type)
        : m_label_scheme_type{label_scheme_type} {
    if (label_scheme_type == LabelSchemeType::HAPLOID) {
        m_label_scheme = "*ACGT";
    } else {
        throw std::runtime_error("Unsupported label scheme type!");
    }
}

std::vector<ConsensusResult> CountsFeatureDecoder::decode_bases(const torch::Tensor& logits) const {
    const auto indices = logits.argmax(-1);  // Shape becomes [N, L]

    std::vector<ConsensusResult> results(indices.size(0));

    for (int64_t sample_id = 0; sample_id < indices.size(0); ++sample_id) {
        const auto& positions = indices[sample_id];

        std::string& seq = results[sample_id].seq;
        seq.resize(positions.size(0), '*');

        for (int64_t j = 0; j < positions.size(0); ++j) {
            const int64_t class_index = positions[j].item<int64_t>();
            assert(class_index < static_cast<int64_t>(std::size(m_label_scheme)));
            seq[j] = m_label_scheme[class_index];
        }
    }

    const torch::Tensor probs = torch::gather(logits, -1, indices.unsqueeze(-1)).squeeze(-1);

    // std::cerr << "probs: " << probs << "\n";

    for (int64_t sample_id = 0; sample_id < indices.size(0); ++sample_id) {
        std::string& quals = results[sample_id].quals;
        quals.clear();

        const auto phred_scores =
                (-10.0 * torch::log10(1.0 - probs[sample_id])).clamp(0, 40).to(torch::kUInt8) + 33;

        quals.resize(phred_scores.size(0), '!');
        for (int64_t j = 0; j < phred_scores.size(0); ++j) {
            quals[j] = static_cast<char>(phred_scores[j].item<uint8_t>());
        }
    }

    return results;
}

}  // namespace dorado::polisher
