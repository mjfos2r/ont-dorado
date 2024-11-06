#include "poly_tail_calculator.h"

#include "dna_poly_tail_calculator.h"
#include "plasmid_poly_tail_calculator.h"
#include "poly_tail_config.h"
#include "read_pipeline/messages.h"
#include "rna_poly_tail_calculator.h"
#include "utils/math_utils.h"
#include "utils/sequence_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace dorado::poly_tail {

namespace {
const int kMaxTailLength = PolyTailCalculator::max_tail_length();

struct Interval {
    int start;
    int end;
    float avg;
};

}  // namespace

std::pair<int, int> PolyTailCalculator::signal_range(int signal_anchor,
                                                     int signal_len,
                                                     float samples_per_base,
                                                     bool fwd) const {
    const int kSpread = int(std::round(samples_per_base * max_tail_length()));
    const float start_scale = fwd ? 1.f : 0.1f;
    const float end_scale = fwd ? 0.1f : 1.f;
    return {std::max(0, static_cast<int>(signal_anchor - kSpread * start_scale)),
            std::min(signal_len, static_cast<int>(signal_anchor + kSpread * end_scale))};
}

std::pair<float, float> PolyTailCalculator::estimate_samples_per_base(
        const dorado::SimplexRead& read) const {
    const size_t num_bases = read.read_common.seq.length();
    const auto num_samples = read.read_common.get_raw_data_samples();
    const auto stride = read.read_common.model_stride;
    const auto seq_to_sig_map =
            dorado::utils::moves_to_map(read.read_common.moves, stride, num_samples, num_bases + 1);
    // Store the samples per base in float to use the quantile calcuation function.
    std::vector<float> sizes(seq_to_sig_map.size() - 1, 0.f);
    for (int i = 1; i < int(seq_to_sig_map.size()); i++) {
        sizes[i - 1] = static_cast<float>(seq_to_sig_map[i] - seq_to_sig_map[i - 1]);
    }

    return {average_samples_per_base(sizes), stdev_samples_per_base(sizes)};
}

float PolyTailCalculator::stdev_samples_per_base(const std::vector<float>& sizes) const {
    auto quantiles = dorado::utils::quantiles(sizes, {0.1f, 0.9f});
    float sum = 0.f;
    int count = 0;
    for (auto s : sizes) {
        if (s >= quantiles[0] && s <= quantiles[1]) {
            sum += s * s;
            count++;
        }
    }
    return (count > 0 ? std::sqrt(sum / count) : 0.f);
}

std::pair<int, int> PolyTailCalculator::determine_signal_bounds(int signal_anchor,
                                                                bool fwd,
                                                                const dorado::SimplexRead& read,
                                                                float num_samples_per_base,
                                                                float std_samples_per_base) const {
    const c10::Half* signal = static_cast<c10::Half*>(read.read_common.raw_data.data_ptr());
    int signal_len = int(read.read_common.get_raw_data_samples());

    auto calc_stats = [&](int s, int e) -> std::pair<float, float> {
        float avg = 0;
        for (int i = s; i < e; i++) {
            avg += signal[i];
        }
        avg = avg / (e - s);
        float var = 0;
        for (int i = s; i < e; i++) {
            var += (signal[i] - avg) * (signal[i] - avg);
        }
        var = var / (e - s);
        return {avg, std::sqrt(var)};
    };

    // Maximum variance between consecutive values to be
    // considered part of the same interval.
    const float kVar = 0.35f;
    // How close the mean values should be for consecutive intervals
    // to be merged.
    const float kMeanValueProximity = 0.25f;
    // Maximum gap between intervals that can be combined.
    const int kMaxSampleGap = int(std::round(num_samples_per_base * 5));
    // Minimum size of intervals considered for merge.
    const int kMinIntervalSizeForMerge = kMaxSampleGap * 2;
    // Floor for average signal value of poly tail.
    const float kMinAvgVal = min_avg_val();

    auto [left_end, right_end] = signal_range(signal_anchor, signal_len, num_samples_per_base, fwd);
    spdlog::trace("Bounds left {}, right {}", left_end, right_end);

    std::vector<Interval> intervals;
    const int kStride = 3;

    for (int s = left_end; s < (right_end - kMaxSampleGap); s += kStride) {
        const int e = s + kMaxSampleGap;
        auto [avg, stdev] = calc_stats(s, e);
        if (avg > kMinAvgVal && stdev < kVar) {
            if (intervals.empty()) {
                spdlog::trace("Add new interval {}-{} avg {} stdev {}", s, e, avg, stdev);
                intervals.push_back({s, e, avg});
            } else {
                // If new interval overlaps with the previous interval and
                // intervals have a similar mean, just extend the previous interval.
                auto& last_interval = intervals.back();
                if (last_interval.end >= s &&
                    std::abs(avg - last_interval.avg) < kMeanValueProximity) {
                    // recalc stats for new interval
                    std::tie(avg, stdev) = calc_stats(last_interval.start, e);
                    spdlog::trace("extend interval {}-{} to {}-{} avg {} stdev {}",
                                  last_interval.start, last_interval.end, last_interval.start, e,
                                  avg, stdev);
                    last_interval = Interval{last_interval.start, e, avg};
                } else {
                    spdlog::trace("Add new interval {}-{} avg {} stdev {}", s, e, avg, stdev);
                    intervals.push_back({s, e, avg});
                }
            }
        }
    }

    std::string int_str = "";
    for (const auto& in : intervals) {
        int_str += std::to_string(in.start) + "-" + std::to_string(in.end) + ", ";
    }
    spdlog::trace("found intervals {}", int_str);

    // Cluster intervals if there are interrupted poly tails that should
    // be combined. Interruption length is specified through a config file.
    // In the example below, tail estimation should include both stretches
    // of As along with the small gap in the middle.
    // e.g. -----AAAAAAA--AAAAAA-----
    const int kMaxInterruption = static_cast<int>(std::floor(
            (num_samples_per_base + std_samples_per_base) * m_config.tail_interrupt_length));

    std::vector<Interval> clustered_intervals;
    bool keep_merging = true;
    while (keep_merging) {  // break when we've stopped making changes
        keep_merging = false;
        for (const auto& i : intervals) {
            if (clustered_intervals.empty()) {
                clustered_intervals.push_back(i);
            } else {
                auto& last = clustered_intervals.back();
                bool mean_proximity_ok = std::abs(i.avg - last.avg) < kMeanValueProximity;
                auto separation = i.start - last.end;
                bool skip_glitch = std::abs(separation) < kMaxSampleGap &&
                                   last.end - last.start > kMinIntervalSizeForMerge &&
                                   (i.end - i.start > kMinIntervalSizeForMerge ||
                                    i.end >= right_end - kStride);
                bool allow_linker = separation >= 0 && separation < kMaxInterruption;
                if (mean_proximity_ok && (skip_glitch || allow_linker)) {
                    // retain avg value from the best section to prevent drift for future merges
                    auto& best = (i.end - i.start) < (last.end - last.start) ? last : i;
                    spdlog::trace("extend interval {}-{} to {}-{}", last.start, last.end,
                                  last.start, i.end);
                    last = Interval{last.start, i.end, best.avg};
                    keep_merging = true;
                } else {
                    clustered_intervals.push_back(i);
                }
            }
        }
        std::swap(clustered_intervals, intervals);
        clustered_intervals.clear();
    }

    int_str = "";
    for (const auto& in : intervals) {
        int_str += std::to_string(in.start) + "-" + std::to_string(in.end) + ", ";
    }
    spdlog::trace("clustered intervals {}", int_str);

    // Once the clustered intervals are available, filter them by how
    // close they are to the anchor.
    std::vector<Interval> filtered_intervals;
    std::copy_if(intervals.begin(), intervals.end(), std::back_inserter(filtered_intervals),
                 [&](const auto& i) {
                     auto buffer = buffer_range({i.start, i.end}, num_samples_per_base);
                     // Only keep intervals that are close-ish to the signal anchor.
                     // i.e. the anchor needs to be within the buffer region of
                     // the interval
                     // <----buffer.first---|--- interval ---|---- buffer.second---->
                     bool within_anchor_dist =
                             (signal_anchor >= std::max(0, i.start - buffer.first)) &&
                             (signal_anchor <= (i.end + buffer.second));
                     bool meets_min_base_count =
                             (i.end - i.start) >=
                             std::round(num_samples_per_base * m_config.min_base_count);

                     return within_anchor_dist && meets_min_base_count;
                 });

    int_str = "";
    for (const auto& in : filtered_intervals) {
        int_str += std::to_string(in.start) + "-" + std::to_string(in.end) + ", ";
    }
    spdlog::trace("filtered intervals {}", int_str);

    if (filtered_intervals.empty()) {
        spdlog::trace("Anchor {} No range within anchor proximity found", signal_anchor);
        return {0, 0};
    }

    // Choose the longest interval. If there is a tie for the longest interval,
    // choose the one that is closest to the anchor.
    auto best_interval = std::max_element(filtered_intervals.begin(), filtered_intervals.end(),
                                          [&](const auto& l, const auto& r) {
                                              auto l_size = l.end - l.start;
                                              auto r_size = r.end - r.start;
                                              if (l_size != r_size) {
                                                  return l_size < r_size;
                                              } else {
                                                  if (fwd) {
                                                      return std::abs(l.end - signal_anchor) <
                                                             std::abs(r.end - signal_anchor);
                                                  } else {
                                                      return std::abs(l.start - signal_anchor) <
                                                             std::abs(r.start - signal_anchor);
                                                  }
                                              }
                                          });

    spdlog::trace("Anchor {} Range {} {}", signal_anchor, best_interval->start, best_interval->end);

    return std::make_pair(best_interval->start, best_interval->end);
}

int PolyTailCalculator::calculate_num_bases(const SimplexRead& read,
                                            const SignalAnchorInfo& signal_info) const {
    spdlog::trace("{} Strand {}; poly A/T signal anchor {}", read.read_common.read_id,
                  signal_info.is_fwd_strand ? '+' : '-', signal_info.signal_anchor);

    auto [num_samples_per_base, stddev] = estimate_samples_per_base(read);

    // Walk through signal. Require a minimum of length 10 poly-A since below that
    // the current algorithm returns a lot of false intervals.
    auto [signal_start, signal_end] =
            determine_signal_bounds(signal_info.signal_anchor, signal_info.is_fwd_strand, read,
                                    num_samples_per_base, stddev);

    auto signal_len = signal_end - signal_start;
    signal_len -= signal_length_adjustment(read, signal_len);

    int num_bases = int(std::round(static_cast<float>(signal_len) / num_samples_per_base)) -
                    signal_info.trailing_adapter_bases;

    spdlog::trace(
            "{} PolyA bases {}, signal anchor {} Signal range is {} {} Signal length "
            "{}, samples/base {} trim {} read len {}",
            read.read_common.read_id, num_bases, signal_info.signal_anchor, signal_start,
            signal_end, signal_len, num_samples_per_base, read.read_common.num_trimmed_samples,
            read.read_common.seq.length());

    return num_bases;
}

std::shared_ptr<const PolyTailCalculator>
PolyTailCalculatorFactory::create(const PolyTailConfig& config, bool is_rna, bool is_rna_adapter) {
    if (is_rna) {
        return std::make_unique<RNAPolyTailCalculator>(config, is_rna_adapter);
    }
    if (config.is_plasmid) {
        return std::make_unique<PlasmidPolyTailCalculator>(config);
    }
    return std::make_unique<DNAPolyTailCalculator>(config);
}

}  // namespace dorado::poly_tail
