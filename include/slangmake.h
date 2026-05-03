#ifndef SLANGMAKE_C_H
#define SLANGMAKE_C_H

/*
 * Pure C ABI wrapper around the slangmake C++ API.
 *
 * Designed for FFI consumers (Rust, Python, etc). The C++ surface in
 * slangmake.hpp uses std::vector / std::span / std::filesystem::path / RAII
 * classes that don't cross language boundaries cleanly; this header replaces
 * them with opaque handles, UTF-8 const char* strings, and pointer+size pairs.
 *
 * Lifetime conventions:
 *   - Anything returned as a sm_*_t* is a heap-allocated handle that the
 *     caller must release with the corresponding sm_*_destroy() function.
 *   - const char* / const uint8_t* / span pointers returned from accessors
 *     are owned by the parent handle and remain valid until that handle is
 *     destroyed (or, for builders, until the next mutating call). Copy them
 *     out if you need a longer lifetime.
 *   - Functions that take const char* strings expect NUL-terminated UTF-8.
 *     Pass NULL only where explicitly documented.
 *
 * Error handling:
 *   - Constructors return NULL on failure. Use sm_last_error() to read a
 *     thread-local diagnostic.
 *   - Status-returning calls use sm_status_t; SM_OK == 0 on success.
 *   - Accessors with no error path return zero / empty values on bad input
 *     rather than crashing.
 *
 * ABI stability:
 *   - The fmt:: POD structs in slangmake.hpp are mirrored 1:1 here as
 *     sm_fmt_*_t with #pragma pack(push, 4). Their layout is part of the
 *     on-disk blob format and changes bump kBlobVersion / kReflectionVersion.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(SLANGMAKE_C_STATIC)
#define SLANGMAKE_C_API
#elif defined(SLANGMAKE_C_BUILDING)
#define SLANGMAKE_C_API __declspec(dllexport)
#else
#define SLANGMAKE_C_API __declspec(dllimport)
#endif
#else
#define SLANGMAKE_C_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    /* ----- Status codes --------------------------------------------------- */
    typedef int sm_status_t;
#define SM_OK                 0
#define SM_ERR_INVALID_HANDLE (-1)
#define SM_ERR_INVALID_ARG    (-2)
#define SM_ERR_NOT_FOUND      (-3)
#define SM_ERR_IO             (-4)
#define SM_ERR_INTERNAL       (-5)

    /* Thread-local last error message (UTF-8). Empty string when no error
     * has been recorded on the current thread. */
    SLANGMAKE_C_API const char* sm_last_error(void);

    /* ----- Format constants ---------------------------------------------- */
    /* Mirror of slangmake::fmt:: constants. */
#define SM_BLOB_MAGIC         0x474E4C53u /* 'SLNG' */
#define SM_BLOB_VERSION       1u
#define SM_REFLECTION_MAGIC   0x46524C53u /* 'SLRF' */
#define SM_REFLECTION_VERSION 1u
#define SM_INVALID_INDEX      0xFFFFFFFFu

    /* ----- Enums --------------------------------------------------------- */
    typedef enum sm_target
    {
        SM_TARGET_SPIRV    = 0,
        SM_TARGET_DXIL     = 1,
        SM_TARGET_DXBC     = 2,
        SM_TARGET_HLSL     = 3,
        SM_TARGET_GLSL     = 4,
        SM_TARGET_METAL    = 5,
        SM_TARGET_METALLIB = 6,
        SM_TARGET_WGSL     = 7
    } sm_target_t;

    typedef enum sm_optimization
    {
        SM_OPT_NONE    = 0,
        SM_OPT_DEFAULT = 1,
        SM_OPT_HIGH    = 2,
        SM_OPT_MAXIMAL = 3
    } sm_optimization_t;

    typedef enum sm_codec
    {
        SM_CODEC_NONE = 0,
        SM_CODEC_LZ4  = 1,
        SM_CODEC_ZSTD = 2
    } sm_codec_t;

    typedef enum sm_matrix_layout
    {
        SM_MATRIX_ROW_MAJOR    = 0,
        SM_MATRIX_COLUMN_MAJOR = 1
    } sm_matrix_layout_t;

    typedef enum sm_fp_mode
    {
        SM_FP_DEFAULT = 0,
        SM_FP_FAST    = 1,
        SM_FP_PRECISE = 2
    } sm_fp_mode_t;

    typedef enum sm_perm_define_kind
    {
        SM_PERM_KIND_CONSTANT = 0,
        SM_PERM_KIND_TYPE     = 1
    } sm_perm_define_kind_t;

    /* Enum string helpers. */
    SLANGMAKE_C_API const char* sm_target_to_string(sm_target_t t);
    SLANGMAKE_C_API sm_status_t sm_target_from_string(const char* s, sm_target_t* out);
    SLANGMAKE_C_API const char* sm_codec_to_string(sm_codec_t c);
    SLANGMAKE_C_API sm_status_t sm_codec_from_string(const char* s, sm_codec_t* out);

    /* ----- Opaque handle forward declarations ---------------------------- */
    typedef struct sm_buffer           sm_buffer_t;
    typedef struct sm_compile_options  sm_compile_options_t;
    typedef struct sm_permutation      sm_permutation_t;
    typedef struct sm_perm_define_list sm_perm_define_list_t;
    typedef struct sm_permutation_list sm_permutation_list_t;
    typedef struct sm_compiler         sm_compiler_t;
    typedef struct sm_compile_result   sm_compile_result_t;
    typedef struct sm_blob_writer      sm_blob_writer_t;
    typedef struct sm_blob_reader      sm_blob_reader_t;
    typedef struct sm_reflection       sm_reflection_t;
    typedef struct sm_batch_compiler   sm_batch_compiler_t;
    typedef struct sm_batch_output     sm_batch_output_t;

    /* ----- Owned byte buffer --------------------------------------------- */
    SLANGMAKE_C_API void           sm_buffer_destroy(sm_buffer_t* buf);
    SLANGMAKE_C_API const uint8_t* sm_buffer_data(const sm_buffer_t* buf);
    SLANGMAKE_C_API size_t         sm_buffer_size(const sm_buffer_t* buf);

    /* ----- CompileOptions builder --------------------------------------- */
    SLANGMAKE_C_API sm_compile_options_t* sm_compile_options_create(void);
    SLANGMAKE_C_API void                  sm_compile_options_destroy(sm_compile_options_t* opts);

    SLANGMAKE_C_API void sm_compile_options_set_input_file(sm_compile_options_t* opts, const char* path);
    SLANGMAKE_C_API void sm_compile_options_set_entry_point(sm_compile_options_t* opts, const char* name);
    SLANGMAKE_C_API void sm_compile_options_set_target(sm_compile_options_t* opts, sm_target_t t);
    SLANGMAKE_C_API void sm_compile_options_set_profile(sm_compile_options_t* opts, const char* profile);
    SLANGMAKE_C_API void sm_compile_options_add_include_path(sm_compile_options_t* opts, const char* path);
    SLANGMAKE_C_API void sm_compile_options_add_define(sm_compile_options_t* opts, const char* name, const char* value);
    SLANGMAKE_C_API void sm_compile_options_clear_defines(sm_compile_options_t* opts);
    SLANGMAKE_C_API void sm_compile_options_set_optimization(sm_compile_options_t* opts, sm_optimization_t o);
    SLANGMAKE_C_API void sm_compile_options_set_matrix_layout(sm_compile_options_t* opts, sm_matrix_layout_t l);
    SLANGMAKE_C_API void sm_compile_options_set_fp_mode(sm_compile_options_t* opts, sm_fp_mode_t m);
    SLANGMAKE_C_API void sm_compile_options_set_debug_info(sm_compile_options_t* opts, int v);
    SLANGMAKE_C_API void sm_compile_options_set_warnings_as_errors(sm_compile_options_t* opts, int v);
    SLANGMAKE_C_API void sm_compile_options_set_glsl_scalar_layout(sm_compile_options_t* opts, int v);
    SLANGMAKE_C_API void sm_compile_options_set_emit_spirv_directly(sm_compile_options_t* opts, int v);
    SLANGMAKE_C_API void sm_compile_options_set_dump_intermediates(sm_compile_options_t* opts, int v);
    SLANGMAKE_C_API void sm_compile_options_set_emit_reflection(sm_compile_options_t* opts, int v);
    SLANGMAKE_C_API void sm_compile_options_set_vulkan_version(sm_compile_options_t* opts, int v);
    SLANGMAKE_C_API void sm_compile_options_add_vulkan_bind_shift(sm_compile_options_t* opts, uint32_t kind,
                                                                  uint32_t space, uint32_t shift);
    SLANGMAKE_C_API void sm_compile_options_set_language_version(sm_compile_options_t* opts, const char* v);

    /* FNV-1a 64 fingerprint of every option field that affects bytecode
     * identity across permutations. Useful for cache invalidation. */
    SLANGMAKE_C_API uint64_t sm_compile_options_hash(const sm_compile_options_t* opts);

    /* ----- Permutation builder ------------------------------------------ */
    SLANGMAKE_C_API sm_permutation_t* sm_permutation_create(void);
    SLANGMAKE_C_API void              sm_permutation_destroy(sm_permutation_t* perm);

    SLANGMAKE_C_API void sm_permutation_add_constant(sm_permutation_t* perm, const char* name, const char* value);
    SLANGMAKE_C_API void sm_permutation_add_type_arg(sm_permutation_t* perm, const char* name, const char* value);

    /* Canonical alphabetical key. The returned pointer is invalidated by any
     * subsequent add_* call on the same permutation. */
    SLANGMAKE_C_API const char* sm_permutation_key(const sm_permutation_t* perm);

    SLANGMAKE_C_API size_t      sm_permutation_constant_count(const sm_permutation_t* perm);
    SLANGMAKE_C_API const char* sm_permutation_constant_name(const sm_permutation_t* perm, size_t idx);
    SLANGMAKE_C_API const char* sm_permutation_constant_value(const sm_permutation_t* perm, size_t idx);
    SLANGMAKE_C_API size_t      sm_permutation_type_arg_count(const sm_permutation_t* perm);
    SLANGMAKE_C_API const char* sm_permutation_type_arg_name(const sm_permutation_t* perm, size_t idx);
    SLANGMAKE_C_API const char* sm_permutation_type_arg_value(const sm_permutation_t* perm, size_t idx);

    /* ----- PermutationDefine list (parse / merge / expand) -------------- */
    SLANGMAKE_C_API sm_perm_define_list_t* sm_perm_define_list_create(void);
    SLANGMAKE_C_API void                   sm_perm_define_list_destroy(sm_perm_define_list_t* list);
    /* `values` is a pointer to `count` UTF-8 strings. */
    SLANGMAKE_C_API void sm_perm_define_list_push(sm_perm_define_list_t* list, const char* name,
                                                  sm_perm_define_kind_t kind, const char* const* values, size_t count);

    SLANGMAKE_C_API size_t                sm_perm_define_list_size(const sm_perm_define_list_t* list);
    SLANGMAKE_C_API const char*           sm_perm_define_list_name(const sm_perm_define_list_t* list, size_t i);
    SLANGMAKE_C_API sm_perm_define_kind_t sm_perm_define_list_kind(const sm_perm_define_list_t* list, size_t i);
    SLANGMAKE_C_API size_t                sm_perm_define_list_value_count(const sm_perm_define_list_t* list, size_t i);
    SLANGMAKE_C_API const char* sm_perm_define_list_value(const sm_perm_define_list_t* list, size_t i, size_t v);

    /* `len == 0` is treated as "find the NUL terminator". */
    SLANGMAKE_C_API sm_perm_define_list_t* sm_parse_permutations_source(const char* src, size_t len);
    SLANGMAKE_C_API sm_perm_define_list_t* sm_parse_permutations_file(const char* path);
    SLANGMAKE_C_API sm_perm_define_list_t* sm_merge_permutations(const sm_perm_define_list_t* file_defines,
                                                                 const sm_perm_define_list_t* cli_override);
    SLANGMAKE_C_API int                    sm_source_has_no_reflection(const char* src, size_t len);
    SLANGMAKE_C_API int                    sm_file_has_no_reflection(const char* path);

    /* ----- Cartesian-product expansion ---------------------------------- */
    SLANGMAKE_C_API sm_permutation_list_t* sm_expand_permutations(const sm_perm_define_list_t* defs);
    SLANGMAKE_C_API void                   sm_permutation_list_destroy(sm_permutation_list_t* list);
    SLANGMAKE_C_API size_t                 sm_permutation_list_size(const sm_permutation_list_t* list);
    /* Returned permutation is owned by the caller (clone). Free with sm_permutation_destroy. */
    SLANGMAKE_C_API sm_permutation_t* sm_permutation_list_clone_at(const sm_permutation_list_t* list, size_t i);

    /* ----- Compiler ------------------------------------------------------ */
    SLANGMAKE_C_API sm_compiler_t* sm_compiler_create(void);
    SLANGMAKE_C_API void           sm_compiler_destroy(sm_compiler_t* c);

    /* Always returns a result handle (or NULL on allocation failure). Use
     * sm_compile_result_success() to check whether compilation actually
     * succeeded; on failure, diagnostics are still populated. */
    SLANGMAKE_C_API sm_compile_result_t* sm_compiler_compile(const sm_compiler_t* c, const sm_compile_options_t* opts,
                                                             const sm_permutation_t* perm);

    SLANGMAKE_C_API void           sm_compile_result_destroy(sm_compile_result_t* r);
    SLANGMAKE_C_API int            sm_compile_result_success(const sm_compile_result_t* r);
    SLANGMAKE_C_API const uint8_t* sm_compile_result_code(const sm_compile_result_t* r, size_t* out_size);
    SLANGMAKE_C_API const uint8_t* sm_compile_result_reflection(const sm_compile_result_t* r, size_t* out_size);
    SLANGMAKE_C_API const char*    sm_compile_result_diagnostics(const sm_compile_result_t* r);
    SLANGMAKE_C_API size_t         sm_compile_result_dependency_count(const sm_compile_result_t* r);
    SLANGMAKE_C_API const char*    sm_compile_result_dependency(const sm_compile_result_t* r, size_t idx);

    /* ----- Blob writer --------------------------------------------------- */
    SLANGMAKE_C_API sm_blob_writer_t* sm_blob_writer_create(sm_target_t target);
    SLANGMAKE_C_API void              sm_blob_writer_destroy(sm_blob_writer_t* w);

    /* `dep_indices` may be NULL (count must then be 0). */
    SLANGMAKE_C_API void sm_blob_writer_add_entry(sm_blob_writer_t* w, const sm_permutation_t* perm,
                                                  const uint8_t* code, size_t code_size, const uint8_t* reflection,
                                                  size_t reflection_size, const uint32_t* dep_indices,
                                                  size_t dep_count);

    typedef struct sm_dep_info
    {
        const char* path;
        uint64_t    content_hash;
    } sm_dep_info_t;

    SLANGMAKE_C_API void sm_blob_writer_set_dependencies(sm_blob_writer_t* w, const sm_dep_info_t* deps, size_t count);
    SLANGMAKE_C_API void sm_blob_writer_set_options_hash(sm_blob_writer_t* w, uint64_t h);
    SLANGMAKE_C_API void sm_blob_writer_set_compression(sm_blob_writer_t* w, sm_codec_t c);
    SLANGMAKE_C_API size_t sm_blob_writer_entry_count(const sm_blob_writer_t* w);

    /* Caller owns the returned buffer; free with sm_buffer_destroy. */
    SLANGMAKE_C_API sm_buffer_t* sm_blob_writer_finalize(const sm_blob_writer_t* w);
    SLANGMAKE_C_API sm_status_t  sm_blob_writer_write_to_file(const sm_blob_writer_t* w, const char* path);

    /* ----- Blob reader --------------------------------------------------- */
    /* Caller must keep `data` alive for the reader's lifetime. */
    SLANGMAKE_C_API sm_blob_reader_t* sm_blob_reader_open_borrowed(const uint8_t* data, size_t size);
    /* Reader takes its own internal copy of `data`. */
    SLANGMAKE_C_API sm_blob_reader_t* sm_blob_reader_open_copy(const uint8_t* data, size_t size);
    SLANGMAKE_C_API sm_blob_reader_t* sm_blob_reader_open_file(const char* path);
    SLANGMAKE_C_API void              sm_blob_reader_destroy(sm_blob_reader_t* r);

    SLANGMAKE_C_API int         sm_blob_reader_valid(const sm_blob_reader_t* r);
    SLANGMAKE_C_API sm_target_t sm_blob_reader_target(const sm_blob_reader_t* r);
    SLANGMAKE_C_API uint64_t    sm_blob_reader_options_hash(const sm_blob_reader_t* r);
    SLANGMAKE_C_API sm_codec_t  sm_blob_reader_compression(const sm_blob_reader_t* r);
    SLANGMAKE_C_API size_t      sm_blob_reader_entry_count(const sm_blob_reader_t* r);

    typedef struct sm_blob_entry
    {
        const char*     key;
        size_t          key_size;
        const uint8_t*  code;
        size_t          code_size;
        const uint8_t*  reflection;
        size_t          reflection_size;
        const uint32_t* dep_indices;
        size_t          dep_index_count;
    } sm_blob_entry_t;

    SLANGMAKE_C_API sm_status_t sm_blob_reader_at(const sm_blob_reader_t* r, size_t idx, sm_blob_entry_t* out);
    SLANGMAKE_C_API sm_status_t sm_blob_reader_find(const sm_blob_reader_t* r, const sm_permutation_t* perm,
                                                    sm_blob_entry_t* out);

    SLANGMAKE_C_API size_t      sm_blob_reader_dependency_count(const sm_blob_reader_t* r);
    SLANGMAKE_C_API const char* sm_blob_reader_dependency_path(const sm_blob_reader_t* r, size_t idx);
    SLANGMAKE_C_API uint64_t    sm_blob_reader_dependency_hash(const sm_blob_reader_t* r, size_t idx);

    /* ----- Reflection: fmt:: POD mirrors --------------------------------- */
#pragma pack(push, 4)

    typedef struct sm_fmt_blob_header
    {
        uint32_t magic;
        uint32_t version;
        uint32_t target;
        uint32_t entry_count;
        uint32_t deps_offset;
        uint32_t deps_count;
        uint32_t deps_strings_offset;
        uint32_t deps_strings_size;
        uint32_t entry_deps_idx_offset;
        uint32_t entry_deps_idx_count;
        uint32_t compression;
        uint32_t uncompressed_payload_size;
        uint64_t options_hash;
    } sm_fmt_blob_header_t;

    typedef struct sm_fmt_dep_entry
    {
        uint32_t path_offset;
        uint32_t path_size;
        uint64_t content_hash;
    } sm_fmt_dep_entry_t;

    typedef struct sm_fmt_entry_record
    {
        uint32_t key_offset;
        uint32_t key_size;
        uint32_t code_offset;
        uint32_t code_size;
        uint32_t refl_offset;
        uint32_t refl_size;
        uint32_t deps_idx_off;
        uint32_t deps_idx_count;
    } sm_fmt_entry_record_t;

    typedef struct sm_fmt_hashed_str
    {
        uint32_t str_idx;
        uint32_t hash;
    } sm_fmt_hashed_str_t;

    typedef struct sm_fmt_attr_arg
    {
        uint32_t kind; /* 0=None, 1=Int, 2=Float, 3=String */
        uint32_t str_idx;
        uint64_t raw;
    } sm_fmt_attr_arg_t;

    typedef struct sm_fmt_attribute
    {
        uint32_t name_str_idx;
        uint32_t arg_off;
        uint32_t arg_count;
    } sm_fmt_attribute_t;

    typedef struct sm_fmt_type
    {
        uint32_t kind;
        uint32_t name_str_idx;
        uint32_t full_name_str_idx;
        uint32_t scalar_kind;
        uint32_t field_count;
        uint32_t field_pool_off;
        uint32_t row_count;
        uint32_t col_count;
        uint32_t element_type_idx;
        uint32_t element_count;
        uint32_t total_array_element_count;
        uint32_t resource_shape;
        uint32_t resource_access;
        uint32_t resource_result_type_idx;
    } sm_fmt_type_t;

    typedef struct sm_fmt_type_layout
    {
        uint32_t type_idx;
        uint32_t kind;
        uint32_t parameter_category;
        uint32_t category_count;
        uint32_t category_pool_off;
        uint32_t field_count;
        uint32_t field_layout_pool_off;
        uint32_t container_var_layout_idx;
        uint32_t element_type_layout_idx;
        uint32_t element_var_layout_idx;
        uint32_t explicit_counter_var_layout_idx;
        uint32_t matrix_layout_mode;
        uint32_t generic_param_index;
        uint32_t size_pool_off;
        uint32_t binding_range_count;
        uint32_t binding_range_off;
        uint32_t descriptor_set_count;
        uint32_t descriptor_set_off;
        uint32_t sub_object_range_count;
        uint32_t sub_object_range_off;
    } sm_fmt_type_layout_t;

    typedef struct sm_fmt_binding_range
    {
        uint32_t binding_type;
        uint32_t binding_count;
        uint32_t leaf_type_layout_idx;
        uint32_t leaf_variable_idx;
        uint32_t image_format;
        uint32_t is_specializable;
        uint32_t descriptor_set_index;
        uint32_t first_descriptor_range_index;
        uint32_t descriptor_range_count;
    } sm_fmt_binding_range_t;

    typedef struct sm_fmt_descriptor_set
    {
        uint32_t space_offset;
        uint32_t descriptor_range_start;
        uint32_t descriptor_range_count;
        uint32_t pad;
    } sm_fmt_descriptor_set_t;

    typedef struct sm_fmt_descriptor_range
    {
        uint32_t index_offset;
        uint32_t descriptor_count;
        uint32_t binding_type;
        uint32_t parameter_category;
    } sm_fmt_descriptor_range_t;

    typedef struct sm_fmt_sub_object_range
    {
        uint32_t binding_range_index;
        uint32_t space_offset;
        uint32_t offset_var_layout_idx;
    } sm_fmt_sub_object_range_t;

    typedef struct sm_fmt_variable
    {
        uint32_t name_str_idx;
        uint32_t type_idx;
        uint32_t modifier_off;
        uint32_t modifier_count;
        uint32_t attr_off;
        uint32_t attr_count;
        uint32_t has_default;
        uint32_t default_kind;
        uint64_t default_value_raw;
        uint32_t generic_container_idx;
    } sm_fmt_variable_t;

    typedef struct sm_fmt_var_layout
    {
        uint32_t var_idx;
        uint32_t type_layout_idx;
        uint32_t category;
        uint32_t category_count;
        uint32_t category_pool_off;
        uint32_t offset_pool_off;
        uint32_t binding_index;
        uint32_t binding_space_pool_off;
        uint32_t image_format;
        uint32_t semantic_name_str_idx;
        uint32_t semantic_index;
        uint32_t stage;
        uint32_t modifier_off;
        uint32_t modifier_count;
        uint32_t attr_off;
        uint32_t attr_count;
    } sm_fmt_var_layout_t;

    typedef struct sm_fmt_function
    {
        uint32_t name_str_idx;
        uint32_t return_type_idx;
        uint32_t param_var_off;
        uint32_t param_var_count;
        uint32_t modifier_off;
        uint32_t modifier_count;
        uint32_t attr_off;
        uint32_t attr_count;
        uint32_t generic_container_idx;
    } sm_fmt_function_t;

    typedef struct sm_fmt_generic
    {
        uint32_t name_str_idx;
        uint32_t inner_decl_idx;
        uint32_t inner_kind;
        uint32_t type_param_off;
        uint32_t type_param_count;
        uint32_t value_param_off;
        uint32_t value_param_count;
        uint32_t constraint_pool_off;
        uint32_t constraint_count;
        uint32_t outer_generic_idx;
    } sm_fmt_generic_t;

    typedef struct sm_fmt_decl
    {
        uint32_t kind;
        uint32_t name_str_idx;
        uint32_t parent_decl_idx;
        uint32_t child_off;
        uint32_t child_count;
        uint32_t payload_idx;
    } sm_fmt_decl_t;

    typedef struct sm_fmt_entry_point
    {
        uint32_t name_str_idx;
        uint32_t name_override_str_idx;
        uint32_t stage;
        uint32_t thread_group_size_x;
        uint32_t thread_group_size_y;
        uint32_t thread_group_size_z;
        uint32_t wave_size;
        uint32_t uses_any_sample_rate_input;
        uint32_t has_default_constant_buffer;
        uint32_t var_layout_idx;
        uint32_t type_layout_idx;
        uint32_t result_var_layout_idx;
        uint32_t function_idx;
        uint32_t param_var_layout_off;
        uint32_t param_var_layout_count;
        uint32_t attr_off;
        uint32_t attr_count;
        uint32_t hash_low;
        uint32_t hash_high;
    } sm_fmt_entry_point_t;

#pragma pack(pop)

    /* ----- ReflectionView ------------------------------------------------ */
    SLANGMAKE_C_API sm_reflection_t* sm_reflection_open(const uint8_t* data, size_t size);
    SLANGMAKE_C_API void             sm_reflection_destroy(sm_reflection_t* r);
    SLANGMAKE_C_API int              sm_reflection_valid(const sm_reflection_t* r);

    SLANGMAKE_C_API uint64_t sm_reflection_first_entry_point_hash(const sm_reflection_t* r);
    SLANGMAKE_C_API uint32_t sm_reflection_global_cb_binding(const sm_reflection_t* r);
    SLANGMAKE_C_API uint32_t sm_reflection_global_cb_size(const sm_reflection_t* r);
    SLANGMAKE_C_API uint32_t sm_reflection_bindless_space(const sm_reflection_t* r);
    /* Returns SM_INVALID_INDEX when there is no global params layout. */
    SLANGMAKE_C_API uint32_t sm_reflection_global_params_var_layout(const sm_reflection_t* r);

    /* String pool resolution. Returns "" for SM_INVALID_INDEX or out-of-range. */
    SLANGMAKE_C_API const char* sm_reflection_string(const sm_reflection_t* r, uint32_t str_idx);
    SLANGMAKE_C_API const char* sm_reflection_hashed_string(const sm_reflection_t* r, uint32_t index);

    /* Raw POD table accessors. Pointers and counts reflect the on-disk layout
     * directly; alignment is 4-byte everywhere because of #pragma pack(push, 4). */
    SLANGMAKE_C_API const sm_fmt_type_t*          sm_reflection_types(const sm_reflection_t* r, size_t* count);
    SLANGMAKE_C_API const sm_fmt_type_layout_t*   sm_reflection_type_layouts(const sm_reflection_t* r, size_t* count);
    SLANGMAKE_C_API const sm_fmt_variable_t*      sm_reflection_variables(const sm_reflection_t* r, size_t* count);
    SLANGMAKE_C_API const sm_fmt_var_layout_t*    sm_reflection_var_layouts(const sm_reflection_t* r, size_t* count);
    SLANGMAKE_C_API const sm_fmt_function_t*      sm_reflection_functions(const sm_reflection_t* r, size_t* count);
    SLANGMAKE_C_API const sm_fmt_generic_t*       sm_reflection_generics(const sm_reflection_t* r, size_t* count);
    SLANGMAKE_C_API const sm_fmt_decl_t*          sm_reflection_decls(const sm_reflection_t* r, size_t* count);
    SLANGMAKE_C_API const sm_fmt_entry_point_t*   sm_reflection_entry_points(const sm_reflection_t* r, size_t* count);
    SLANGMAKE_C_API const sm_fmt_attribute_t*     sm_reflection_attributes(const sm_reflection_t* r, size_t* count);
    SLANGMAKE_C_API const sm_fmt_attr_arg_t*      sm_reflection_attribute_args(const sm_reflection_t* r, size_t* count);
    SLANGMAKE_C_API const uint32_t*               sm_reflection_modifier_pool(const sm_reflection_t* r, size_t* count);
    SLANGMAKE_C_API const sm_fmt_hashed_str_t*    sm_reflection_hashed_strings(const sm_reflection_t* r, size_t* count);
    SLANGMAKE_C_API const sm_fmt_binding_range_t* sm_reflection_binding_ranges(const sm_reflection_t* r, size_t* count);
    SLANGMAKE_C_API const sm_fmt_descriptor_set_t*   sm_reflection_descriptor_sets(const sm_reflection_t* r,
                                                                                   size_t*                count);
    SLANGMAKE_C_API const sm_fmt_descriptor_range_t* sm_reflection_descriptor_ranges(const sm_reflection_t* r,
                                                                                     size_t*                count);
    SLANGMAKE_C_API const sm_fmt_sub_object_range_t* sm_reflection_sub_object_ranges(const sm_reflection_t* r,
                                                                                     size_t*                count);
    SLANGMAKE_C_API const uint32_t*                  sm_reflection_u32_pool(const sm_reflection_t* r, size_t* count);

    /* Walking the reflection tree to compute (set, slot, byte-offset) tuples
     * is the RHI's job — slangmake exposes the raw tables above and stops
     * there. Consumers build their own bind-path representation on top. */

    /* ----- BatchCompiler ------------------------------------------------- */
    SLANGMAKE_C_API sm_batch_compiler_t* sm_batch_compiler_create(sm_compiler_t* compiler);
    SLANGMAKE_C_API void                 sm_batch_compiler_destroy(sm_batch_compiler_t* b);

    SLANGMAKE_C_API void sm_batch_compiler_set_keep_going(sm_batch_compiler_t* b, int v);
    SLANGMAKE_C_API void sm_batch_compiler_set_verbose(sm_batch_compiler_t* b, int v);
    SLANGMAKE_C_API void sm_batch_compiler_set_quiet(sm_batch_compiler_t* b, int v);
    SLANGMAKE_C_API void sm_batch_compiler_set_jobs(sm_batch_compiler_t* b, int n);
    SLANGMAKE_C_API void sm_batch_compiler_set_incremental(sm_batch_compiler_t* b, int v);
    SLANGMAKE_C_API void sm_batch_compiler_set_compression(sm_batch_compiler_t* b, sm_codec_t c);

    SLANGMAKE_C_API size_t sm_batch_compiler_last_reused(const sm_batch_compiler_t* b);
    SLANGMAKE_C_API size_t sm_batch_compiler_last_compiled(const sm_batch_compiler_t* b);

    /* `cli_override` may be NULL. Always returns a non-NULL output handle on
     * allocation success; check failures via sm_batch_output_failure_count. */
    SLANGMAKE_C_API sm_batch_output_t* sm_batch_compile_file(sm_batch_compiler_t* b, const char* file,
                                                             const sm_compile_options_t*  opts,
                                                             const sm_perm_define_list_t* cli_override,
                                                             const char*                  output_path);

    SLANGMAKE_C_API void           sm_batch_output_destroy(sm_batch_output_t* o);
    SLANGMAKE_C_API const char*    sm_batch_output_path(const sm_batch_output_t* o);
    SLANGMAKE_C_API const uint8_t* sm_batch_output_blob(const sm_batch_output_t* o, size_t* out_size);
    SLANGMAKE_C_API size_t         sm_batch_output_compiled_count(const sm_batch_output_t* o);
    SLANGMAKE_C_API const char*    sm_batch_output_compiled_key(const sm_batch_output_t* o, size_t i);
    /* Caller owns the clone; free with sm_permutation_destroy. */
    SLANGMAKE_C_API sm_permutation_t* sm_batch_output_compiled_clone(const sm_batch_output_t* o, size_t i);
    SLANGMAKE_C_API size_t            sm_batch_output_failure_count(const sm_batch_output_t* o);
    SLANGMAKE_C_API const char*       sm_batch_output_failure(const sm_batch_output_t* o, size_t i);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SLANGMAKE_C_H */
