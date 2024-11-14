#include "polish_impl.h"

#include "torch_utils/gpu_profiling.h"
#include "utils/ssize.h"

#include <cxxpool.h>
#include <htslib/faidx.h>
#include <spdlog/spdlog.h>
#include <torch/torch.h>

namespace dorado::polisher {

std::ostream& operator<<(std::ostream& os, const Window& w) {
    os << "seq_id = " << w.seq_id << ", start = " << w.start << ", end = " << w.end
       << ", seq_length = " << w.seq_length
       << ", region_id = " << w.region_id;  // << ", window_id = " << w.window_id;
    return os;
}

/**
 * \brief Linearly splits sequence lengths into windows. It also returns the backward mapping of which
 *          windows correspond to which sequences, needed for stitching.
 */
std::vector<Window> create_windows(const int32_t seq_id,
                                   const int64_t seq_start,
                                   const int64_t seq_end,
                                   const int64_t seq_len,
                                   const int32_t window_len,
                                   const int32_t window_overlap,
                                   const int32_t region_id) {
    if (window_overlap >= window_len) {
        spdlog::error(
                "The window overlap cannot be larger than the window size! window_len = {}, "
                "window_overlap = {}\n",
                window_len, window_overlap);
        return {};
    }

    std::vector<Window> ret;
    const int64_t length = seq_end - seq_start;

    const int32_t num_windows =
            static_cast<int32_t>(std::ceil(static_cast<double>(length) / window_len));

    ret.reserve(num_windows);

    int32_t win_id = 0;
    for (int64_t start = seq_start; start < seq_end;
         start += (window_len - window_overlap), ++win_id) {
        const int64_t end = std::min(seq_end, start + window_len);
        const int64_t start_no_overlap =
                (start == seq_start) ? start : std::min<int64_t>(start + window_overlap, seq_end);

        ret.emplace_back(Window{
                seq_id,
                seq_len,
                start,
                end,
                region_id,
                start_no_overlap,
                end,
        });

        if (end == seq_end) {
            break;
        }
    }

    return ret;
}

std::string fetch_seq(const std::filesystem::path& index_fn,
                      const std::string& seq_name,
                      int32_t start = 0,
                      int32_t end = -1) {
    faidx_t* fai = fai_load(index_fn.c_str());
    if (!fai) {
        spdlog::error("Failed to load index for file: '{}'.", index_fn.string());
        return {};
    }

    const int32_t seq_len = faidx_seq_len(fai, seq_name.c_str());

    start = std::max(start, 0);
    end = (end < 0) ? seq_len : std::min(end, seq_len);

    int32_t temp_seq_len = 0;
    char* seq = faidx_fetch_seq(fai, seq_name.c_str(), start, end - 1, &temp_seq_len);

    if (end <= start) {
        spdlog::error(
                "Cannot load sequence because end <= start! seq_name = {}, start = {}, end = {}.",
                seq_name, start, end);
        return {};
    }

    if (temp_seq_len != (end - start)) {
        spdlog::error(
                "Loaded sequence length does not match the specified interval! seq_name = {}, "
                "start = {}, end = {}, loaded len = {}.",
                seq_name, start, end, temp_seq_len);
        return {};
    }

    std::string ret;
    if (seq) {
        ret = std::string(seq, temp_seq_len);
        free(seq);
    }

    fai_destroy(fai);

    return ret;
}

[[maybe_unused]] void debug_print_samples(std::ostream& os,
                                          const std::vector<polisher::Sample>& samples,
                                          int64_t start = 0,
                                          int64_t end = -1,
                                          int64_t debug_id = -1) {
    start = std::max<int64_t>(0, start);
    end = (end <= 0) ? static_cast<int64_t>(std::size(samples)) : end;

    for (int64_t i = start; i < end; ++i) {
        os << "[i = " << i << "] ";
        polisher::debug_print_sample(os, samples[i], 0, -1, i == debug_id);
        os << '\n';
    }
}

void remove_deletions(polisher::ConsensusResult& cons) {
    if (std::size(cons.seq) != std::size(cons.quals)) {
        spdlog::error(
                "[remove_deletions] Sequence and quality string length mismatch! Not removing "
                "anything. seq.size = {}, quals.size = {}",
                std::size(cons.seq), std::size(cons.quals));
        return;
    }
    size_t n = 0;
    for (size_t j = 0; j < std::size(cons.seq); ++j) {
        if (cons.seq[j] == '*') {
            continue;
        }
        cons.seq[n] = cons.seq[j];
        cons.quals[n] = cons.quals[j];
        ++n;
    }
    cons.seq.resize(n);
    cons.quals.resize(n);
}

polisher::ConsensusResult stitch_sequence(
        const std::filesystem::path& in_draft_fn,
        const std::string& header,
        const std::vector<polisher::Sample>& samples,
        const std::vector<polisher::TrimInfo>& trims,
        const std::vector<polisher::ConsensusResult>& sample_results,
        const std::vector<std::pair<int64_t, int32_t>>& samples_for_seq,
        [[maybe_unused]] const int32_t seq_id) {
    if (std::size(samples) != std::size(trims)) {
        throw std::runtime_error("Number of samples and trims differs! std::size(samples) = " +
                                 std::to_string(std::size(samples)) +
                                 ", std::size(trims) = " + std::to_string(std::size(trims)));
    }

    const std::string draft = fetch_seq(in_draft_fn, header);

    if (std::empty(samples_for_seq)) {
        std::string dummy_quals(std::size(draft), '!');
        return polisher::ConsensusResult{draft, std::move(dummy_quals)};
    }

    polisher::ConsensusResult result;

#ifdef DEBUG_POLISH_DUMP_SEQ_PIECES
    std::ofstream ofs("debug.seq_id_" + std::to_string(seq_id) + ".fasta");
#endif

    // This is an inclusive coordinate. If it was 0, then adding front draft chunk would miss 1 base.
    int64_t last_end = -1;
    for (size_t i = 0; i < std::size(samples_for_seq); ++i) {
        const int32_t sample_index = samples_for_seq[i].second;
        const polisher::Sample& sample = samples[sample_index];
        const polisher::ConsensusResult& sample_result = sample_results[sample_index];
        const polisher::TrimInfo& trim = trims[sample_index];

        const int64_t start_pos = sample.positions_major[trim.start];
        const int64_t end_pos = sample.positions_major.back();

        if (start_pos > (last_end + 1)) {
            result.seq += draft.substr(last_end + 1, start_pos - last_end - 1);
            result.quals += std::string(start_pos - last_end - 1, '!');
        }

#ifdef DEBUG_POLISH_DUMP_SEQ_PIECES
        {
            polisher::ConsensusResult tmp{
                    sample_result.seq.substr(trim.start, trim.end - trim.start), {}};
            remove_deletions(tmp);
            ofs << ">seq_id_" << seq_id << "_part_" << i << '\n' << tmp.seq << '\n';
        }
#endif

        result.seq += sample_result.seq.substr(trim.start, trim.end - trim.start);
        result.quals += sample_result.quals.substr(trim.start, trim.end - trim.start);

        last_end = end_pos;
    }

    // Add the back draft part.
    if ((last_end + 1) < dorado::ssize(draft)) {
        result.seq += draft.substr(last_end + 1);
        result.quals += std::string(dorado::ssize(draft) - last_end - 1, '!');
    }

    return result;
}

std::vector<polisher::Sample> split_sample_on_discontinuities(polisher::Sample& sample) {
    std::vector<polisher::Sample> results;

    const auto find_gaps = [](const std::vector<int64_t>& positions,
                              int64_t threshold = 1) -> std::vector<int64_t> {
        std::vector<int64_t> ret;
        for (size_t i = 1; i < std::size(positions); ++i) {
            if ((positions[i] - positions[i - 1]) > threshold) {
                ret.emplace_back(i);
            }
        }
        return ret;
    };

    // for (auto& data : pileups) {
    const std::vector<int64_t> gaps = find_gaps(sample.positions_major);

    if (std::empty(gaps)) {
        return {sample};

    } else {
        int64_t start = 0;
        for (const int64_t i : gaps) {
            std::vector<int64_t> new_major_pos(sample.positions_major.begin() + start,
                                               sample.positions_major.begin() + i);
            std::vector<int64_t> new_minor_pos(sample.positions_minor.begin() + start,
                                               sample.positions_minor.begin() + i);

            results.emplace_back(
                    polisher::Sample{sample.features.slice(0, start, i), std::move(new_major_pos),
                                     std::move(new_minor_pos), sample.depth.slice(0, start, i),
                                     sample.seq_id, sample.region_id});
            start = i;
        }

        if (start < static_cast<int64_t>(std::size(sample.positions_major))) {
            std::vector<int64_t> new_major_pos(sample.positions_major.begin() + start,
                                               sample.positions_major.end());
            std::vector<int64_t> new_minor_pos(sample.positions_minor.begin() + start,
                                               sample.positions_minor.end());
            results.emplace_back(
                    polisher::Sample{sample.features.slice(0, start), std::move(new_major_pos),
                                     std::move(new_minor_pos), sample.depth.slice(0, start),
                                     sample.seq_id, sample.region_id});
        }
    }
    // }

    return results;
}

std::vector<polisher::Sample> merge_adjacent_samples(std::vector<polisher::Sample>& samples) {
    std::vector<torch::Tensor> features_buffer;
    std::vector<std::vector<int64_t>> positions_major_buffer;
    std::vector<std::vector<int64_t>> positions_minor_buffer;
    std::vector<torch::Tensor> depth_buffer;
    int32_t seq_id_buffer = -1;
    int32_t region_id_buffer = -1;
    int64_t last_end = -1;

    std::vector<polisher::Sample> results;

    const auto cat_vectors = [](const std::vector<std::vector<int64_t>>& vecs) {
        size_t size = 0;
        for (const auto& vec : vecs) {
            size += std::size(vec);
        }
        std::vector<int64_t> ret;
        ret.reserve(size);
        for (const auto& vec : vecs) {
            ret.insert(std::end(ret), std::cbegin(vec), std::cend(vec));
        }
        return ret;
    };

    for (auto& sample : samples) {
        if (std::empty(sample.positions_major)) {
            continue;
        }
        const int64_t start = sample.start();

        if (std::empty(features_buffer) ||
            ((sample.seq_id == seq_id_buffer) && (sample.region_id == region_id_buffer) &&
             ((start - last_end) == 0))) {
            // New or contiguous chunk.
            last_end = sample.end();
            features_buffer.emplace_back(std::move(sample.features));
            positions_major_buffer.emplace_back(std::move(sample.positions_major));
            positions_minor_buffer.emplace_back(std::move(sample.positions_minor));
            depth_buffer.emplace_back(std::move(sample.depth));
            seq_id_buffer = sample.seq_id;
            region_id_buffer = sample.region_id;

        } else {
            // Discontinuity found, finalize the current chunk
            last_end = sample.end();

            // The torch::cat is slow, so just move if there is nothing to concatenate.
            if (std::size(features_buffer) == 1) {
                results.emplace_back(polisher::Sample{std::move(features_buffer.front()),
                                                      std::move(positions_major_buffer.front()),
                                                      std::move(positions_minor_buffer.front()),
                                                      std::move(depth_buffer.front()),
                                                      seq_id_buffer, region_id_buffer});
            } else {
                results.emplace_back(polisher::Sample{
                        torch::cat(std::move(features_buffer)), cat_vectors(positions_major_buffer),
                        cat_vectors(positions_minor_buffer), torch::cat(std::move(depth_buffer)),
                        seq_id_buffer, region_id_buffer});
            }
            features_buffer = {std::move(sample.features)};
            positions_major_buffer = {std::move(sample.positions_major)};
            positions_minor_buffer = {std::move(sample.positions_minor)};
            depth_buffer = {std::move(sample.depth)};
            seq_id_buffer = sample.seq_id;
            region_id_buffer = sample.region_id;
        }
    }

    if (!features_buffer.empty()) {
        // The torch::cat is slow, so just move if there is nothing to concatenate.
        if (std::size(features_buffer) == 1) {
            results.emplace_back(polisher::Sample{
                    std::move(features_buffer.front()), std::move(positions_major_buffer.front()),
                    std::move(positions_minor_buffer.front()), std::move(depth_buffer.front()),
                    seq_id_buffer, region_id_buffer});
        } else {
            results.emplace_back(polisher::Sample{
                    torch::cat(std::move(features_buffer)), cat_vectors(positions_major_buffer),
                    cat_vectors(positions_minor_buffer), torch::cat(std::move(depth_buffer)),
                    seq_id_buffer, region_id_buffer});
        }
    }

    return results;
}

/**
 * \brief Takes an input sample and splits it bluntly if it has too many positions. This can happen when
 *          there are many long insertions in an input window, and can easily cause out-of-memory issues on the GPU
 *          if the sample is not split.
 *          Splitting is implemented to match Medaka, where a simple sliding window is used to create smaller samples.
 *          In case of a smalle trailing portion (smaller than chunk_len), a potentially large overlap is produced to
 *          cover this region instead of just outputing the small chunk.
 */
std::vector<polisher::Sample> split_samples(std::vector<polisher::Sample> samples,
                                            const int64_t chunk_len,
                                            const int64_t chunk_overlap) {
    if ((chunk_overlap < 0) || (chunk_overlap > chunk_len)) {
        throw std::runtime_error(
                "Wrong chunk_overlap length. chunk_len = " + std::to_string(chunk_len) +
                ", chunk_overlap = " + std::to_string(chunk_overlap));
    }

    const auto create_chunk = [](const polisher::Sample& sample, const int64_t start,
                                 const int64_t end) {
        torch::Tensor new_features = sample.features.slice(0, start, end);
        std::vector<int64_t> new_major(std::begin(sample.positions_major) + start,
                                       std::begin(sample.positions_major) + end);
        std::vector<int64_t> new_minor(std::begin(sample.positions_minor) + start,
                                       std::begin(sample.positions_minor) + end);
        torch::Tensor new_depth = sample.depth.slice(0, start, end);
        return polisher::Sample{std::move(new_features), std::move(new_major), std::move(new_minor),
                                std::move(new_depth),    sample.seq_id,        sample.region_id};
    };

    std::vector<polisher::Sample> results;
    results.reserve(std::size(samples));

    for (auto& sample : samples) {
        const int64_t sample_len = static_cast<int64_t>(std::size(sample.positions_major));

        if (sample_len <= chunk_len) {
            results.emplace_back(std::move(sample));
            continue;
        }

        const int64_t step = chunk_len - chunk_overlap;

        int64_t end = 0;
        for (int64_t start = 0; start < (sample_len - chunk_len + 1); start += step) {
            end = start + chunk_len;
            results.emplace_back(create_chunk(sample, start, end));
        }

        // This will create a chunk with potentially large overlap.
        if (end < sample_len) {
            const int64_t start = sample_len - chunk_len;
            end = sample_len;
            results.emplace_back(create_chunk(sample, start, end));
        }
    }

    return results;
}

std::tuple<std::string, int64_t, int64_t> parse_region_string(const std::string& region) {
    const size_t colon_pos = region.find(':');
    if (colon_pos == std::string::npos) {
        return {region, -1, -1};
    }

    std::string name = region.substr(0, colon_pos);

    if ((colon_pos + 1) == std::size(region)) {
        return {std::move(name), -1, -1};
    }

    size_t dash_pos = region.find('-', colon_pos + 1);
    dash_pos = (dash_pos == std::string::npos) ? std::size(region) : dash_pos;
    const int64_t start =
            ((dash_pos - colon_pos - 1) == 0)
                    ? -1
                    : std::stoll(region.substr(colon_pos + 1, dash_pos - colon_pos - 1)) - 1;
    const int64_t end =
            ((dash_pos + 1) < std::size(region)) ? std::stoll(region.substr(dash_pos + 1)) : -1;

    return {std::move(name), start, end};
}

std::vector<Interval> compute_chunks(const int32_t num_items, const int32_t num_chunks) {
    std::vector<Interval> chunks;
    const int32_t chunk_size = num_items / num_chunks;
    std::vector<int32_t> chunk_sizes(num_chunks, chunk_size);
    for (int32_t i = 0; i < (num_items % num_chunks); ++i) {
        ++chunk_sizes[i];
    }
    int32_t sum = 0;
    for (const int32_t v : chunk_sizes) {
        if (v == 0) {
            continue;
        }
        chunks.emplace_back(Interval{sum, sum + v});
        sum += v;
    }
    if (sum != num_items) {
        throw std::runtime_error{
                "Wrong sum of items divided into chunks! num_items = " + std::to_string(num_items) +
                ", num_chunks = " + std::to_string(num_chunks) + ", sum = " + std::to_string(sum)};
    }
    return chunks;
}

std::pair<std::vector<polisher::Sample>, std::vector<polisher::TrimInfo>> create_samples(
        const std::filesystem::path& in_aln_bam_fn,
        const std::vector<Window>& bam_regions,
        const std::vector<std::pair<std::string, int64_t>>& draft_lens,
        const int32_t num_threads,
        const int32_t window_len,
        const int32_t window_overlap) {
    // Open the BAM file for each thread and spawn encoders.
    spdlog::info("Creating {} encoders.", num_threads);
    std::vector<bam_fset*> bam_sets;
    std::vector<polisher::CountsFeatureEncoder> encoders;
    for (int32_t i = 0; i < num_threads; ++i) {
        bam_sets.emplace_back(create_bam_fset(in_aln_bam_fn.c_str()));
        encoders.emplace_back(polisher::CountsFeatureEncoder(bam_sets.back()));
    }

    /// Medaka has a slightly strange window construction:
    //  1. It splits BAM references into 100kbp overlapping BAM windows.
    //  2. Each BAM window is processed separately.
    //  3. If a BAM window is > window_len (10kbp), it is linearly split (no overlaps)
    //      into 10kbp pieces for parallel processing.
    //  4. Each sub-window region is fetched from BAM, pileup counts are constructed,
    //      features are computed and converted into tensors. If there is a gap in coverage
    //      in any of these tensors (based on ref coordinates), such tensors are split.
    //  5. All neighboring sub-windows (zero distance in ref coordinates) are then merged
    //      back into the total BAM window. Only coverage gaps are split.
    //  6. Samples are then generated by splitting the BAM window tensors in 10k rows with
    //      1k overlaps. This does not correspond to 10k positions in the reference, because
    //      the tensors have also insertions and can grow significantly.
    // It parallelizes this process on both levels of windowing.

    spdlog::info("Input: {} BAM windows from {} sequences.", std::size(bam_regions),
                 std::size(draft_lens));

#ifdef DEBUG_POLISH_REGIONS
    for (size_t i = 0; i < std::size(draft_lens); ++i) {
        std::cerr << "[draft i = " << i << "] name = " << draft_lens[i].first
                  << ", len = " << draft_lens[i].second << "\n";
    }
    for (size_t i = 0; i < std::size(bam_regions); ++i) {
        std::cerr << "[bam_regions i = " << i << "] " << bam_regions[i] << "\n";
    }
#endif

    // Split BAM regions into non-overlapping windows for parallel processing.
    // The non-overlapping windows will be merged after samples are constructed.
    std::vector<Window> windows;
    std::vector<Interval> bam_region_intervals;
    for (size_t i = 0; i < std::size(bam_regions); ++i) {
        const Window& bw = bam_regions[i];
        std::vector<Window> new_windows =
                create_windows(bw.seq_id, bw.start, bw.end, bw.seq_length, window_len, 0, i);
        if (std::empty(new_windows)) {
            continue;
        }
        const int32_t num_windows = static_cast<int32_t>(std::size(windows));
        const int32_t num_new_windows = static_cast<int32_t>(std::size(new_windows));
        bam_region_intervals.emplace_back(Interval{num_windows, num_windows + num_new_windows});
        windows.reserve(std::size(windows) + std::size(new_windows));
        windows.insert(std::end(windows), std::begin(new_windows), std::end(new_windows));
    }

    // Convert windows to samples in parallel.
    std::vector<polisher::Sample> parallel_results;
    {
        const auto worker = [&](const int32_t thread_id, const int32_t start, const int32_t end,
                                std::vector<polisher::Sample>& results) {
            for (int32_t i = start; i < end; ++i) {
                const auto& window = windows[i];
                const std::string& name = draft_lens[window.seq_id].first;
                if (thread_id == 0) {
                    spdlog::debug(
                            "Processing i = {}, start = {}, end = {}, region = "
                            "{}:{}-{} ({} %).",
                            i, start, end, name, window.start, window.end,
                            100.0 * static_cast<double>(i - start) / (end - start));
                }
                results[i] = encoders[thread_id].encode_region(name, window.start, window.end,
                                                               window.seq_id);
            }
        };
        parallel_results.resize(std::size(windows));
        const std::vector<Interval> chunks =
                compute_chunks(static_cast<int32_t>(std::size(windows)), num_threads);
        spdlog::info("Starting to encode regions for {} windows using {} threads.",
                     std::size(windows), std::size(chunks));
        cxxpool::thread_pool pool{std::size(chunks)};
        std::vector<std::future<void>> futures;
        futures.reserve(std::size(chunks));
        for (int32_t tid = 0; tid < static_cast<int32_t>(std::size(chunks)); ++tid) {
            const auto [chunk_start, chunk_end] = chunks[tid];
            futures.emplace_back(
                    pool.push(worker, tid, chunk_start, chunk_end, std::ref(parallel_results)));
        }
        for (auto& f : futures) {
            f.wait();
        }
    }

    spdlog::info("Merging the samples into {} BAM chunks.", std::size(bam_region_intervals));

    // Three tasks for this worker:
    //  1. Merge adjacent samples, which were split for efficiencly of computing the pileup.
    //  2. Check for discontinuities in any of the samples and split (gap in coverage).
    //  3. Split the merged samples into equally sized pieces which will be used for inference.
    std::vector<std::vector<polisher::Sample>> merged_samples;
    std::vector<std::vector<polisher::TrimInfo>> merged_trims;
    {
        const auto worker = [&](const int32_t start, const int32_t end,
                                std::vector<std::vector<polisher::Sample>>& results,
                                std::vector<std::vector<polisher::TrimInfo>>& results_trims) {
            for (int32_t bam_chunk_id = start; bam_chunk_id < end; ++bam_chunk_id) {
                const Interval interval = bam_region_intervals[bam_chunk_id];

#ifdef DEBUG_POLISH_SAMPLE_CONSTRUCTION
                std::cout << "[merged_samples worker bam_chunk_id = " << bam_chunk_id
                          << "] Before merging. interval = [" << interval.start << ", "
                          << interval.end << ">:\n";
                std::cout << "- [bam_chunk_id = " << bam_chunk_id
                          << "] Input (parallel_results):\n";
                debug_print_samples(std::cout, parallel_results, interval.start, interval.end);
#endif

                std::vector<polisher::Sample> local_samples;

                // Split all samples on discontinuities.
                for (int32_t sample_id = interval.start; sample_id < interval.end; ++sample_id) {
                    auto& sample = parallel_results[sample_id];
                    std::vector<polisher::Sample> split_samples =
                            split_sample_on_discontinuities(sample);
                    local_samples.insert(std::end(local_samples),
                                         std::make_move_iterator(std::begin(split_samples)),
                                         std::make_move_iterator(std::end(split_samples)));
                }

#ifdef DEBUG_POLISH_SAMPLE_CONSTRUCTION
                std::cout << "- [bam_chunk_id = " << bam_chunk_id
                          << "] After splitting on discontinuities (local_samples):\n";
                debug_print_samples(std::cout, local_samples);
#endif

                local_samples = merge_adjacent_samples(local_samples);

#ifdef DEBUG_POLISH_SAMPLE_CONSTRUCTION
                std::cout << "- [bam_chunk_id = " << bam_chunk_id
                          << "] After merging adjacent (local_samples):\n";
                debug_print_samples(std::cout, local_samples);
#endif

                local_samples = split_samples(std::move(local_samples), window_len, window_overlap);

#ifdef DEBUG_POLISH_SAMPLE_CONSTRUCTION
                std::cout << "- [bam_chunk_id = " << bam_chunk_id
                          << "] After splitting samples (local_samples):\n";
                debug_print_samples(std::cout, local_samples);
#endif

                const Window& reg = bam_regions[bam_chunk_id];
                results_trims[bam_chunk_id] = polisher::trim_samples(
                        local_samples, {reg.seq_id, reg.start_no_overlap, reg.end_no_overlap});
                results[bam_chunk_id] = std::move(local_samples);
            }
        };
        merged_samples.resize(std::size(bam_region_intervals));
        merged_trims.resize(std::size(bam_region_intervals));

        // Process BAM windows in parallel.
        const std::vector<Interval> chunks =
                compute_chunks(static_cast<int32_t>(std::size(bam_region_intervals)), 1);
        spdlog::info("Starting to merge samples for {} BAM windows using {} threads.",
                     std::size(bam_region_intervals), std::size(chunks));

        cxxpool::thread_pool pool{std::size(chunks)};
        std::vector<std::future<void>> futures;
        futures.reserve(std::size(chunks));

        for (int32_t tid = 0; tid < static_cast<int32_t>(std::size(chunks)); ++tid) {
            const auto [chunk_start, chunk_end] = chunks[tid];
            futures.emplace_back(pool.push(worker, chunk_start, chunk_end, std::ref(merged_samples),
                                           std::ref(merged_trims)));
        }
        for (auto& f : futures) {
            f.wait();
        }
    }

    // Flatten the samples.
    size_t num_samples = 0;
    for (const auto& vals : merged_samples) {
        num_samples += std::size(vals);
    }

    std::vector<polisher::Sample> samples;
    samples.reserve(num_samples);
    for (auto& vals : merged_samples) {
        samples.insert(std::end(samples), std::make_move_iterator(std::begin(vals)),
                       std::make_move_iterator(std::end(vals)));
    }

    std::vector<polisher::TrimInfo> trims;
    trims.reserve(num_samples);
    for (auto& vals : merged_trims) {
        trims.insert(std::end(trims), std::make_move_iterator(std::begin(vals)),
                     std::make_move_iterator(std::end(vals)));
    }

    for (auto& bam_set : bam_sets) {
        destroy_bam_fset(bam_set);
    }

    spdlog::info("Total num samples to infer: {}", std::size(samples));

    return {std::move(samples), std::move(trims)};
}

void process_samples(polisher::TorchModel& model,
                     const polisher::CountsFeatureDecoder& decoder,
                     const std::vector<polisher::Sample>& in_samples,
                     const std::vector<int64_t>& in_samples_to_process,
                     const int32_t batch_size,
                     std::vector<polisher::ConsensusResult>& results) {
    /**
     * \brief This creates a copy of the features from samples, so we have the original ones for trimming.
     */

    auto batch_infer = [&model](const std::vector<polisher::Sample>& samples,
                                const std::vector<int64_t>& samples_to_process) {
        utils::ScopedProfileRange infer("infer", 1);

        at::InferenceMode infer_guard;

        // We can simply stack these since all windows are of the same size. (Smaller windows are set aside.)
        std::vector<torch::Tensor> batch_features;
        for (const int64_t i : samples_to_process) {
            batch_features.emplace_back(samples[i].features);
        }
        torch::Tensor batch_features_tensor = torch::stack(batch_features);

        spdlog::info(
                "About to call forward(): batch_features_tensor.size() = ({}, {}, {}), approx "
                "size: {} MB.",
                batch_features_tensor.size(0), batch_features_tensor.size(1),
                batch_features_tensor.size(2),
                batch_features_tensor.numel() * batch_features_tensor.element_size() /
                        (1024.0 * 1024.0));

        torch::Tensor output;
        try {
            output = model.predict_on_batch(std::move(batch_features_tensor));
        } catch (std::exception& e) {
            std::cerr << "ERROR! Exception caught: " << e.what() << "\n";
            throw e;
        }

        return output;
    };

    results.resize(std::size(in_samples));

    const int64_t num_samples = dorado::ssize(in_samples_to_process);

    for (int64_t start = 0; start < num_samples; start += batch_size) {
        const int64_t end = std::min((start + batch_size), num_samples);

        const std::vector<int64_t> ids(std::begin(in_samples_to_process) + start,
                                       std::begin(in_samples_to_process) + end);

        torch::Tensor output = batch_infer(in_samples, ids);

        // Convert to sequences and qualities.
        std::vector<polisher::ConsensusResult> new_results = decoder.decode_bases(output);

        assert(static_cast<int64_t>(std::size(new_results)) == (end - start));

        // Trim the overlapping sequences.
        for (int64_t j = 0; j < dorado::ssize(new_results); ++j) {
            auto& result = new_results[j];
            const int64_t sample_id = ids[j];
            results[sample_id] = std::move(result);
        }

        spdlog::info(
                "Processed a batch of {} samples. Total samples processed: {}, "
                "num_samples = {}.",
                (end - start), end, num_samples);
    }
}

std::vector<polisher::ConsensusResult> process_samples_in_parallel(
        const std::vector<polisher::Sample>& in_samples,
        const std::vector<std::shared_ptr<polisher::TorchModel>>& models,
        const polisher::CountsFeatureDecoder& decoder,
        const int32_t window_len,
        const int32_t batch_size) {
    if (std::empty(models)) {
        throw std::runtime_error("No models have been initialized, cannot run inference.");
    }

    const auto worker = [&models, &decoder, &in_samples, &batch_size, &window_len](
                                const int32_t thread_id, const int32_t chunk_start,
                                const int32_t chunk_end,
                                std::vector<polisher::ConsensusResult>& results) {
        assert(chunk_end <= dorado::ssize(in_samples));

        // Find samples which will not fit into the batch tensor.
        std::vector<int64_t> regular;
        std::vector<int64_t> remainders;
        for (int64_t i = chunk_start; i < chunk_end; ++i) {
            const auto& sample = in_samples[i];
            if (dorado::ssize(sample.positions_major) != window_len) {
                remainders.emplace_back(i);
                continue;
            }
            regular.emplace_back(i);
        }

        std::cerr << "[thread_id = " << thread_id << "] chunk_start = " << chunk_start
                  << ", chunk_end = " << chunk_end << ", regular.size() = " << regular.size()
                  << ", remainders.size() = " << remainders.size() << "\n";

        // Infer samples which can fully fit into a Nx10x10000 tensor.
        process_samples(*models[thread_id], decoder, in_samples, regular, batch_size, results);

        // Infer samples which are of varying size. Cannot use padding in case of bidirectional GRU.
        process_samples(*models[thread_id], decoder, in_samples, remainders, 1, results);
    };

    std::vector<polisher::ConsensusResult> results(std::size(in_samples));

    const int32_t num_items = dorado::ssize(in_samples);
    const int32_t num_threads = dorado::ssize(models);
    const std::vector<Interval> chunks = compute_chunks(num_items, num_threads);

    spdlog::info("Starting to call consensus for {} samples using {} devices.", num_items,
                 num_threads);

    cxxpool::thread_pool pool{std::size(chunks)};

    std::vector<std::future<void>> futures;
    futures.reserve(std::size(chunks));

    for (int32_t tid = 0; tid < static_cast<int32_t>(std::size(chunks)); ++tid) {
        const auto [chunk_start, chunk_end] = chunks[tid];
        futures.emplace_back(pool.push(worker, tid, chunk_start, chunk_end, std::ref(results)));
    }
    for (auto& f : futures) {
        f.wait();
    }

    spdlog::info("Finished calling consensus.");

    return results;
}

std::vector<Window> create_bam_regions(
        const std::vector<std::pair<std::string, int64_t>>& draft_lens,
        const int32_t bam_chunk_len,
        const int32_t window_overlap,
        const std::string& region_str) {
    // Canonical case where each sequence is linearly split with an overlap.
    if (std::empty(region_str)) {
        std::vector<Window> windows;
        for (int32_t seq_id = 0; seq_id < dorado::ssize(draft_lens); ++seq_id) {
            const int64_t len = draft_lens[seq_id].second;
            const std::vector<Window> new_windows =
                    create_windows(seq_id, 0, len, len, bam_chunk_len, window_overlap, -1);
            windows.reserve(std::size(windows) + std::size(new_windows));
            windows.insert(windows.end(), new_windows.begin(), new_windows.end());
        }
        return windows;
    } else {
        // Create windows for only this one region.

        auto [region_name, region_start, region_end] = parse_region_string(region_str);

        spdlog::info("Processing a custom region: '{}:{}-{}'.", region_name, region_start + 1,
                     region_end);

        // Find the sequence ID of the region sequence name.
        int32_t seq_id = -1;
        int64_t seq_length = 0;
        for (int32_t i = 0; i < static_cast<int32_t>(std::size(draft_lens)); ++i) {
            if (draft_lens[i].first == region_name) {
                seq_id = i;
                seq_length = draft_lens[i].second;
                break;
            }
        }
        if (region_start < 0) {
            region_start = 0;
        }
        if (region_end <= 0) {
            region_end = seq_length;
        }
        if (seq_id < 0) {
            throw std::runtime_error(
                    "Sequence provided by custom region not found in input! region_name = " +
                    region_name);
        }

        // Split-up the custom region if it's too long.
        const std::vector<Window> windows = create_windows(
                seq_id, region_start, region_end, seq_length, bam_chunk_len, window_overlap, -1);

        return windows;
    }
}

}  // namespace dorado::polisher
