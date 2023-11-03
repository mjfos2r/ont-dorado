#include "TestUtils.h"
#include "read_pipeline/HtsReader.h"
#include "utils/bam_utils.h"
#include "utils/barcode_kits.h"

#include <catch2/catch.hpp>
#include <htslib/sam.h>

#include <filesystem>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#define TEST_GROUP "[bam_utils]"

namespace fs = std::filesystem;
using namespace dorado;

namespace {
class WrappedKString {
    kstring_t m_str = KS_INITIALIZE;

public:
    WrappedKString() {
        // On Windows |sam_hdr_find_tag_id| lives in a DLL and uses a different heap, but
        // |ks_free| is inline so when we call it we crash trying to free unknown memory. To
        // work around this we resize the kstring to a big value in our code so no resizing
        // happens inside the htslib library.
        ks_resize(&m_str, 1e6);
    }
    ~WrappedKString() { ks_free(&m_str); }

    kstring_t *get() { return &m_str; }
};
}  // namespace

TEST_CASE("BamUtilsTest: fetch keys from PG header", TEST_GROUP) {
    fs::path aligner_test_dir = fs::path(get_data_dir("aligner_test"));
    auto sam = aligner_test_dir / "basecall.sam";

    auto keys = utils::extract_pg_keys_from_hdr(sam.string(), {"PN", "CL", "VN"});
    CHECK(keys["PN"] == "dorado");
    CHECK(keys["VN"] == "0.2.3+0f041c4+dirty");
    CHECK(keys["CL"] ==
          "dorado basecaller dna_r9.4.1_e8_hac@v3.3 ./tests/data/pod5 -x cpu --modified-bases "
          "5mCG");
}

TEST_CASE("BamUtilsTest: add_rg_hdr read group headers", TEST_GROUP) {
    auto has_read_group_header = [](sam_hdr_t *ptr, const char *id) {
        return sam_hdr_line_index(ptr, "RG", id) >= 0;
    };
    WrappedKString barcode_kstring;
    auto get_barcode_tag = [&barcode_kstring](sam_hdr_t *ptr,
                                              const char *id) -> std::optional<std::string> {
        if (sam_hdr_find_tag_id(ptr, "RG", "ID", id, "BC", barcode_kstring.get()) != 0) {
            return std::nullopt;
        }
        std::string tag(ks_str(barcode_kstring.get()), ks_len(barcode_kstring.get()));
        return tag;
    };

    SECTION("No read groups generate no headers") {
        dorado::SamHdrPtr sam_header(sam_hdr_init());
        CHECK(sam_hdr_count_lines(sam_header.get(), "RG") == 0);
        dorado::utils::add_rg_hdr(sam_header.get(), {}, {}, nullptr);
        CHECK(sam_hdr_count_lines(sam_header.get(), "RG") == 0);
    }

    const std::unordered_map<std::string, dorado::ReadGroup> read_groups{
            {"id_0",
             {"run_0", "basecalling_mod_0", "flowcell_0", "device_0", "exp_start_0", "sample_0"}},
            {"id_1",
             {"run_1", "basecalling_mod_1", "flowcell_1", "device_1", "exp_start_1", "sample_1"}},
    };

    SECTION("Read groups") {
        dorado::SamHdrPtr sam_header(sam_hdr_init());
        dorado::utils::add_rg_hdr(sam_header.get(), read_groups, {}, nullptr);

        // Check the IDs of the groups are all there.
        CHECK(sam_hdr_count_lines(sam_header.get(), "RG") == read_groups.size());
        for (auto &&[id, read_group] : read_groups) {
            CHECK(has_read_group_header(sam_header.get(), id.c_str()));
            // None of the read groups should have a barcode.
            CHECK(get_barcode_tag(sam_header.get(), id.c_str()) == std::nullopt);
        }
    }

    // Pick some of the barcode kits (randomly chosen indices).
    const auto &kit_infos = dorado::barcode_kits::get_kit_infos();
    const std::vector<std::string> barcode_kits{
            std::next(kit_infos.begin(), 1)->first,
            std::next(kit_infos.begin(), 7)->first,
    };

    SECTION("Read groups with barcodes") {
        dorado::SamHdrPtr sam_header(sam_hdr_init());
        dorado::utils::add_rg_hdr(sam_header.get(), read_groups, barcode_kits, nullptr);

        // Check the IDs of the groups are all there.
        size_t total_barcodes = 0;
        for (const auto &kit_name : barcode_kits) {
            total_barcodes += kit_infos.at(kit_name).barcodes.size();
        }
        const size_t total_groups = read_groups.size() * (total_barcodes + 1);
        CHECK(sam_hdr_count_lines(sam_header.get(), "RG") == total_groups);

        // Check that the IDs match the expected format.
        const auto &barcode_seqs = dorado::barcode_kits::get_barcodes();
        for (auto &&[id, read_group] : read_groups) {
            CHECK(has_read_group_header(sam_header.get(), id.c_str()));
            CHECK(get_barcode_tag(sam_header.get(), id.c_str()) == std::nullopt);

            // The headers with barcodes should contain those barcodes.
            for (const auto &kit_name : barcode_kits) {
                const auto &kit_info = kit_infos.at(kit_name);
                for (const auto &barcode_name : kit_info.barcodes) {
                    const auto full_id = id + "_" +
                                         dorado::barcode_kits::generate_standard_barcode_name(
                                                 kit_name, barcode_name);
                    const auto &barcode_seq = barcode_seqs.at(barcode_name);
                    CHECK(has_read_group_header(sam_header.get(), full_id.c_str()));
                    CHECK(get_barcode_tag(sam_header.get(), full_id.c_str()) == barcode_seq);
                }
            }
        }
    }

    SECTION("Read groups with unknown barcode kit") {
        dorado::SamHdrPtr sam_header(sam_hdr_init());
        CHECK_THROWS(dorado::utils::add_rg_hdr(sam_header.get(), read_groups, {"blah"}, nullptr));
    }
}

TEST_CASE("BamUtilsTest: Test bam extraction helpers", TEST_GROUP) {
    fs::path bam_utils_test_dir = fs::path(get_data_dir("bam_utils"));
    auto sam = bam_utils_test_dir / "test.sam";

    HtsReader reader(sam.string());
    REQUIRE(reader.read());  // Parse first and only record.
    auto record = reader.record.get();

    SECTION("Test sequence extraction") {
        std::string seq = utils::extract_sequence(record);
        CHECK(seq ==
              "AATAAACCGAAGACAATTTAGAAGCCAGCGAGGTATGTGCGTCTACTTCGTTCGGTTATGCGAAGCCGATATAACCTGCAGGAC"
              "AACACAACATTTCCACTGTTTTCGTTCATTCGTAAACGCTTTCGCGTTCATCACACTCAACCATAGGCTTTAGCCAGAACGTTA"
              "TGAACCCCAGCGACTTCCAGAACGGCGCGCGTGCCACCACCGGCGATGATACCGGTTCCTTCGGAAGCCGGCTGCATGAATACG"
              "CGAGAACCCGTGTGAACACCTTTAACAGGGTGTTGCAGAGTGCCGTTGCTGCGGCACGATAGTTAAGTCGTATTGCTGAAGCGA"
              "CACTGTCCATCGCTTTCTGGATGGCT");
    }

    SECTION("Test quality extraction") {
        const std::string qual =
                "%$%&%$####%'%%$&'(1/...022.+%%%%%%$$%%&%$%%%&&+)()./"
                "0%$$'&'&'%$###$&&&'*(()()%%%%(%%'))(('''3222276<BAAABE:+''&)**%(/"
                "''(:322**(*,,++&+++/1)(&&(006=B??@AKLK=<==HHHHHFFCBB@??>==943323/-.'56::71.//"
                "0933))%&%&))*1739:666455116/"
                "0,(%%&(*-55EBEB>@;??>>@BBDC?><<98-,,BGHEGFFGIIJFFDBB;6AJ>===KB:::<70/"
                "..--,++,))+*)&&'*-,+*)))(%%&'&''%%%$&%$###$%%$$%'%%$$+1.--.7969....*)))";
        auto qual_vector = utils::extract_quality(record);
        CHECK(qual_vector.size() == qual.length());
        for (int i = 0; i < qual.length(); i++) {
            CHECK(qual[i] == qual_vector[i] + 33);
        }
    }

    SECTION("Test move table extraction") {
        auto [stride, move_table] = utils::extract_move_table(record);
        int seqlen = record->core.l_qseq;
        REQUIRE(!move_table.empty());
        CHECK(stride == 6);
        CHECK(seqlen == std::accumulate(move_table.begin(), move_table.end(), 0));
    }

    SECTION("Test mod base info extraction") {
        auto [modbase_str, modbase_probs] = utils::extract_modbase_info(record);
        const std::vector<int8_t> expected_modbase_probs = {5, 1};
        CHECK(modbase_str == "C+h?,1;C+m?,1;");
        CHECK(modbase_probs.size() == expected_modbase_probs.size());
        for (int i = 0; i < expected_modbase_probs.size(); i++) {
            CHECK(modbase_probs[i] == expected_modbase_probs[i]);
        }
    }
}

TEST_CASE("BamUtilsTest: cigar2str utility", TEST_GROUP) {
    const std::string cigar = "12S17M1D296M2D21M1D3M2D10M1I320M1D2237M41S";
    size_t m = 0;
    uint32_t *a_cigar = NULL;
    char *end;
    int n_cigar = sam_parse_cigar(cigar.c_str(), &end, &a_cigar, &m);
    std::string converted_str = utils::cigar2str(n_cigar, a_cigar);
    CHECK(cigar == converted_str);
}

TEST_CASE("BamUtilsTest: Test trim CIGAR", TEST_GROUP) {
    const std::string cigar = "12S17M1D296M2D21M1D3M2D10M1I320M1D2237M41S";
    size_t m = 0;
    uint32_t *a_cigar = NULL;
    char *end;
    int n_cigar = sam_parse_cigar(cigar.c_str(), &end, &a_cigar, &m);
    const uint32_t qlen = bam_cigar2qlen(n_cigar, a_cigar);

    SECTION("Trim nothing") {
        auto ops = utils::trim_cigar(n_cigar, a_cigar, {0, qlen});
        std::string converted_str = utils::cigar2str(ops.size(), ops.data());
        CHECK(converted_str == "12S17M1D296M2D21M1D3M2D10M1I320M1D2237M41S");
    }

    SECTION("Trim from first op") {
        auto ops = utils::trim_cigar(n_cigar, a_cigar, {1, qlen});
        std::string converted_str = utils::cigar2str(ops.size(), ops.data());
        CHECK(converted_str == "11S17M1D296M2D21M1D3M2D10M1I320M1D2237M41S");
    }

    SECTION("Trim entire first op") {
        auto ops = utils::trim_cigar(n_cigar, a_cigar, {12, qlen});
        std::string converted_str = utils::cigar2str(ops.size(), ops.data());
        CHECK(converted_str == "17M1D296M2D21M1D3M2D10M1I320M1D2237M41S");
    }

    SECTION("Trim several ops from the front") {
        auto ops = utils::trim_cigar(n_cigar, a_cigar, {29, qlen});
        std::string converted_str = utils::cigar2str(ops.size(), ops.data());
        CHECK(converted_str == "296M2D21M1D3M2D10M1I320M1D2237M41S");
    }

    SECTION("Trim from last op") {
        auto ops = utils::trim_cigar(n_cigar, a_cigar, {0, qlen - 20});
        std::string converted_str = utils::cigar2str(ops.size(), ops.data());
        CHECK(converted_str == "12S17M1D296M2D21M1D3M2D10M1I320M1D2237M21S");
    }

    SECTION("Trim entire last op") {
        auto ops = utils::trim_cigar(n_cigar, a_cigar, {0, qlen - 41});
        std::string converted_str = utils::cigar2str(ops.size(), ops.data());
        CHECK(converted_str == "12S17M1D296M2D21M1D3M2D10M1I320M1D2237M");
    }

    SECTION("Trim several ops from the end") {
        auto ops = utils::trim_cigar(n_cigar, a_cigar, {0, qlen - 2278});
        std::string converted_str = utils::cigar2str(ops.size(), ops.data());
        CHECK(converted_str == "12S17M1D296M2D21M1D3M2D10M1I320M");
    }

    SECTION("Trim from the middle") {
        auto ops = utils::trim_cigar(n_cigar, a_cigar, {29, qlen - 2278});
        std::string converted_str = utils::cigar2str(ops.size(), ops.data());
        CHECK(converted_str == "296M2D21M1D3M2D10M1I320M");
    }

    free(a_cigar);
}

TEST_CASE("BamUtilsTest: Ref positions consumed", TEST_GROUP) {
    const std::string cigar = "12S17M1D296M2D21M1D3M2D10M1I320M1D2237M41S";
    size_t m = 0;
    uint32_t *a_cigar = NULL;
    char *end;
    int n_cigar = sam_parse_cigar(cigar.c_str(), &end, &a_cigar, &m);
    const uint32_t qlen = bam_cigar2qlen(n_cigar, a_cigar);

    SECTION("No positions consumed") {
        auto pos_consumed = utils::ref_pos_consumed(n_cigar, a_cigar, 0);
        CHECK(pos_consumed == 0);
    }

    SECTION("No positions consumed with soft clipping") {
        auto pos_consumed = utils::ref_pos_consumed(n_cigar, a_cigar, 12);
        CHECK(pos_consumed == 0);
    }

    SECTION("Match positions consumed") {
        auto pos_consumed = utils::ref_pos_consumed(n_cigar, a_cigar, 25);
        CHECK(pos_consumed == 13);
    }

    SECTION("Match and delete positions consumed") {
        auto pos_consumed = utils::ref_pos_consumed(n_cigar, a_cigar, 29);
        CHECK(pos_consumed == 18);
    }
}
