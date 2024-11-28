#include "polish/architectures/feature_decoder.h"

#include <cstdint>

namespace dorado::polisher {

FeatureDecoder::FeatureDecoder(const LabelSchemeType label_scheme_type)
        : m_label_scheme_type(label_scheme_type) {}

std::vector<ConsensusResult> FeatureDecoder::decode_bases(const torch::Tensor& logits) const {
    return decode_bases_impl(m_label_scheme_type, logits);
}

LabelSchemeType parse_label_scheme_type(const std::string& type) {
    if (type == "HaploidLabelScheme") {
        return LabelSchemeType::HAPLOID;
    }
    throw std::runtime_error{"Unknown label scheme type: '" + type + "'!"};
}

std::vector<ConsensusResult> decode_bases_impl(const LabelSchemeType label_scheme_type,
                                               const torch::Tensor& logits) {
    std::string label_scheme;
    if (label_scheme_type == LabelSchemeType::HAPLOID) {
        label_scheme = "*ACGT";
    } else {
        throw std::runtime_error("Unsupported label scheme type!");
    }

    const auto indices = logits.argmax(-1);  // Shape becomes [N, L]

    std::vector<ConsensusResult> results(indices.size(0));

    for (int64_t sample_id = 0; sample_id < indices.size(0); ++sample_id) {
        const auto& positions = indices[sample_id];

        std::string& seq = results[sample_id].seq;
        seq.resize(positions.size(0), '*');

        for (int64_t j = 0; j < positions.size(0); ++j) {
            const int64_t class_index = positions[j].item<int64_t>();
            assert(class_index < static_cast<int64_t>(std::size(label_scheme)));
            seq[j] = label_scheme[class_index];
        }
    }

    const torch::Tensor probs = torch::gather(logits, -1, indices.unsqueeze(-1)).squeeze(-1);

    // std::cerr << "probs: " << probs << "\n";

    for (int64_t sample_id = 0; sample_id < indices.size(0); ++sample_id) {
        std::string& quals = results[sample_id].quals;
        quals.clear();

        constexpr double cap = 70.0;

        const auto phred_scores =
                (-10.0 * torch::log10(1.0 - probs[sample_id])).clamp(0.0, cap).to(torch::kUInt8) +
                33;

        quals.resize(phred_scores.size(0), '!');
        for (int64_t j = 0; j < phred_scores.size(0); ++j) {
            quals[j] = static_cast<char>(phred_scores[j].item<uint8_t>());
        }
    }

    return results;
}

}  // namespace dorado::polisher
