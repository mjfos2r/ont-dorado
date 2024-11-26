#pragma once

#include "polish/architectures/base_feature_encoder.h"
#include "polish/bam_file.h"
#include "polish/consensus_result.h"
#include "polish/medaka_bamiter.h"
#include "polish/sample.h"

#include <torch/torch.h>
#include <torch/types.h>

#include <string>
#include <string_view>
#include <vector>

#ifdef NDEBUG
#define LOG_TRACE(...)
#else
#define LOG_TRACE(...) spdlog::trace(__VA_ARGS__)
#endif

namespace dorado::polisher {

struct CountsResult {
    torch::Tensor counts;
    std::vector<int64_t> positions_major;
    std::vector<int64_t> positions_minor;
};

class CountsFeatureEncoder : public BaseFeatureEncoder {
public:
    CountsFeatureEncoder() = default;

    CountsFeatureEncoder(const NormaliseType normalise_type,
                         const std::vector<std::string>& dtypes,
                         const std::string_view tag_name,
                         const int32_t tag_value,
                         const bool tag_keep_missing,
                         const std::string_view read_group,
                         const int32_t min_mapq,
                         const bool symmetric_indels,
                         const LabelSchemeType label_scheme_type);

    ~CountsFeatureEncoder() = default;

    Sample encode_region(BamFile& bam_file,
                         const std::string& ref_name,
                         const int64_t ref_start,
                         const int64_t ref_end,
                         const int32_t seq_id) const override;

    torch::Tensor collate(std::vector<torch::Tensor> batch) const override;

    std::vector<polisher::Sample> merge_adjacent_samples(
            std::vector<Sample> samples) const override;

    std::vector<ConsensusResult> decode_bases(const torch::Tensor& logits) const override;

private:
    NormaliseType m_normalise_type{NormaliseType::TOTAL};
    int32_t m_num_dtypes = 1;
    std::vector<std::string> m_dtypes;
    std::string m_tag_name;
    int32_t m_tag_value = 0;
    bool m_tag_keep_missing = false;
    std::string m_read_group;
    int32_t m_min_mapq = 1;
    bool m_symmetric_indels = false;
    FeatureIndicesType m_feature_indices;
    LabelSchemeType m_label_scheme_type = LabelSchemeType::HAPLOID;
};

}  // namespace dorado::polisher