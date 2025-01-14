#include "bam_utils.h"

#include "Version.h"
#include "htslib/bgzf.h"
#include "htslib/kroundup.h"
#include "htslib/sam.h"
#include "minimap.h"
//todo: mmpriv.h is a private header from mm2 for the mm_event_identity function.
//Ask lh3 t  make some of these funcs publicly available?
#include "mmpriv.h"
#include "read_pipeline/ReadPipeline.h"
#include "utils/sequence_utils.h"
#include "utils/types.h"

#include <indicators/progress_bar.hpp>
#include <spdlog/spdlog.h>

#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace dorado::utils {

Aligner::Aligner(MessageSink& sink,
                 const std::string& filename,
                 int k,
                 int w,
                 uint64_t index_batch_size,
                 int threads)
        : MessageSink(10000), m_sink(sink), m_threads(threads) {
    // Initialize option structs.
    mm_set_opt(0, &m_idx_opt, &m_map_opt);
    // Setting options to map-ont default till relevant args are exposed.
    mm_set_opt("map-ont", &m_idx_opt, &m_map_opt);

    m_idx_opt.k = k;
    m_idx_opt.w = w;
    spdlog::info("> Index parameters input by user: kmer size={} and window size={}.", m_idx_opt.k,
                 m_idx_opt.w);

    // Set batch sizes large enough to not require chunking since that's
    // not supported yet.
    m_idx_opt.batch_size = index_batch_size;
    m_idx_opt.mini_batch_size = index_batch_size;

    // Force cigar generation.
    m_map_opt.flag |= MM_F_CIGAR;

    mm_check_opt(&m_idx_opt, &m_map_opt);

    m_index_reader = mm_idx_reader_open(filename.c_str(), &m_idx_opt, 0);
    m_index = mm_idx_reader_read(m_index_reader, m_threads);
    if (mm_idx_reader_read(m_index_reader, m_threads) != nullptr) {
        throw std::runtime_error(
                "Dorado doesn't support split index for alignment. Please re-run with larger index "
                "size.");
    }
    mm_mapopt_update(&m_map_opt, m_index);

    if (m_index->k != m_idx_opt.k || m_index->w != m_idx_opt.w) {
        spdlog::warn(
                "Indexing parameters mismatch prebuilt index: using paramateres kmer "
                "size={} and window size={} from prebuilt index.",
                m_index->k, m_index->w);
    }

    if (mm_verbose >= 3) {
        mm_idx_stat(m_index);
    }

    for (int i = 0; i < m_threads; i++) {
        m_tbufs.push_back(mm_tbuf_init());
    }

    for (size_t i = 0; i < m_threads; i++) {
        m_workers.push_back(
                std::make_unique<std::thread>(std::thread(&Aligner::worker_thread, this, i)));
    }
}

Aligner::~Aligner() {
    terminate();
    for (auto& m : m_workers) {
        m->join();
    }
    for (int i = 0; i < m_threads; i++) {
        mm_tbuf_destroy(m_tbufs[i]);
    }
    mm_idx_reader_close(m_index_reader);
    mm_idx_destroy(m_index);
    // Adding for thread safety in case worker thread throws exception.
    m_sink.terminate();
}

std::vector<std::pair<char*, uint32_t>> Aligner::get_sequence_records_for_header() {
    std::vector<std::pair<char*, uint32_t>> records;
    for (int i = 0; i < m_index->n_seq; ++i) {
        records.push_back(std::make_pair(m_index->seq[i].name, m_index->seq[i].len));
    }
    return records;
}

void Aligner::worker_thread(size_t tid) {
    m_active++;  // Track active threads.

    Message message;
    while (m_work_queue.try_pop(message)) {
        auto read = std::get<BamPtr>(std::move(message));
        auto records = align(read.get(), m_tbufs[tid]);
        for (auto& record : records) {
            m_sink.push_message(std::move(record));
        }
    }

    int num_active = --m_active;
    if (num_active == 0) {
        terminate();
        m_sink.terminate();
    }
}

// Function to add auxiliary tags to the alignment record.
// These are added to maintain parity with mm2.
void Aligner::add_tags(bam1_t* record,
                       const mm_reg1_t* aln,
                       const std::string& seq,
                       const mm_tbuf_t* buf) {
    if (aln->p) {
        // NM
        int32_t nm = aln->blen - aln->mlen + aln->p->n_ambi;
        bam_aux_append(record, "NM", 'i', sizeof(nm), (uint8_t*)&nm);

        // ms
        int32_t ms = aln->p->dp_max;
        bam_aux_append(record, "ms", 'i', sizeof(nm), (uint8_t*)&ms);

        // AS
        int32_t as = aln->p->dp_score;
        bam_aux_append(record, "AS", 'i', sizeof(nm), (uint8_t*)&as);

        // nn
        int32_t nn = aln->p->n_ambi;
        bam_aux_append(record, "nn", 'i', sizeof(nm), (uint8_t*)&nn);

        if (aln->p->trans_strand == 1 || aln->p->trans_strand == 2) {
            bam_aux_append(record, "ts", 'A', 2, (uint8_t*)&("?+-?"[aln->p->trans_strand]));
        }
    }

    // de / dv
    if (aln->p) {
        float div;
        div = 1.0 - mm_event_identity(aln);
        bam_aux_append(record, "de", 'f', sizeof(div), (uint8_t*)&div);
    } else if (aln->div >= 0.0f && aln->div <= 1.0f) {
        bam_aux_append(record, "dv", 'f', sizeof(aln->div), (uint8_t*)&aln->div);
    }

    // tp
    char type;
    if (aln->id == aln->parent) {
        type = aln->inv ? 'I' : 'P';
    } else {
        type = aln->inv ? 'i' : 'S';
    }
    bam_aux_append(record, "tp", 'A', sizeof(type), (uint8_t*)&type);

    // cm
    bam_aux_append(record, "cm", 'i', sizeof(aln->cnt), (uint8_t*)&aln->cnt);

    // s1
    bam_aux_append(record, "s1", 'i', sizeof(aln->score), (uint8_t*)&aln->score);

    // s2
    if (aln->parent == aln->id) {
        bam_aux_append(record, "s2", 'i', sizeof(aln->subsc), (uint8_t*)&aln->subsc);
    }

    // MD
    char* md = NULL;
    int max_len = 0;
    int md_len = mm_gen_MD(NULL, &md, &max_len, m_index, aln, seq.c_str());
    if (md_len > 0) {
        bam_aux_append(record, "MD", 'Z', md_len + 1, (uint8_t*)md);
    }
    free(md);

    // zd
    if (aln->split) {
        uint32_t split = uint32_t(aln->split);
        bam_aux_append(record, "zd", 'i', sizeof(split), (uint8_t*)&split);
    }

    // rl
    bam_aux_append(record, "rl", 'i', sizeof(buf->rep_len), (uint8_t*)&buf->rep_len);
}

std::vector<BamPtr> Aligner::align(bam1_t* irecord, mm_tbuf_t* buf) {
    // some where for the hits
    std::vector<BamPtr> results;

    // get the sequence to map from the record
    auto seqlen = irecord->core.l_qseq;

    // get query name.
    std::string_view qname(bam_get_qname(irecord));

    auto bseq = bam_get_seq(irecord);
    std::string seq = convert_nt16_to_str(bseq, seqlen);
    // Pre-generate reverse complement sequence.
    std::string seq_rev = reverse_complement(seq);

    // Pre-generate reverse of quality string.
    std::vector<uint8_t> qual;
    std::vector<uint8_t> qual_rev;
    if (bam_get_qual(irecord)) {
        qual = std::vector<uint8_t>(bam_get_qual(irecord), bam_get_qual(irecord) + seqlen);
        qual_rev = std::vector<uint8_t>(qual.rbegin(), qual.rend());
    }

    // do the mapping
    int hits = 0;
    mm_reg1_t* reg =
            mm_map(m_index, seq.length(), seq.c_str(), &hits, buf, &m_map_opt, qname.data());

    // just return the input record
    if (hits == 0) {
        results.push_back(BamPtr(bam_dup1(irecord)));
    }

    for (int j = 0; j < hits; j++) {
        // new output record
        bam1_t* record = bam_init1();

        // mapping region
        auto aln = &reg[j];

        // Set FLAGS
        uint16_t flag = 0x0;

        if (aln->rev) {
            flag |= BAM_FREVERSE;
        }
        if (aln->parent != aln->id) {
            flag |= BAM_FSECONDARY;
        } else if (!aln->sam_pri) {
            flag |= BAM_FSUPPLEMENTARY;
        }

        int32_t tid = aln->rid;
        hts_pos_t pos = aln->rs;
        uint8_t mapq = aln->mapq;

        // Create CIGAR.
        // Note: max_bam_cigar_op doesn't need to handled specially when
        // using htslib since the sam_write1 method already takes care
        // of moving the CIGAR string to the tags if the length
        // exceeds 65535.
        size_t n_cigar = aln->p ? aln->p->n_cigar : 0;
        std::vector<uint32_t> cigar;
        if (n_cigar != 0) {
            uint32_t clip_len[2] = {0};
            clip_len[0] = aln->rev ? irecord->core.l_qseq - aln->qe : aln->qs;
            clip_len[1] = aln->rev ? aln->qs : irecord->core.l_qseq - aln->qe;

            if (clip_len[0]) {
                n_cigar++;
            }
            if (clip_len[1]) {
                n_cigar++;
            }
            int offset = clip_len[0] ? 1 : 0;

            cigar.resize(n_cigar);

            // write the left softclip
            if (clip_len[0]) {
                auto clip = bam_cigar_gen(clip_len[0], BAM_CSOFT_CLIP);
                cigar[0] = clip;
            }

            // write the cigar
            memcpy(&cigar[offset], aln->p->cigar, aln->p->n_cigar * sizeof(uint32_t));

            // write the right softclip
            if (clip_len[1]) {
                auto clip = bam_cigar_gen(clip_len[1], BAM_CSOFT_CLIP);
                cigar[offset + aln->p->n_cigar] = clip;
            }
        }

        // Add SEQ and QUAL.
        size_t l_seq = 0;
        char* seq_tmp = nullptr;
        unsigned char* qual_tmp = nullptr;
        if (flag & BAM_FSECONDARY) {
            // To match minimap2 output behavior, don't emit sequence
            // or quality info for secondary alignments.
        } else {
            l_seq = seq.size();
            if (aln->rev) {
                seq_tmp = seq_rev.data();
                qual_tmp = qual_rev.empty() ? nullptr : qual_rev.data();
            } else {
                seq_tmp = seq.data();
                qual_tmp = qual.empty() ? nullptr : qual.data();
            }
        }

        // Set properties of the BAM record.
        // NOTE: Passing bam_get_qname(irecord) + l_qname into bam_set1
        // was causing the generated string to have some extra
        // null characters. Not sure why yet. Using string_view
        // resolved that issue, which is okay to use since it doesn't
        // copy any data and we know the underlying string is null
        // terminated.
        // TODO: See if bam_get_qname(irecord) usage can be fixed.
        bam_set1(record, qname.size(), qname.data(), flag, tid, pos, mapq, n_cigar,
                 cigar.empty() ? nullptr : cigar.data(), irecord->core.mtid, irecord->core.mpos,
                 irecord->core.isize, l_seq, seq_tmp, (char*)qual_tmp, bam_get_l_aux(irecord));

        // Copy over tags from input alignment.
        memcpy(bam_get_aux(record), bam_get_aux(irecord), bam_get_l_aux(irecord));
        record->l_data += bam_get_l_aux(irecord);

        // Add new tags to match minimap2.
        add_tags(record, aln, seq, buf);

        free(aln->p);
        results.push_back(BamPtr(record));
    }

    free(reg);
    return results;
}

HtsReader::HtsReader(const std::string& filename) {
    m_file = hts_open(filename.c_str(), "r");
    if (!m_file) {
        throw std::runtime_error("Could not open file: " + filename);
    }
    format = hts_format_description(hts_get_format(m_file));
    header = sam_hdr_read(m_file);
    if (!header) {
        throw std::runtime_error("Could not read header from file: " + filename);
    }
    is_aligned = header->n_targets > 0;
    record.reset(bam_init1());
}

HtsReader::~HtsReader() {
    hts_free(format);
    sam_hdr_destroy(header);
    record.reset();
    hts_close(m_file);
}

bool HtsReader::read() { return sam_read1(m_file, header, record.get()) >= 0; }

bool HtsReader::has_tag(std::string tagname) {
    uint8_t* tag = bam_aux_get(record.get(), tagname.c_str());
    return static_cast<bool>(tag);
}

void HtsReader::read(MessageSink& read_sink, int max_reads) {
    int num_reads = 0;
    while (this->read()) {
        read_sink.push_message(BamPtr(bam_dup1(record.get())));
        if (++num_reads >= max_reads) {
            break;
        }
        if (num_reads % 50000 == 0) {
            spdlog::debug("Processed {} reads", num_reads);
        }
    }
    spdlog::debug("Total reads processed: {}", num_reads);
    read_sink.terminate();
}

HtsWriter::HtsWriter(const std::string& filename, OutputMode mode, size_t threads, size_t num_reads)
        : MessageSink(10000), m_num_reads_expected(num_reads) {
    switch (mode) {
    case FASTQ:
        m_file = hts_open(filename.c_str(), "wf");
        break;
    case BAM:
        m_file = hts_open(filename.c_str(), "wb");
        break;
    case SAM:
        m_file = hts_open(filename.c_str(), "w");
        break;
    case UBAM:
        m_file = hts_open(filename.c_str(), "wb0");
        break;
    default:
        throw std::runtime_error("Unknown output mode selected: " + std::to_string(mode));
    }
    if (!m_file) {
        throw std::runtime_error("Could not open file: " + filename);
    }
    if (m_file->format.compression == bgzf) {
        auto res = bgzf_mt(m_file->fp.bgzf, threads, 128);
        if (res < 0) {
            throw std::runtime_error("Could not enable multi threading for BAM generation.");
        }
    }

    if (m_num_reads_expected == 0) {
        m_progress_bar_interval = 100;
    } else {
        m_progress_bar_interval = m_num_reads_expected < 100 ? 1 : 100;
    }

    m_worker = std::make_unique<std::thread>(std::thread(&HtsWriter::worker_thread, this));
}

HtsWriter::~HtsWriter() {
    // Adding for thread safety in case worker thread throws exception.
    terminate();
    if (m_worker->joinable()) {
        join();
    }
    sam_hdr_destroy(header);
    hts_close(m_file);
}

HtsWriter::OutputMode HtsWriter::get_output_mode(std::string mode) {
    if (mode == "sam") {
        return SAM;
    } else if (mode == "bam") {
        return BAM;
    } else if (mode == "fastq") {
        return FASTQ;
    }
    throw std::runtime_error("Unknown output mode: " + mode);
}

void HtsWriter::join() { m_worker->join(); }

void HtsWriter::worker_thread() {
    std::unordered_set<std::string> processed_read_ids;
    size_t write_count = 0;

    // Initialize progress logging.
    if (m_num_reads_expected == 0) {
        std::cerr << "\r> Output records written: " << write_count;
    }

    Message message;
    while (m_work_queue.try_pop(message)) {
        auto aln = std::get<BamPtr>(std::move(message));
        write(aln.get());
        std::string read_id = bam_get_qname(aln.get());
        aln.reset();  // Free the bam alignment that's already written

        // For the purpose of estimating write count, we ignore duplex reads
        // these can be identified by a semicolon in their ID.
        // TODO: This is a hack, we should have a better way of identifying duplex reads.
        bool ignore_read_id = read_id.find(';') != std::string::npos;

        if (!ignore_read_id) {
            processed_read_ids.emplace(std::move(read_id));
        }

        if (m_num_reads_expected != 0) {
            write_count = processed_read_ids.size();
        } else {
            if (!ignore_read_id) {
                write_count++;
            }
        }

        if ((write_count % m_progress_bar_interval) == 0) {
            if ((write_count == 0) && !m_prog_bar_initialized) {
                m_progress_bar.set_progress(0.0f);
                m_prog_bar_initialized = true;
            }
            if (m_num_reads_expected != 0) {
                float progress = 100.f * static_cast<float>(write_count) / m_num_reads_expected;
                m_progress_bar.set_progress(progress);
                std::cerr << "\033[K";
            } else {
                std::cerr << "\r> Output records written: " << write_count;
            }
        }
    }
    // Clear progress information.
    if (m_num_reads_expected != 0 || write_count >= m_progress_bar_interval) {
        std::cerr << "\r";
    }
    spdlog::debug("Written {} records.", write_count);
}

int HtsWriter::write(bam1_t* record) {
    // track stats
    total++;
    if (record->core.flag & BAM_FUNMAP) {
        unmapped++;
    }
    if (record->core.flag & BAM_FSECONDARY) {
        secondary++;
    }
    if (record->core.flag & BAM_FSUPPLEMENTARY) {
        supplementary++;
    }
    primary = total - secondary - supplementary - unmapped;

    auto res = sam_write1(m_file, header, record);
    if (res < 0) {
        throw std::runtime_error("Failed to write SAM record, error code " + std::to_string(res));
    }
    return res;
}

void HtsWriter::add_header(const sam_hdr_t* hdr) { header = sam_hdr_dup(hdr); }

int HtsWriter::write_header() {
    if (header) {
        return sam_hdr_write(m_file, header);
    }
    return 0;
}

read_map read_bam(const std::string& filename, const std::unordered_set<std::string>& read_ids) {
    HtsReader reader(filename);

    read_map reads;

    while (reader.read()) {
        std::string read_id = bam_get_qname(reader.record);

        if (read_ids.find(read_id) == read_ids.end()) {
            continue;
        }

        uint8_t* qstring = bam_get_qual(reader.record);
        uint8_t* sequence = bam_get_seq(reader.record);

        uint32_t seqlen = reader.record->core.l_qseq;
        std::vector<uint8_t> qualities(seqlen);
        std::vector<char> nucleotides(seqlen);

        // Todo - there is a better way to do this.
        for (int i = 0; i < seqlen; i++) {
            qualities[i] = qstring[i] + 33;
            nucleotides[i] = seq_nt16_str[bam_seqi(sequence, i)];
        }

        auto tmp_read = std::make_shared<Read>();
        tmp_read->read_id = read_id;
        tmp_read->seq = std::string(nucleotides.begin(), nucleotides.end());
        tmp_read->qstring = std::string(qualities.begin(), qualities.end());
        reads[read_id] = tmp_read;
    }

    return reads;
}

void add_rg_hdr(sam_hdr_t* hdr, const std::unordered_map<std::string, ReadGroup>& read_groups) {
    // Add read groups
    for (auto const& x : read_groups) {
        std::stringstream rg;
        rg << "@RG\t";
        rg << "ID:" << x.first << "\t";
        rg << "PU:" << x.second.flowcell_id << "\t";
        rg << "PM:" << x.second.device_id << "\t";
        rg << "DT:" << x.second.exp_start_time << "\t";
        rg << "PL:"
           << "ONT"
           << "\t";
        rg << "DS:"
           << "basecall_model=" << x.second.basecalling_model << " runid=" << x.second.run_id
           << "\t";
        rg << "LB:" << x.second.sample_id << "\t";
        rg << "SM:" << x.second.sample_id;
        rg << std::endl;
        sam_hdr_add_lines(hdr, rg.str().c_str(), 0);
    }
}

void add_sq_hdr(sam_hdr_t* hdr, const sq_t& seqs) {
    for (auto pair : seqs) {
        char* name;
        int length;
        std::tie(name, length) = pair;
        sam_hdr_add_line(hdr, "SQ", "SN", name, "LN", std::to_string(length).c_str(), NULL);
    }
}

std::map<std::string, std::string> get_read_group_info(sam_hdr_t* header, const char* key) {
    if (header == nullptr) {
        throw std::invalid_argument("header cannot be nullptr");
    }

    int num_read_groups = sam_hdr_count_lines(header, "RG");
    if (num_read_groups == -1) {
        throw std::runtime_error("no read groups in file");
    }

    kstring_t rg = {0, 0, NULL};
    std::map<std::string, std::string> read_group_info;

    for (int i = 0; i < num_read_groups; ++i) {
        const char* id = sam_hdr_line_name(header, "RG", i);
        if (id == nullptr) {
            continue;
        }

        std::string read_group_id(id);
        int res = sam_hdr_find_tag_id(header, "RG", "ID", id, key, &rg);
        if (res == 0 && rg.l > 0) {
            read_group_info[read_group_id] = std::string(rg.s, rg.l);
        }
    }

    free(rg.s);
    return read_group_info;
}

AlignmentOps get_alignment_op_counts(bam1_t* record) {
    AlignmentOps counts = {};

    uint32_t* cigar = bam_get_cigar(record);
    int n_cigar = record->core.n_cigar;

    if (bam_cigar_op(cigar[0]) == BAM_CSOFT_CLIP) {
        counts.softclip_start = bam_cigar_oplen(cigar[0]);
    }

    if (bam_cigar_op(cigar[n_cigar - 1]) == BAM_CSOFT_CLIP) {
        counts.softclip_end = bam_cigar_oplen(cigar[n_cigar - 1]);
    }

    for (int i = 0; i < n_cigar; ++i) {
        int op = bam_cigar_op(cigar[i]);
        int op_len = bam_cigar_oplen(cigar[i]);

        switch (op) {
        case BAM_CMATCH:
            counts.matches += op_len;
            break;
        case BAM_CINS:
            counts.insertions += op_len;
            break;
        case BAM_CDEL:
            counts.deletions += op_len;
            break;
        default:
            break;
        }
    }

    uint8_t* md_ptr = bam_aux_get(record, "MD");

    if (md_ptr) {
        int i = 0;
        int md_length = 0;
        char* md = bam_aux2Z(md_ptr);

        while (md[i]) {
            if (std::isdigit(md[i])) {
                md_length = md_length * 10 + (md[i] - '0');
            } else {
                if (md[i] == '^') {
                    // Skip deletions
                    i++;
                    while (md[i] && !std::isdigit(md[i])) {
                        i++;
                    }
                } else {
                    // Substitution found
                    counts.substitutions++;
                    md_length++;
                }
            }
            i++;
        }
    }

    return counts;
}

}  // namespace dorado::utils
