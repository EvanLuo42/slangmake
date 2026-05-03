// End-to-end smoke tests for the C ABI layer in include/slangmake.h.
// Exercises compile, blob round-trip, raw reflection tables, and batch
// compile strictly through the C entry points, so a future ABI break in the
// wrapper (signature, layout, or symbol export) shows up here.

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "slangmake.h"

namespace fs = std::filesystem;

namespace
{
fs::path shader(const char* name) { return fs::path(SLANG_MAKE_TESTS_SHADER_DIR) / name; }
} // namespace

TEST_CASE("C ABI: enum string round-trip")
{
    sm_target_t t;
    REQUIRE(sm_target_from_string("SPIRV", &t) == SM_OK);
    CHECK(t == SM_TARGET_SPIRV);
    CHECK(std::string(sm_target_to_string(t)) == "SPIRV");

    REQUIRE(sm_target_from_string("spv", &t) == SM_OK);
    CHECK(t == SM_TARGET_SPIRV);

    sm_codec_t c;
    REQUIRE(sm_codec_from_string("zstd", &c) == SM_OK);
    CHECK(c == SM_CODEC_ZSTD);
    CHECK(std::string(sm_codec_to_string(SM_CODEC_LZ4)) == "lz4");

    CHECK(sm_target_from_string("nonsense", &t) == SM_ERR_NOT_FOUND);
}

TEST_CASE("C ABI: compile compute.slang to SPIR-V")
{
    auto* opts = sm_compile_options_create();
    REQUIRE(opts);
    sm_compile_options_set_input_file(opts, shader("compute.slang").string().c_str());
    sm_compile_options_set_target(opts, SM_TARGET_SPIRV);
    sm_compile_options_set_profile(opts, "sm_6_5");

    auto* perm = sm_permutation_create();
    REQUIRE(perm);

    auto* compiler = sm_compiler_create();
    REQUIRE(compiler);

    auto* result = sm_compiler_compile(compiler, opts, perm);
    REQUIRE(result);
    INFO("diagnostics: " << sm_compile_result_diagnostics(result));
    REQUIRE(sm_compile_result_success(result) == 1);

    size_t code_size = 0;
    auto*  code      = sm_compile_result_code(result, &code_size);
    REQUIRE(code != nullptr);
    REQUIRE(code_size >= 4);
    uint32_t magic = 0;
    std::memcpy(&magic, code, 4);
    CHECK(magic == 0x07230203u);

    size_t refl_size = 0;
    sm_compile_result_reflection(result, &refl_size);
    CHECK(refl_size > 0);

    sm_compile_result_destroy(result);
    sm_compiler_destroy(compiler);
    sm_permutation_destroy(perm);
    sm_compile_options_destroy(opts);
}

TEST_CASE("C ABI: missing entry point reports failure with diagnostics")
{
    auto* opts = sm_compile_options_create();
    sm_compile_options_set_input_file(opts, shader("compute.slang").string().c_str());
    sm_compile_options_set_target(opts, SM_TARGET_SPIRV);
    sm_compile_options_set_profile(opts, "sm_6_5");
    sm_compile_options_set_entry_point(opts, "doesNotExist");

    auto* compiler = sm_compiler_create();
    auto* perm     = sm_permutation_create();
    auto* result   = sm_compiler_compile(compiler, opts, perm);
    REQUIRE(result);
    CHECK(sm_compile_result_success(result) == 0);
    CHECK(std::strlen(sm_compile_result_diagnostics(result)) > 0);

    sm_compile_result_destroy(result);
    sm_compiler_destroy(compiler);
    sm_permutation_destroy(perm);
    sm_compile_options_destroy(opts);
}

TEST_CASE("C ABI: blob writer + reader round-trip")
{
    const uint8_t code_a[] = {0x01, 0x02, 0x03, 0x04};
    const uint8_t code_b[] = {0x10, 0x20, 0x30, 0x40};

    auto* perm_a = sm_permutation_create();
    sm_permutation_add_constant(perm_a, "USE_SHADOW", "0");
    auto* perm_b = sm_permutation_create();
    sm_permutation_add_constant(perm_b, "USE_SHADOW", "1");

    CHECK(std::string(sm_permutation_key(perm_a)) == "USE_SHADOW=0");

    auto* writer = sm_blob_writer_create(SM_TARGET_SPIRV);
    sm_blob_writer_set_options_hash(writer, 0xdeadbeefcafebabeULL);
    sm_blob_writer_add_entry(writer, perm_a, code_a, sizeof(code_a), nullptr, 0, nullptr, 0);
    sm_blob_writer_add_entry(writer, perm_b, code_b, sizeof(code_b), nullptr, 0, nullptr, 0);
    CHECK(sm_blob_writer_entry_count(writer) == 2);

    auto* buf = sm_blob_writer_finalize(writer);
    REQUIRE(buf);
    REQUIRE(sm_buffer_size(buf) > 0);

    auto* reader = sm_blob_reader_open_borrowed(sm_buffer_data(buf), sm_buffer_size(buf));
    REQUIRE(reader);
    CHECK(sm_blob_reader_valid(reader) == 1);
    CHECK(sm_blob_reader_target(reader) == SM_TARGET_SPIRV);
    CHECK(sm_blob_reader_options_hash(reader) == 0xdeadbeefcafebabeULL);
    CHECK(sm_blob_reader_entry_count(reader) == 2);

    sm_blob_entry_t entry{};
    REQUIRE(sm_blob_reader_at(reader, 0, &entry) == SM_OK);
    CHECK(std::string_view(entry.key, entry.key_size) == "USE_SHADOW=0");
    REQUIRE(entry.code_size == sizeof(code_a));
    CHECK(std::memcmp(entry.code, code_a, sizeof(code_a)) == 0);

    REQUIRE(sm_blob_reader_find(reader, perm_b, &entry) == SM_OK);
    CHECK(std::string_view(entry.key, entry.key_size) == "USE_SHADOW=1");
    CHECK(std::memcmp(entry.code, code_b, sizeof(code_b)) == 0);

    sm_blob_reader_destroy(reader);
    sm_buffer_destroy(buf);
    sm_blob_writer_destroy(writer);
    sm_permutation_destroy(perm_b);
    sm_permutation_destroy(perm_a);
}

TEST_CASE("C ABI: reflection view exposes raw tables")
{
    auto* opts = sm_compile_options_create();
    sm_compile_options_set_input_file(opts, shader("with_resources.slang").string().c_str());
    sm_compile_options_set_target(opts, SM_TARGET_SPIRV);
    sm_compile_options_set_profile(opts, "sm_6_5");
    sm_compile_options_set_entry_point(opts, "csMain");

    auto* compiler = sm_compiler_create();
    auto* perm     = sm_permutation_create();
    auto* result   = sm_compiler_compile(compiler, opts, perm);
    REQUIRE(result);
    REQUIRE(sm_compile_result_success(result) == 1);

    size_t refl_size = 0;
    auto*  refl      = sm_compile_result_reflection(result, &refl_size);
    REQUIRE(refl_size > 0);

    auto* view = sm_reflection_open(refl, refl_size);
    REQUIRE(view);
    CHECK(sm_reflection_valid(view) == 1);

    size_t ep_count = 0;
    auto*  eps      = sm_reflection_entry_points(view, &ep_count);
    REQUIRE(ep_count >= 1);
    REQUIRE(eps != nullptr);
    CHECK(std::strlen(sm_reflection_string(view, eps[0].name_str_idx)) > 0);

    size_t var_count = 0;
    sm_reflection_variables(view, &var_count);
    CHECK(var_count > 0);

    size_t var_layout_count = 0;
    sm_reflection_var_layouts(view, &var_layout_count);
    CHECK(var_layout_count > 0);

    sm_reflection_destroy(view);
    sm_compile_result_destroy(result);
    sm_compiler_destroy(compiler);
    sm_permutation_destroy(perm);
    sm_compile_options_destroy(opts);
}

TEST_CASE("C ABI: permutation parser + expander walk a file")
{
    auto* defs = sm_parse_permutations_file(shader("permuted.slang").string().c_str());
    REQUIRE(defs);
    CHECK(sm_perm_define_list_size(defs) == 2);

    bool seen_use_shadow = false;
    bool seen_quality    = false;
    for (size_t i = 0; i < sm_perm_define_list_size(defs); ++i)
    {
        std::string name = sm_perm_define_list_name(defs, i);
        if (name == "USE_SHADOW")
            seen_use_shadow = (sm_perm_define_list_value_count(defs, i) == 2);
        else if (name == "QUALITY")
            seen_quality = (sm_perm_define_list_value_count(defs, i) == 3);
    }
    CHECK(seen_use_shadow);
    CHECK(seen_quality);

    auto* perms = sm_expand_permutations(defs);
    REQUIRE(perms);
    CHECK(sm_permutation_list_size(perms) == 2 * 3);

    sm_permutation_list_destroy(perms);
    sm_perm_define_list_destroy(defs);
}

TEST_CASE("C ABI: batch compile permuted.slang and verify incremental reuse")
{
    auto out_dir = fs::temp_directory_path() / "slangmake_capi_test";
    fs::create_directories(out_dir);
    auto out_path = (out_dir / "permuted.bin").string();

    auto* opts = sm_compile_options_create();
    sm_compile_options_set_target(opts, SM_TARGET_SPIRV);
    sm_compile_options_set_profile(opts, "sm_6_5");

    auto* compiler = sm_compiler_create();
    auto* batch    = sm_batch_compiler_create(compiler);
    REQUIRE(batch);
    sm_batch_compiler_set_quiet(batch, 1);
    sm_batch_compiler_set_jobs(batch, 1);

    auto* output = sm_batch_compile_file(batch, shader("permuted.slang").string().c_str(), opts, /*cli=*/nullptr,
                                         out_path.c_str());
    REQUIRE(output);
    INFO("failures: " << sm_batch_output_failure_count(output));
    CHECK(sm_batch_output_failure_count(output) == 0);
    CHECK(sm_batch_output_compiled_count(output) == 6);
    CHECK(sm_batch_compiler_last_compiled(batch) == 6);
    CHECK(sm_batch_compiler_last_reused(batch) == 0);

    size_t blob_size = 0;
    auto*  blob      = sm_batch_output_blob(output, &blob_size);
    REQUIRE(blob != nullptr);
    REQUIRE(blob_size > 0);

    sm_batch_output_destroy(output);

    // Second pass should reuse every entry from the on-disk blob.
    output = sm_batch_compile_file(batch, shader("permuted.slang").string().c_str(), opts, nullptr, out_path.c_str());
    REQUIRE(output);
    CHECK(sm_batch_output_failure_count(output) == 0);
    CHECK(sm_batch_compiler_last_reused(batch) == 6);
    CHECK(sm_batch_compiler_last_compiled(batch) == 0);

    sm_batch_output_destroy(output);
    sm_batch_compiler_destroy(batch);
    sm_compiler_destroy(compiler);
    sm_compile_options_destroy(opts);

    fs::remove_all(out_dir);
}
