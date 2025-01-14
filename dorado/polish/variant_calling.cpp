#include "variant_calling.h"

#include "consensus_result.h"
#include "trim.h"
#include "utils/rle.h"
#include "utils/ssize.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <unordered_map>
#include <unordered_set>

namespace dorado::polisher {

/**
 * \brief Copy the draft sequence for a given sample, and expand it with '*' in places of gaps.
 */
std::string extract_draft_with_gaps(const std::string& draft,
                                    const std::vector<int64_t>& positions_major,
                                    const std::vector<int64_t>& positions_minor) {
    if (std::size(positions_major) != std::size(positions_minor)) {
        throw std::runtime_error(
                "The positions_major and positions_minor are not of the same size! "
                "positions_major.size = " +
                std::to_string(std::size(positions_major)) +
                ", positions_minor.size = " + std::to_string(std::size(positions_minor)));
    }

    std::string ret(std::size(positions_major), '*');

    for (int64_t i = 0; i < dorado::ssize(positions_major); ++i) {
        ret[i] = (positions_minor[i] == 0) ? draft[positions_major[i]] : '*';
    }

    return ret;
}

std::string extract_draft(const std::string& draft, const std::vector<int64_t>& positions_major) {
    std::string ret(std::size(positions_major), '*');
    for (int64_t i = 0; i < dorado::ssize(positions_major); ++i) {
        ret[i] = draft[positions_major[i]];
    }
    return ret;
}

VariantCallingSample slice_vc_sample(const VariantCallingSample& vc_sample,
                                     const int64_t idx_start,
                                     const int64_t idx_end) {
    const int64_t num_columns = dorado::ssize(vc_sample.sample.positions_major);

    // Validate lengths.
    if (vc_sample.logits.defined() &&
        (vc_sample.logits.size(0) != dorado::ssize(vc_sample.sample.positions_major))) {
        throw std::runtime_error(
                "VariantCallingSample::logits is of incorrect size. logits.size = " +
                std::to_string(vc_sample.logits.size(0)) +
                ", num_columns = " + std::to_string(num_columns));
    }

    // Validate idx.
    if ((idx_start < 0) || (idx_start >= num_columns) || (idx_start >= idx_end) ||
        (idx_end > num_columns)) {
        throw std::out_of_range("Index is out of range in slice_vc_sample. idx_start = " +
                                std::to_string(idx_start) +
                                ", idx_end = " + std::to_string(idx_end) +
                                ", num_columns = " + std::to_string(num_columns));
    }

    // Slice.
    return VariantCallingSample{
            slice_sample(vc_sample.sample, idx_start, idx_end),
            vc_sample.logits.index({at::indexing::Slice(idx_start, idx_end)}).clone()};
}

std::vector<VariantCallingSample> merge_vc_samples(
        const std::vector<VariantCallingSample>& vc_samples) {
    if (std::empty(vc_samples)) {
        return {};
    }

    std::vector<VariantCallingSample> ret;

    ret.emplace_back(vc_samples.front());

    for (int64_t i = 1; i < dorado::ssize(vc_samples); ++i) {
        if (ret.back().sample.end() == (vc_samples[i].sample.start() + 1)) {
            merge_adjacent_samples_in_place(ret.back().sample, vc_samples[i].sample);
            ret.back().logits = torch::cat({std::move(ret.back().logits), vc_samples[i].logits});
        } else {
            ret.emplace_back(vc_samples[i]);
        }
    }

    return ret;
}

/**
 * \brief This function restructures the neighboring samples for one draft sequence
 */
std::vector<VariantCallingSample> join_samples(const std::vector<VariantCallingSample>& vc_samples,
                                               const std::string& draft,
                                               const DecoderBase& decoder) {
    std::vector<VariantCallingSample> ret;

    std::vector<VariantCallingSample> queue;

    for (int64_t i = 0; i < dorado::ssize(vc_samples); ++i) {
        const VariantCallingSample& vc_sample = vc_samples[i];
        const Sample& sample = vc_sample.sample;

        // Validate the sample.
        sample.validate();

        // Validate the logits
        if (!vc_sample.logits.defined()) {
            throw std::runtime_error("Logits tensor is not defined!");
        }
        if (vc_sample.logits.size(0) != dorado::ssize(vc_sample.sample.positions_major)) {
            throw std::runtime_error(
                    "Length of the logits tensor does not match sample length! logits.size = " +
                    std::to_string(vc_sample.logits.size(0)) + ", positions_major.size = " +
                    std::to_string(std::size(vc_sample.sample.positions_major)));
        }

        // Unsqueeze the logits because this vector contains logits for each individual sample of the shape
        // [positions x class_probabilities], whereas the decode_bases function expects that the first dimension is
        // the batch sample ID. That is, the tensor should be of shape: [batch_sample_id x positions x class_probabilities].
        // In this case, the "batch size" is 1.
        const at::Tensor logits = vc_sample.logits.unsqueeze(0);
        const std::vector<ConsensusResult> c = decoder.decode_bases(logits);

        // This shouldn't be possible.
        if (std::size(c) != 1) {
            spdlog::warn(
                    "Unexpected number of consensus sequences generated from a single sample: "
                    "c.size = {}. Skipping consensus of this sample.",
                    std::size(c));
            continue;
        }

        // Sequences for comparison.
        const std::string& call_with_gaps = c.front().seq;
        const std::string draft_with_gaps =
                extract_draft_with_gaps(draft, sample.positions_major, sample.positions_minor);
        assert(std::size(call_with_gaps) == std::size(draft_with_gaps));

        const auto check_is_diff = [](const char base1, const char base2) {
            return (base1 != base2) || (base1 == '*' && base2 == '*');
        };

        // Check if all positions are diffs, or if all positions are gaps in both sequences.
        {
            int64_t count = 0;
            for (int64_t j = 0; j < dorado::ssize(call_with_gaps); ++j) {
                if (check_is_diff(call_with_gaps[j], draft_with_gaps[j])) {
                    ++count;
                }
            }
            if (count == dorado::ssize(call_with_gaps)) {
                // Merge the entire sample with the next one. We need at least one non-diff non-gap pos.
                queue.emplace_back(vc_sample);
                continue;
            }
        }

        const int64_t num_positions = dorado::ssize(sample.positions_major);

        // Find a location where to split the sample.
        int64_t last_non_var_start = 0;
        for (int64_t j = (num_positions - 1); j >= 0; --j) {
            if ((sample.positions_minor[j] == 0) &&
                !check_is_diff(call_with_gaps[j], draft_with_gaps[j])) {
                last_non_var_start = j;
                break;
            }
        }

        // Split the sample.
        VariantCallingSample left_slice = slice_vc_sample(vc_sample, 0, last_non_var_start);
        VariantCallingSample right_slice =
                slice_vc_sample(vc_sample, last_non_var_start, num_positions);

        // Enqueue the queue if possible.
        if (last_non_var_start > 0) {
            queue.emplace_back(std::move(left_slice));
        }

        // Merge and insert.
        if (!std::empty(queue)) {
            auto new_samples = merge_vc_samples(queue);
            ret.insert(std::end(ret), std::make_move_iterator(std::begin(new_samples)),
                       std::make_move_iterator(std::end(new_samples)));
        }

        // Reset the queue.
        queue = {std::move(right_slice)};
    }

    // Merge and insert.
    if (!std::empty(queue)) {
        auto new_samples = merge_vc_samples(queue);
        ret.insert(std::end(ret), std::make_move_iterator(std::begin(new_samples)),
                   std::make_move_iterator(std::end(new_samples)));
        queue.clear();
    }

    return ret;
}

std::vector<bool> variant_columns(const std::vector<int64_t>& minor,
                                  const std::string& reference,
                                  const std::string& prediction) {
    const bool lengths_valid = (std::size(minor) == std::size(reference)) &&
                               (std::size(reference) == std::size(prediction));
    if (!lengths_valid) {
        std::ostringstream oss;
        oss << "Cannot find variant columns because sequences are not of equal length. minor.size "
               "= "
            << std::size(minor) << ", reference.size = " << std::size(reference)
            << ", prediction.size = " << std::size(prediction);
        throw std::runtime_error(oss.str());
    }

    const int64_t len = dorado::ssize(prediction);
    std::vector<bool> ret(len, false);

    int64_t insert_length = 0;
    bool is_var = (reference[0] != prediction[0]);  // Assume start on major.
    ret[0] = is_var;

    for (int64_t i = 1; i < len; ++i) {
        if (minor[i] == 0) {
            // Start of new reference position.
            if (is_var) {
                // If we saw any vars in an insert run, set all inserts to true.
                for (int64_t j = (i - insert_length); j < i; ++j) {
                    ret[j] = true;
                }
            }
            is_var = (reference[i] != prediction[i]);
            ret[i] = is_var;
            insert_length = 0;
        } else {
            insert_length += 1;
            is_var = (is_var || (reference[i] != prediction[i]));
        }
    }

    // Set any remaining inserts.
    if (is_var) {
        for (int64_t j = (len - insert_length); j <= (len - 1); ++j) {
            ret[j] = true;
        }
    }

    return ret;
}

std::vector<Variant> decode_variants(const DecoderBase& decoder,
                                     const VariantCallingSample& vc_sample,
                                     const std::string& draft,
                                     const bool ambig_ref,
                                     const bool gvcf) {
    // No work to do.
    if (std::empty(vc_sample.sample.positions_major)) {
        return {};
    }

    // Check that the sample begins on non-insertion base.
    if (vc_sample.sample.positions_minor.front() != 0) {
        std::ostringstream oss;
        oss << "The first position of a sample must not be an insertion. sample = "
            << vc_sample.sample;
        throw std::runtime_error(oss.str());
    }

    // Helper lambdas.
    const auto remove_gaps = [](const std::string_view seq) {
        std::string ret;
        ret.reserve(std::size(seq));
        std::copy_if(std::begin(seq), std::end(seq), std::back_inserter(ret),
                     [](char c) { return c != '*'; });
        return ret;
    };
    const auto is_subset_of_symbols = [](const std::unordered_set<char>& symbol_map,
                                         const std::string& query) {
        for (const char c : query) {
            if (symbol_map.count(c) == 0) {
                return false;
            }
        }
        return true;
    };
    const auto create_symbol_lookup = [](const std::string& symbols) {
        std::array<int32_t, 256> ret;
        std::fill(std::begin(ret), std::end(ret), -1);
        for (int32_t i = 0; i < static_cast<int32_t>(std::size(symbols)); ++i) {
            ret[static_cast<int32_t>(symbols[i])] = i;
        }
        return ret;
    };
    const auto encode_seq = [](const std::array<int32_t, 256>& symbol_lookup,
                               const std::string_view seq, const bool substitute_n) {
        std::vector<int32_t> ret(std::size(seq));
        for (int64_t i = 0; i < dorado::ssize(seq); ++i) {
            const char c = (substitute_n && (seq[i] == 'N')) ? '*' : seq[i];
            ret[i] = symbol_lookup[c];
        }
        return ret;
    };
    const auto phred = [](double err, const double cap) {
        err = std::clamp(err, std::pow(10, -cap / 10.0), 1.0);
        const double q = -10.0 * std::log10(err);
        return std::min(q, cap);
    };
    const auto compute_seq_quality = [&encode_seq, &phred](
                                             const std::array<int32_t, 256>& symbol_lookup,
                                             const at::Tensor& class_probs,
                                             const std::string_view seq, const bool substitute_n) {
        const std::vector<int32_t> encoded = encode_seq(symbol_lookup, seq, substitute_n);
        double sum = 0.0;
        for (size_t i = 0; i < std::size(encoded); ++i) {
            const int32_t j = encoded[i];
            const float prob = class_probs[i][j].item<float>();
            const double err = 1.0 - prob;
            sum += phred(err, 70.0);
        }
        return sum;
    };
    const auto format_double = [](const double val, const int32_t dec_places) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(dec_places) << val;
        return oss.str();
    };

    // Alphabet for the label scheme.
    const std::string symbols = decoder.get_label_scheme_symbols();
    const std::unordered_set<char> symbol_set(std::begin(symbols), std::end(symbols));
    const std::array<int32_t, 256> symbol_lookup = create_symbol_lookup(symbols);

    // Predicted sequence with gaps.
    const at::Tensor logits = vc_sample.logits.unsqueeze(0);
    const std::vector<ConsensusResult> c = decoder.decode_bases(logits);
    const std::string& prediction = c.front().seq;

    // Draft sequence with gaps.
    const std::string reference = extract_draft_with_gaps(draft, vc_sample.sample.positions_major,
                                                          vc_sample.sample.positions_minor);

    // Candidate variant positions.
    const std::vector<bool> is_variant =
            variant_columns(vc_sample.sample.positions_minor, reference, prediction);
    const std::vector<std::tuple<int64_t, int64_t, bool>> runs =
            dorado::run_length_encode(is_variant);

    // Extract variants.
    std::vector<Variant> variants;
    for (const auto& [rstart, rend, is_var] : runs) {
        // Skip non-variants.
        if (!is_var) {
            continue;
        }

        // Get the reference and the predictions for the variant stretch.
        const std::string_view var_ref_with_gaps(std::data(reference) + rstart,
                                                 static_cast<size_t>(rend - rstart));
        const std::string_view var_pred_with_gaps(std::data(prediction) + rstart,
                                                  static_cast<size_t>(rend - rstart));

        //     std::begin(reference) + rstart, std::begin(reference) + rend);
        // const std::string_view var_pred_with_gaps(std::begin(prediction) + rstart, std::begin(prediction) + rend);

        // Mutable ref and pred sequences - a ref base may be prepended later.
        std::string var_ref = remove_gaps(var_ref_with_gaps);
        std::string var_pred = remove_gaps(var_pred_with_gaps);

        // Verbatim comment from Medaka:
        //      "del followed by insertion can lead to non-variant"
        //      "maybe skip things if reference contains ambiguous symbols"
        if ((var_ref == var_pred) && is_var) {
            continue;
        } else if (!ambig_ref && !is_subset_of_symbols(symbol_set, var_ref)) {
            continue;
        }

        // // Encode the sequences: base -> number.
        // const std::vector<int32_t> var_ref_encoded = encode_seq(symbol_lookup, var_ref_with_gaps, true);
        // const std::vector<int32_t> var_pred_encoded = encode_seq(symbol_lookup, var_pred_with_gaps, false);
        // const double pred_qual = compute_seq_quality(var_probs, var_ref_encoded);

        // Calculate probabilities.
        const at::Tensor var_probs = vc_sample.logits.slice(0, rstart, rend);
        const double ref_qv =
                compute_seq_quality(symbol_lookup, var_probs, var_ref_with_gaps, true);
        const double pred_qv =
                compute_seq_quality(symbol_lookup, var_probs, var_pred_with_gaps, false);

        // Variant data.
        const double qual = pred_qv - ref_qv;
        const std::string qual_str = format_double(qual, 3);
        const std::unordered_map<std::string, std::string> genotype{
                {"GT", "1"},
                {"GQ", qual_str},
        };
        const int64_t var_pos = vc_sample.sample.positions_major[rstart];
        if (vc_sample.sample.positions_minor[rstart] != 0) {
            // Variant starts on insert - prepend ref base.
            var_ref = draft[var_pos] + var_ref;
            var_pred = draft[var_pos] + var_pred;
        }
        Variant variant{
                vc_sample.sample.seq_id, var_pos, var_ref, var_pred, "PASS", {}, qual_str, genotype,
        };
        variants.emplace_back(std::move(variant));
    }

    if (gvcf) {
    }

    return variants;
}

std::vector<Sample> apply_trimming(const std::vector<const Sample*>& samples,
                                   const std::vector<TrimInfo>& trims) {
    std::vector<Sample> ret;

    for (int64_t i = 0; i < dorado::ssize(trims); ++i) {
        const Sample* s = samples[i];
        const TrimInfo& t = trims[i];
        ret.emplace_back(slice_sample(*s, t.start, t.end));
    }

    return ret;
}

std::vector<Variant> call_variants(const dorado::polisher::Interval& region_batch,
                                   const std::vector<VariantCallingSample>& vc_input_data,
                                   const hts_io::FastxRandomReader& draft_reader,
                                   const std::vector<std::pair<std::string, int64_t>>& draft_lens,
                                   const DecoderBase& decoder) {
    // Group samples by sequence ID.
    std::vector<std::vector<std::pair<int64_t, int32_t>>> groups(region_batch.length());
    for (int32_t i = 0; i < dorado::ssize(vc_input_data); ++i) {
        const auto& [sample, logits] = vc_input_data[i];

        const int32_t local_id = sample.seq_id - region_batch.start;

        // Skip filtered samples.
        if (sample.seq_id < 0) {
            continue;
        }

        if ((sample.seq_id >= dorado::ssize(draft_lens)) || (local_id < 0) ||
            (local_id >= dorado::ssize(groups))) {
            spdlog::error(
                    "Draft ID out of bounds! r.draft_id = {}, draft_lens.size = {}, "
                    "groups.size = {}",
                    sample.seq_id, std::size(draft_lens), std::size(groups));
            continue;
        }
        groups[local_id].emplace_back(sample.start(), i);
    }

    std::vector<Variant> all_variants;

    // For each sequence, call variants.
    for (int64_t group_id = 0; group_id < dorado::ssize(groups); ++group_id) {
        const int64_t seq_id = group_id + region_batch.start;
        const std::string& header = draft_lens[seq_id].first;

        // Sort the group by start positions.
        auto& group = groups[group_id];
        std::sort(std::begin(group), std::end(group));

        if (std::empty(group)) {
            continue;
        }

        // Get the draft sequence.
        const std::string draft = draft_reader.fetch_seq(header);

        // Create a view into samples for this draft.
        std::vector<const Sample*> local_samples;
        local_samples.reserve(std::size(group));
        // NOTE: I wouldn't use a reference here because both start and id are POD, but Clang complains.
        for (const auto& [start, id] : group) {
            local_samples.emplace_back(&(vc_input_data[id].sample));
        }

        // Compute trimming of all samples for this draft sequence.
        const std::vector<TrimInfo> trims = trim_samples(local_samples, std::nullopt);

        // Produce trimmed samples.
        std::vector<Sample> trimmed_samples = apply_trimming(local_samples, trims);

        // Produce trimmed logits.
        std::vector<at::Tensor> trimmed_logits = [&]() {
            std::vector<at::Tensor> ret;
            for (size_t i = 0; i < std::size(trims); ++i) {
                const int64_t id = group[i].second;
                const TrimInfo& trim = trims[i];
                at::Tensor t = vc_input_data[id]
                                       .logits.index({at::indexing::Slice(trim.start, trim.end)})
                                       .clone();
                ret.emplace_back(std::move(t));
            }
            return ret;
        }();

        assert(std::size(trimmed_samples) == std::size(trimmed_logits));

        // Interleave the samples and logits for easier handling.
        const std::vector<VariantCallingSample> trimmed_vc_samples = [&]() {
            std::vector<VariantCallingSample> ret;
            for (size_t i = 0; i < std::size(trimmed_samples); ++i) {
                ret.emplace_back(VariantCallingSample{std::move(trimmed_samples[i]),
                                                      std::move(trimmed_logits[i])});
            }
            return ret;
        }();

        // Break and merge samples on non-variant positions.
        // const auto joined_samples = join_samples(vc_input_data, group, trims, draft, decoder);
        const auto joined_samples = join_samples(trimmed_vc_samples, draft, decoder);

        constexpr bool AMBIG_REF = false;
        constexpr bool GVCF = false;

        for (const auto& vc_sample : joined_samples) {
            std::vector<Variant> variants =
                    decode_variants(decoder, vc_sample, draft, AMBIG_REF, GVCF);
            all_variants.insert(std::end(all_variants),
                                std::make_move_iterator(std::begin(variants)),
                                std::make_move_iterator(std::end(variants)));
        }

        // TODO:
        //      join_samples();
        //      decode_variants();
    }

    return all_variants;
}

}  // namespace dorado::polisher
